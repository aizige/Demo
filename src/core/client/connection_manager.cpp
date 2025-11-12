//
// Created by Aiziboy on 2025/7/18.
//

#include "connection_manager.hpp"

#include <ranges>
#include <spdlog/spdlog.h>

#include "http_connection.hpp"
#include "https_connection.hpp"
#include "h2_connection.hpp"
#include "iconnection.hpp"
#include "utils/utils.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/beast/ssl.hpp>

#include "error/aizix_error.hpp"

// 命名空间和类型别名
using namespace std::literals::chrono_literals;
using results_type = boost::asio::ip::tcp::resolver::results_type;

/**
 * @brief ConnectionManager 的构造函数。
 *        负责初始化 SSL 上下文、DNS 解析器，并根据配置启动后台维护任务。
 */
ConnectionManager::ConnectionManager(boost::asio::io_context& ioc, const AizixConfig& config)
    : max_redirects(config.client.max_redirects),
      ioc_(ioc),
      strand_(ioc.get_executor()),
      ssl_ctx_(boost::asio::ssl::context::tls_client),
      resolver_(ioc),
      connect_timeout_ms_(config.client.connect_timeout_ms),
      http2_max_concurrent_streams_(config.client.http2_max_concurrent_streams),
      maintenance_timer_(ioc),
      maintenance_interval_ms_(config.client.maintenance_interval_ms),
      idle_timeout_for_close_ms_(config.client.idle_timeout_for_close_ms),
      idle_timeout_for_ping_ms_(config.client.idle_timeout_for_ping_ms) {
    // 配置 SSL/TLS 上下文，增强安全性。
    ssl_ctx_.set_options(network::ssl::CONTEXT_OPTIONS);

    // 设置推荐的现代加密套件列表，以提高安全性。
    if (SSL_CTX_set_cipher_list(ssl_ctx_.native_handle(), network::ssl::CIPHER_SUITES) != 1) {
        // 如果设置失败，可以记录一个日志，但不中断程序
        SPDLOG_WARN("Could not set SSL cipher list. Using OpenSSL defaults");
    }

    // ---配置证书验证 ---
    // 加载系统默认的根证书
    ssl_ctx_.set_default_verify_paths();

    // 验证对端证书
    // - verify_none 为不验证证书
    // 对于客户端：验证服务器证书是否由受信任的 CA 签发
    // 对于服务器：如果你还调用了 "set_verify_callback()" 并启用了 "verify_peer" ，可以验证客户端证书（用于双向认证）
    ssl_ctx_.set_verify_mode(config.client.ssl_verify ? boost::asio::ssl::verify_peer : boost::asio::ssl::verify_none);


    // 设置客户端 ALPN (Application-Layer Protocol Negotiation) 协议列表。
    // "h2" 优先于 "http/1.1"，让服务器在 TLS 握手期间选择最优协议。
    // 3. 确定并打印服务器将要提供的协议列表
    const unsigned char* client_protos;
    size_t client_protos_len;
    if (config.client.http2_enabled) {
        client_protos = network::alpn::PROTOS_H2_PREFERRED;
        client_protos_len = sizeof(network::alpn::PROTOS_H2_PREFERRED);
    } else {
        client_protos = network::alpn::PROTOS_H1_ONLY;
        client_protos_len = sizeof(network::alpn::PROTOS_H1_ONLY);
    }

    SSL_CTX_set_alpn_protos(ssl_ctx_.native_handle(), client_protos, client_protos_len);

    // 启动后台维护任务
    start_maintenance();
}

/**
 * @brief 析构函数。确保后台任务被停止。
 */
ConnectionManager::~ConnectionManager() {
    stop_internal();
}

/**
 * @brief 异步地获取一个到指定目标的健康连接，包含完整的连接池管理和背压逻辑。
 *
 * 流程：
 * 1. 切换到 `strand_` 保证线程安全。
 * 2. 尝试从 H2 或 H1.1 池中复用一个现有连接。
 * 3. 如果无法复用，检查是否达到主机连接数上限。如果达到，则进入等待队列（背压）。
 * 4. 如果可以创建，检查是否已有其他协程正在创建。如果是，则等待其完成（惊群效应处理）。
 * 5. 如果是第一个创建者，则启动一个后台协程来创建新连接。
 * 6. 异步等待创建结果，并返回 `PooledConnection`。
 */
boost::asio::awaitable<PooledConnection> ConnectionManager::get_connection(const std::string_view scheme, const std::string_view host, uint16_t port) {
    // 构造用于标识连接池的唯一 Key。
    const std::string key = std::string(scheme) + "//" + std::string(host) + ":" + std::to_string(port);
    SPDLOG_DEBUG("获取连接 [{}]", key);

    // --- 进入 strand 以保证对所有共享状态的访问都是线程安全的 ---
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);

    // --- 阶段 1: 尝试从池中复用现有连接 ---

    // 策略 1: 检查 H2 连接列表，寻找负载最低的可用连接。
    if (auto it = h2_pool_.find(key); it != h2_pool_.end()) {
        auto& h2_conns = it->second;
        std::shared_ptr<IConnection> best_conn = nullptr;
        size_t min_streams = SIZE_MAX;

        for (const auto& conn : h2_conns) {
            if (conn->is_usable()) {
                const size_t current_streams = conn->get_active_streams();
                if (current_streams < conn->get_max_concurrent_streams() && current_streams < min_streams) {
                    best_conn = conn;
                    min_streams = current_streams;
                }
            }
        }

        // 清理失效连接
        std::erase_if(h2_conns, [](const auto& conn) { return !conn->is_usable(); });

        if (best_conn) {
            SPDLOG_DEBUG("复用H2连接 [{}]-[{}] (活动流 {} 个)", best_conn->id(), best_conn->get_pool_key(), min_streams);
            co_return PooledConnection{best_conn, true};
        }
        SPDLOG_DEBUG("H2连接池里没有可复用连接 [{}]", key);
    }

    // 策略 2: 检查 H1.1 连接池，取出一个空闲连接。
    if (auto it = pool_.find(key); it != pool_.end()) {
        auto& h1_queue = it->second;
        // 逐个取出，跳过不可用或非空闲的，顺便丢弃失效连接
        while (!h1_queue.empty()) {
            const auto conn = std::move(h1_queue.front());
            h1_queue.pop_front();
            if (!conn || !conn->is_usable()) {
                continue; // 丢弃失效
            }
            if (conn->get_active_streams() == 0) {
                SPDLOG_DEBUG("复用H1连接 [{}]-[{}]", conn->id(), conn->get_pool_key());
                co_return PooledConnection{conn, true};
            } else {
                // todo: 理论上H1连接不可能取到active_streams > 0 的连接。非空闲丢弃？？ 还是保留？？
            }
        }
        SPDLOG_DEBUG("http/1.1连接池里没有可复用连接 [{}]", key);
    }

    // --- 阶段 2: 并发创建控制（惊群效应处理） ---
    auto it = creation_in_progress_.find(key);
    if (it == creation_in_progress_.end()) {
        // 没有创建者，当前协程成为“创建者”
        SPDLOG_DEBUG("连接池里没有连接 '{}'，发起新的创建任务。", key);

        // 注意：将占位的 executor 绑定到 ioc_，避免 channel 等待阻塞 strand_
        auto new_creation = std::make_shared<CreationInProgress>(ioc_.get_executor());
        it = creation_in_progress_.emplace(key, new_creation).first;

        // 启动一个分离的任务来执行创建工作
        boost::asio::co_spawn(
            ioc_, // 在 ioc_ 的上下文中执行网络操作
            [self = shared_from_this(),
                key,
                state = new_creation,
                scheme_s = std::string(scheme),
                host_s = std::string(host),
                port]() -> boost::asio::awaitable<void> {
                std::exception_ptr exception = nullptr;
                std::shared_ptr<IConnection> new_conn = nullptr;

                try {
                    new_conn = co_await self->create_new_connection(key, scheme_s, host_s, port);
                } catch (...) {
                    exception = std::current_exception();
                }

                // 回到 strand 来“设置结果、关闭信号、移除占位”三件事在同一序列中完成，避免竞态
                co_await boost::asio::post(self->strand_, boost::asio::use_awaitable);
                try {
                    if (exception) {
                        state->result = exception;
                    } else {
                        // add_connection_to_pool 也可能抛异常，必须捕获并写入 result
                        try {
                            self->add_connection_to_pool(key, new_conn);
                            state->result = new_conn;
                        } catch (...) {
                            state->result = std::current_exception();
                        }
                    }
                } catch (...) {
                    // 极端情况下如果设置 result 自身抛异常，至少确保有异常结果
                    state->result = std::current_exception();
                }

                // 广播完成信号（先设置 result 再 close）
                state->signal->close();

                // 最后移除“创建占位”
                self->creation_in_progress_.erase(key);
                SPDLOG_DEBUG("创建新连接 '{}' 的创建占位符已被移除。", key);

                co_return;
            },
            boost::asio::detached);
    }

    // --- 阶段 3: 等待结果（创建者和等待者共用） ---
    const auto state_to_wait_on = it->second;
    SPDLOG_DEBUG("等待host '{}' 的连接创建完成...", key);

    // *** 关键修复 ***
    // 在等待之前，我们必须先离开 strand。
    // 使用 co_spawn 一个新的临时协程来做等待工作，
    // 这样 get_connection 当前的 strand 上下文就可以结束了。
    co_return co_await [](
        // 捕获必要的变量
        auto self,
        auto state_to_wait_on,
        const std::string& key
    ) -> boost::asio::awaitable<PooledConnection> {
            // 这个新的协程默认在 ioc_ 的通用执行器上运行，不在 strand 上。

            // a. 等待 channel 关闭（广播信号）。使用 as_tuple 避免异常。
            co_await state_to_wait_on->signal->async_receive(
                boost::asio::as_tuple(boost::asio::use_awaitable));

            // b. 等待完成后，回到 strand，安全地读取结果。
            co_await boost::asio::post(self->strand_, boost::asio::use_awaitable);

            // c. 检查最终的创建结果。
            if (auto* conn_ptr = std::get_if<std::shared_ptr<IConnection>>(&state_to_wait_on->result)) {
                // 成功（这个连接刚创建，非复用）
                co_return PooledConnection{*conn_ptr, false};
            } else if (auto* ex_ptr = std::get_if<std::exception_ptr>(&state_to_wait_on->result)) {
                // 失败：抛出创建中的异常
                std::rethrow_exception(*ex_ptr);
            } else {
                // 逻辑错误，不应该发生（防御性检查）
                throw std::logic_error("连接创建已完成，但未设置结果。");
            }
        }(this, state_to_wait_on, key); // 立即调用 lambda
}

/**
 * @brief 将一个使用完毕的连接释放回池中，并可能唤醒一个等待者
 *
 * 如果连接仍然可用 (`is_usable()`)，它会被放回池中以供后续请求复用。
 * 如果连接已损坏或被标记为不可用，它将被安全地丢弃。
 * 此方法是线程安全的。
 *
 * @note 这是一个非阻塞操作，它会将实际工作 post 到 `strand_` 上执行。
 * @param conn 要释放的连接的共享指针。
 */
void ConnectionManager::release_connection(const std::shared_ptr<IConnection>& conn) {
    // 同样，将释放操作调度到 strand 上
    if (!conn) { return; }

    // 将操作 post 到 strand 上以保证线程安全。
    post(strand_, [self = shared_from_this(), conn, this]() {
        const auto& key = conn->get_pool_key();
        const bool usable = conn->is_usable();


        // H2 连接是多路复用的，不“归还”，只检查是否需要因失效而被干掉。
        if (conn->supports_multiplexing()) {
            if (!usable) {
                SPDLOG_DEBUG("丢弃不可用的H2连接｛｝。", conn->id());
                // 从 H2 列表中移除
                if (const auto it = self->h2_pool_.find(conn->get_pool_key()); it != self->h2_pool_.end()) {
                    std::erase(it->second, conn);
                    // 如果 vector 变为空，选择删除整个 key
                    if (it->second.empty()) {
                        h2_pool_.erase(it);
                    }
                }
            }
            return; // H2 连接不归还，它一直在“池”里
        } else {
            // H1.1 连接：可用的归还池中，不可用的丢弃。
            SPDLOG_DEBUG("我来归还H1连接 [{}] {}", conn->id(), conn->get_pool_key());
            if (usable) {
                //  H1 的归还池中...
                self->pool_[key].push_back(conn); // 放回队尾，实现 LIFO/FIFO 策略
                SPDLOG_DEBUG("将连接 [{}] {}存入连接池,当前连接 [{}] 的连接池大小: {}", conn->id(), key, pool_[key].back()->get_pool_key(), pool_[key].size());
                return;
            }
            SPDLOG_DEBUG("[{}] {} 这个H1连接已经断开了不用归还直接丢弃 ", conn->id(), conn->get_pool_key());
            // 这里丢弃了不可用的H1连接...
        }
    });
}

/**
 * @brief 异步地关闭所有连接池中的连接并停止后台任务。
 *        这是优雅停机流程的核心部分。
 */
boost::asio::awaitable<void> ConnectionManager::stop() {
    // 1. 切换到 strand，以保证对所有内部状态的访问都是线程安全的。
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);

    // 2. 检查并设置 stopped_ 标志，确保关闭逻辑只执行一次。
    if (stopped_) co_return;
    stopped_ = true;

    // 3. 立即取消后台维护计时器，防止新的维护任务启动。
    maintenance_timer_.cancel();

    SPDLOG_INFO("关闭连接池里的所有连接...");

    // 4. 收集所有池中的连接到一个列表中，准备并行关闭。
    std::vector<std::shared_ptr<IConnection>> all_conns_to_close;

    for (const auto& queue : pool_ | std::views::values) {
        all_conns_to_close.insert(all_conns_to_close.end(), queue.begin(), queue.end());
    }
    pool_.clear(); // 清空池

    for (const auto& vec : h2_pool_ | std::views::values) {
        all_conns_to_close.insert(all_conns_to_close.end(), vec.begin(), vec.end());
    }
    h2_pool_.clear(); // 清空池

    if (all_conns_to_close.empty()) {
        SPDLOG_DEBUG("连接池里没有要关闭的连接");
        co_return;
    }

    SPDLOG_DEBUG("正在并发关闭 {} 个连接...", all_conns_to_close.size());

    // 5.使用 co_spawn + channel 实现健壮的并行等待

    auto ex = co_await boost::asio::this_coro::executor;
    //  创建一个 channel 的【共享指针】。channel 本身不可拷贝，但它的 shared_ptr 可以。
    //    channel 用于从关闭任务中接收“完成”信号。
    auto completion_channel = std::make_shared<boost::asio::experimental::channel<void(boost::system::error_code)>>(ex, all_conns_to_close.size());

    // 6. 为每个连接 co_spawn 一个独立的关闭协程。
    for (auto& conn : all_conns_to_close) {
        boost::asio::co_spawn(
            ioc_, // 在 io_context 的通用执行器上运行，以实现真正的并行 I/O。

            // a. lambda 捕获连接的 shared_ptr 和 channel 的 shared_ptr。
            [conn, channel_ptr = completion_channel]() -> boost::asio::awaitable<void> {
                try {
                    // b. 每个协程只负责关闭一个连接。
                    co_await conn->close();
                } catch (const std::exception& e) {
                    // 忽略关闭时可能发生的任何异常，以确保关机流程不会中断。
                    SPDLOG_WARN("[{}]-[{}] 关闭时出现异常：{}", conn->id(), conn->get_pool_key(), e.what());
                }

                // c. 任务完成，向 channel 发送一个信号。
                //    我们通过共享指针调用 async_send，这是非 const 操作，但指针本身是 const 的，所以没问题。
                boost::system::error_code ignored_ec;
                co_await channel_ptr->async_send(ignored_ec, boost::asio::use_awaitable);
            },

            boost::asio::detached // 分离协程，我们在这里不等待它，而是通过 channel 等待。
        );
    }

    // 7. 在主 stop() 协程中，循环等待，直到收到所有任务的完成信号。
    SPDLOG_DEBUG("等待 {} 个关闭任务完成...", all_conns_to_close.size());
    for (size_t i = 0; i < all_conns_to_close.size(); ++i) {
        // 每次 async_receive 都会挂起，直到一个关闭任务完成并发送信号。
        co_await completion_channel->async_receive(boost::asio::use_awaitable);
    }

    SPDLOG_DEBUG("所有连接已成功关闭");
}

/**
 * @brief [私有] 将一个新创建的连接添加到相应的池中。
 */
void ConnectionManager::add_connection_to_pool(const std::string& key, const std::shared_ptr<IConnection>& conn) {
    if (conn->supports_multiplexing()) {
        h2_pool_[key].push_back(conn);
        SPDLOG_DEBUG("将 {} 添加到h2_pool中，当前连接数量{}", key, h2_pool_[key].size());
    } else {
        //pool_[key].push_back(conn);
        //SPDLOG_DEBUG("将 {} 添加到h1_pool中，当前连接数量{}", key, pool_[key].size());
    }
}

/**
 * @brief 创建一个新连接的完整协程。内置了总的连接超时
 *
 * 这个协程封装了建立一个出站网络连接所需的所有步骤，包括：
 * 1. 异步 DNS 解析，将主机名转换为 IP 地址列表。
 * 2. 根据协议 (`http` 或 `https`) 创建相应的连接类型。
 * 3. 对于 `https`，它会执行 TCP 连接、TLS 握手和 ALPN 协议协商。
 * 4. 根据 ALPN 的结果，最终决定是创建 HTTP/2 连接还是回退到 HTTP/1.1。
 *
 * @note 此协程通常在 ConnectionManager 的 `strand_` 上下文中被启动，所以内部是安全的
 *       但其内部的耗时网络 I/O 操作是在通用的 `io_context` 执行器上运行的。
 *
 * @param key 用于标识连接池的唯一键。
 * @param scheme 协议类型 ("http:" 或 "https:")。
 * @param host 目标主机名。
 * @param port 目标端口号。
 * @return 一个协程句柄，其结果是成功创建的 `IConnection` 对象的共享指针。
 * @throws boost::system::system_error 如果在任何网络I/O阶段发生错误。
 * @throws std::runtime_error 如果协议不被支持。
 */
boost::asio::awaitable<std::shared_ptr<IConnection>> ConnectionManager::create_new_connection(const std::string& key, const std::string_view scheme, std::string_view host, const uint16_t port) {
    // 这个函数总是被 get_connection 在 strand 上调用，所以内部是安全的

    // 直接在 ioc_ 上创建定时器
    boost::asio::steady_timer timer(ioc_);

    // 1. 设置一个总的连接超时
    //    这涵盖了 DNS、TCP 和 TLS 握手的总和时间。
    timer.expires_after(std::chrono::milliseconds(connect_timeout_ms_));
    try {
        // 2. 使用 `||` 操作符，让连接创建逻辑与超时定时器进行“竞赛”。
        auto result = co_await ([&]() -> boost::asio::awaitable<std::shared_ptr<IConnection>> {
                // --- 操作A: 完整的连接创建逻辑，被封装在 lambda 协程中 ---

                // 3. 异步 DNS 解析。
                boost::system::error_code ec;
                const results_type endpoints = co_await resolver_.async_resolve(tcp::v4(), host, std::to_string(port), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                if (ec) throw boost::system::system_error(ec, "DNS resolution failed");

                // 4. 创建连接
                if (scheme == "https:") {
                    // a. 创建 HTTP/1.1 socket连接。
                    tcp::socket socket(ioc_);

                    // b. 用 socket 创建 ssl_stream
                    auto stream = std::make_shared<boost::beast::ssl_stream<tcp::socket>>(std::move(socket), ssl_ctx_);

                    // c. 建立 TCP 连接，底层 socket 是 stream->next_layer()
                    ec.clear();
                    co_await async_connect(stream->next_layer(), endpoints, boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                    if (ec) throw boost::system::system_error(ec, "TCP connect failed for SSL");

                    // d. 设置 SNI (Server Name Indication)。
                    //    这在 TLS 握手中至关重要，它告诉服务器我们想要访问哪个主机。
                    //    缺少 SNI 会导致使用多域名证书的服务器返回错误的证书。
                    if (!SSL_set_tlsext_host_name(stream->native_handle(), std::string(host).c_str())) {
                        throw boost::system::system_error(static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category());
                    }

                    // e. 执行 TLS 握手。
                    ec.clear();
                    co_await stream->async_handshake(boost::asio::ssl::stream_base::client, boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                    if (ec) throw boost::system::system_error(ec, "TLS handshake failed");

                    // f. 检查 ALPN 协商结果并返回对应连接。
                    const unsigned char* proto = nullptr;
                    unsigned int len = 0;
                    SSL_get0_alpn_selected(stream->native_handle(), &proto, &len);
                    if (proto && std::string_view(reinterpret_cast<const char*>(proto), len) == "h2") {
                        // 协商成功，创建 H2 连接。
                        SPDLOG_DEBUG("ALPN为 {} 选择了HTTP/2协议", host);
                        auto conn = Http2Connection::create(stream, key, http2_max_concurrent_streams_, idle_timeout_for_close_ms_);

                        // 等待 H2 握手完成。
                        co_await conn->run();

                        co_return conn;
                    } else {
                        // 协商失败或不支持，回退到 HTTPS/1.1。
                        SPDLOG_DEBUG("ALPN selected HTTP/1.1 for {}.", host);
                        co_return std::make_shared<HttpsConnection>(stream, key);
                    }
                }
                if (scheme == "http:") {
                    // 创建纯文本 HTTP/1.1 连接。
                    ec.clear();
                    tcp::socket socket(ioc_);

                    co_await async_connect(socket, endpoints.begin(), endpoints.end(), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                    if (ec) { throw boost::system::system_error(ec, "TCP connect failed"); }

                    co_return std::make_shared<HttpConnection>(std::move(socket), key);
                }
                // 确保所有路径都有出口
                throw std::runtime_error("未知协议Http: '" + std::string(scheme) + "'. 仅支持 'http:' and 'https:' 协议");
            }() // <<-- 立即调用 lambda 以创建 awaitable
            ||
            // --- 操作B: 超时定时器 ---
            timer.async_wait(boost::asio::use_awaitable));

        // 5. 检查是否建立连接超时了
        if (result.index() == 1) {
            // .index() == 1 表示定时器获胜
            // 超时发生，抛出明确的超时异常。
            // 这会中断 get_connection 的流程，并将错误传递给最终的调用者。
           // throw boost::system::system_error(boost::asio::error::timed_out, "创建 '" + std::string(scheme) + "' 连接超时");
           throw boost::system::system_error(aizix_error::network::connection_timeout);
        }

        // 如果能到这里，说明是连接创建操作 (index 0) 获胜。
        // 必须取消仍在后台等待的定时器，以清理资源。
        timer.cancel();

        // 6. 从 variant 中获取连接创建的结果（一个 shared_ptr）并返回。
        co_return std::get<0>(result);
    } catch (const boost::system::system_error& e) {
        SPDLOG_ERROR("Failed to create new connection to {}: {}", key, e.what());
        const std::string new_message = "Connection failed for key '" + key + "'. Underlying cause: " + e.what();
        throw boost::system::system_error(aizix_error::network::connection_error, new_message);
    }
}

/**
 * @brief [私有] 启动后台维护任务。
 */
void ConnectionManager::start_maintenance() {
    // 启动后台协程，它将独立运行
    boost::asio::co_spawn(ioc_, run_maintenance(), boost::asio::detached);
}

/**
 * @brief [私有] 后台维护任务的协程实现。
 */
boost::asio::awaitable<void> ConnectionManager::run_maintenance() {
    while (!stopped_) {
        // 1. 设置下一次维护的定时器
        maintenance_timer_.expires_after(std::chrono::milliseconds(maintenance_interval_ms_));
        boost::system::error_code ec;


        // 2. 挂起协程，等待定时器到期或被取消
        co_await maintenance_timer_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        // 如果服务器已停止或定时器被取消，则退出维护循环
        if (stopped_ || ec) { co_return; }

        // 3. 将后续的维护工作调度到 strand_ 上，以保证对连接池的线程安全访问
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);


        if (!pool_.empty() || !h2_pool_.empty()) {
            // SPDLOG_DEBUG("开始维护连接池...");
            const auto now = time_utils::steady_clock_ms_since_epoch();
            // 4. 维护所有的 HTTP/1.1 连接池
            //    使用 C++20 的 ranges::views::values 可以优雅地只遍历 map 中的值（即 deque）。
            for (auto& queue : pool_ | std::views::values) {
                HttpConnectionDeque healthy_queue;
                while (!queue.empty()) {
                    //SPDLOG_DEBUG("开始维护连接");
                    auto conn = std::move(queue.front());
                    queue.pop_front();

                    // 规则1：连接已死或不可用，记录并丢弃
                    if (!conn->is_usable()) {
                        SPDLOG_DEBUG("抛弃死连接 {}.", conn->id());
                    }

                    // 规则2：连接正在忙，直接放回健康队列
                    else if (conn->get_active_streams() > 0) {
                        // 连接正在处理一个或多个请求，绝对是活的，直接保留
                        SPDLOG_DEBUG("连接正在忙，直接放回健康队列 {}.", conn->id());
                        healthy_queue.push_back(std::move(conn));
                    }

                    // 规则3：连接空闲时间过长，启动关闭流程
                    else if (now - conn->get_last_used_timestamp_ms() > idle_timeout_for_close_ms_) {
                        SPDLOG_DEBUG("关闭闲置时间过长的连接 {}", conn->id());
                        boost::asio::co_spawn(
                            strand_,
                            // 这个 lambda 会捕获 conn 的拷贝，延长其生命周期
                            [conn_to_close = std::move(conn)]() -> boost::asio::awaitable<void> {
                                co_return co_await conn_to_close->close();
                            },
                            boost::asio::detached
                        );
                    }

                    // 规则4：连接需要 Ping 保活
                    else if (now - conn->get_ping_used_timestamp_ms() > idle_timeout_for_ping_ms_) {
                        SPDLOG_DEBUG("对空闲连接 {} 发送 PING 保活", conn->id());
                        if (co_await conn->ping()) {
                            // Ping 成功，放回健康队列
                            healthy_queue.push_back(std::move(conn));
                        } else {
                            // [修复] Ping 失败，说明连接也死了，启动关闭流程
                            SPDLOG_WARN("连接 {} PING 失败，将予以关闭.", conn->id());
                            boost::asio::co_spawn(
                                strand_,
                                [conn_to_close = std::move(conn)]() -> boost::asio::awaitable<void> {
                                    co_await conn_to_close->close();
                                },
                                boost::asio::detached
                            );
                        }
                    }
                    // 规则5 (最后的 else)：连接健康且空闲，但还不需要保活，直接放回
                    else {
                        healthy_queue.push_back(std::move(conn));
                    }
                }
                // 用维护过的健康连接队列替换旧的
                queue = std::move(healthy_queue);
            }


            // 5. 维护所有的 HTTP/2 连接池
            // for (auto& vec : h2_pool_ | std::views::values) {
            //     co_await maintain_pool_container(vec);
            // }

            // SPDLOG_DEBUG("连接池维护完成");
        }
    }
}

/**
 * @brief [私有] 触发后台任务的停止。
 *        这是一个非阻塞操作，通过 post 到 strand 来安全地修改状态。
 */
void ConnectionManager::stop_internal() {
    // 使用 post 确保在 strand 上安全地修改 stopped_ 标志
    post(strand_, [self = shared_from_this()] {
        if (self->stopped_) return;
        self->stopped_ = true;
        self->maintenance_timer_.cancel();
        // 如果需要在析构时也清理连接，可以在这里添加
    });
}
