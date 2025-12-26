//
// Created by Aiziboy on 2025/7/18.
//

#include <aizix/core/client/HttpClientPool.hpp>
#include <aizix/utils/time_util.hpp>
#include <aizix/error/aizix_error.hpp>
#include <aizix/http/network_constants.hpp>
#include <aizix/core/client/http_connection.hpp>
#include <aizix/core/client/https_connection.hpp>
#include <aizix/core/client/h2_connection.hpp>
#include <aizix/core/client/iconnection.hpp>

#include <ranges>
#include <unordered_set>
#include <boost/asio/as_tuple.hpp>
#include <spdlog/spdlog.h>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/beast/ssl.hpp>
#include <aizix/App.hpp>


// 命名空间和类型别名
using namespace std::literals::chrono_literals;
using results_type = boost::asio::ip::tcp::resolver::results_type;
using boost::asio::experimental::awaitable_operators::operator||;
using ConnectionResult = std::pair<std::shared_ptr<IConnection>, ProtocolKnowledge>;
/**
 * @brief ConnectionManager 的构造函数。
 *        负责初始化 SSL 上下文、DNS 解析器，并根据配置启动后台维护任务。
 */
HttpClientPool::HttpClientPool(aizix::App& app, const AizixConfig& config)
    : max_redirects(config.client.max_redirects),
      app_(app),                                  // 保存 App 引用
      ioc_(app.get_main_ioc()),                   // 绑定到 Main Context
      strand_(app.get_main_ioc().get_executor()), // Strand 绑定到 Main Context，保护全局 Map
      ssl_ctx_(boost::asio::ssl::context::tls_client),
      resolver_(app.get_main_ioc()), // Resolver 绑定到 Main Context (DNS 解析量不大，且有缓存，放在主线程无妨)
      connect_timeout_(config.client.connect_timeout_ms),
      http2_enabled_(config.client.http2_enabled),
      http2_max_concurrent_streams_(config.client.http2_max_concurrent_streams),
      maintenance_timer_(app.get_main_ioc()),
      maintenance_interval_(config.client.maintenance_interval_ms),
      idle_timeout_for_close_(config.client.idle_timeout_for_close_ms),
      idle_timeout_for_ping_(config.client.idle_timeout_for_ping_ms),
      max_h1_connections_per_host_(config.client.max_h1_connections_per_host),
      max_h2_connections_per_host_(config.client.max_h2_connections_per_host),
      protocol_cache_ttl_(config.client.protocol_cache_ttl_ms) {
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
    const std::span<const unsigned char> client_protos = network::alpn::get_alpn_protos(config.client.http2_enabled);
    if (SSL_CTX_set_alpn_protos(ssl_ctx_.native_handle(), client_protos.data(), client_protos.size_bytes()) != 0) {
        throw std::runtime_error("Failed to set ALPN protocols on SSL_CTX.");
    }

    // 启动后台维护任务
    start_maintenance();
}

/**
 * @brief 析构函数。确保后台任务被停止。
 */
HttpClientPool::~HttpClientPool() {
    // ✅ 直接清理
    // 此时 ioc 通常已经停止了，或者是正在销毁过程中
    // 我们只需要确保 timer 被取消，不再触发回调即可
    if (!stopped_) stopped_ = true;
    maintenance_timer_.cancel();
}

/**
 * @brief 异步地获取一个到指定目标的健康连接。
 *
 * 这是 HttpClientPool 的核心函数，它以线程安全的方式，为调用者提供一个可用的网络连接。
 * 它实现了连接池的复用、并发创建控制、以及等待/唤醒机制。
 *
 * @par 并发模型
 * - **Strand 序列化**: 所有的核心逻辑，包括对连接池的检查和修改，都通过 `post` 提交到
 *   一个 `strand_` 上执行。这保证了对同一个 ConnectionManager 实例内部状态的访问是
 *   完全串行的，从根本上杜绝了数据竞争。
 * - **创建者/等待者模式**: 当需要创建一个新连接时：
 *   - 第一个到达的协程成为“创建者”，它会启动一个**后台** `co_spawn` 任务去执行耗时的网络IO。
 *   - 后续到达的协程成为“等待者”，它们会异步等待创建任务完成的信号，而不会重复创建连接。
 * - **异步等待**: 所有耗时的操作（创建连接、等待信号）都在 `strand` 之外的通用执行器上进行，
 *   避免了阻塞 `strand`，使得 ConnectionManager 可以同时处理对不同主机的请求。
 * - **原子认领**: 为了解决 H1.1 连接（独占性）在唤醒多个等待者时被并发获取的问题，
 *   引入了 `atomic<bool> has_been_claimed_` 标志。只有一个等待者能成功“认领”新连接，
 *   其他失败者则进入重试循环。
 *
 * @param scheme 协议 ("http" 或 "https")。
 * @param host 目标主机名或 IP。
 * @param port 目标端口号。
 * @return PooledConnection 封装了连接对象和是否为复用的标志。
 * @throws boost::system::system_error 或 std::exception 如果连接创建失败。
 */
boost::asio::awaitable<PooledConnection> HttpClientPool::get_connection(const std::string_view scheme, const std::string_view host, uint16_t port) {
    // 构造用于标识连接池的唯一 Key。
    const std::string key = std::string(scheme) + "//" + std::string(host) + ":" + std::to_string(port);
    SPDLOG_DEBUG("获取连接 [{}]", key);

    // 使用无限循环，以便“认领失败”的协程可以无缝地重试
    while (true) {
        // --- 步骤 1: 进入 Strand，确保对共享状态的独占访问 ---
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);

        // --- 步骤 2: 尝试从池中复用 (在 Strand 内，绝对安全) ---

        // A: 优先检查 H2 连接池
        if (const auto it = h2_pool_.find(key); it != h2_pool_.end()) {
            auto& h2_conns = it->second;
            std::shared_ptr<IConnection> best_conn = nullptr;
            size_t min_streams = SIZE_MAX;

            // 遍历查找一个可用的、且当前负载最低的 H2 连接
            for (const auto& conn : h2_conns) {
                if (conn->is_usable()) {
                    if (const size_t current_streams = conn->get_active_streams(); current_streams < conn->get_max_concurrent_streams() && current_streams < min_streams) {
                        best_conn = conn;
                        min_streams = current_streams;
                    }
                }
            }

            // 在检查过程中，顺便清理掉所有已失效的连接
            std::erase_if(h2_conns, [](const auto& conn) { return !conn->is_usable(); });

            if (best_conn) {
                SPDLOG_DEBUG("复用H2连接 [{}]-[{}] (活动流 {} 个)", best_conn->id(), best_conn->get_pool_key(), min_streams);
                co_return PooledConnection{best_conn, true};
            }
        }

        // B: 如果没有可用的 H2 连接，再检查 H1.1 连接池
        if (const auto it = pool_.find(key); it != pool_.end()) {
            auto& h1_queue = it->second;
            // 逐个取出，跳过不可用或非空闲的，顺便丢弃失效连接
            while (!h1_queue.empty()) {
                // 原子性地从队头取出一个连接
                const auto conn = std::move(h1_queue.front());
                h1_queue.pop_front();
                if (!conn || !conn->is_usable()) {
                    continue; // 丢弃已失效的连接
                }

                // H1.1 连接池中的连接必须是空闲的
                if (conn->get_active_streams() == 0) {
                    SPDLOG_DEBUG("复用H1连接 [{}]-[{}]", conn->id(), conn->get_pool_key());
                    co_return PooledConnection{conn, true};
                } else {
                    // 这是一个不应该发生的状态，表明在其他地方可能存在 Bug。
                    // 记录错误并丢弃这个有问题的连接。
                    SPDLOG_ERROR("取到个能复用的H1连接 [{}] 在连接池中却拥有 {} 个活动流！已将其丢弃。", conn->id(), conn->get_active_streams());
                }
            }
        }


        // 步骤 3: 池空且无创建任务，进行动态决策
        {
            ProtocolKnowledge knowledge = ProtocolKnowledge::Unknown;
            auto iterator = hosts_protocol_.find(key);
            // 检查主机的协议缓存是否有效
            if (iterator != hosts_protocol_.end()) {
                if (std::chrono::steady_clock::now() - iterator->second.last_updated > protocol_cache_ttl_) {
                    // 缓存已过期，当作未知处理，并从 map 中移除
                    SPDLOG_DEBUG("Protocol cache for host '{}' has expired. Re-probing.", key);
                    hosts_protocol_.erase(iterator);
                    knowledge = ProtocolKnowledge::Unknown;
                } else {
                    // 缓存有效
                    knowledge = iterator->second.knowledge;
                }
            }

            bool should_auto_create = false;
            switch (knowledge) {
                case ProtocolKnowledge::SupportsH2:
                    should_auto_create = true;
                    break;
                case ProtocolKnowledge::RequiresH1:
                    should_auto_create = false;
                    break;
                case ProtocolKnowledge::Unknown:
                    // 只有在配置允许 H2 且协议是 https 时，才进行乐观尝试
                    should_auto_create = (scheme == "https:" && http2_enabled_);
                    break;
            }
            if (!should_auto_create) {
                // 决策为：不自动创建，交由调用者处理
                SPDLOG_DEBUG("主机 '{}' (已知为H1.1或不符合H2尝试条件) 连接池为空，交由调用者处理。", key);
                co_return PooledConnection{nullptr, false};
            }
        }

        // --- 步骤 4: 并发创建控制（惊群效应处理） ---

        // a. 检查是否已有其他协程正在为这个 key 创建连接
        auto it = creation_in_progress_.find(key);
        if (it == creation_in_progress_.end()) {
            // --- 情况 A: 成为“创建者” ---
            SPDLOG_DEBUG("连接池 '{}' 为空，发起新的创建任务。", key);

            // 创建一个状态对象，包含用于通知的 channel
            auto new_creation = std::make_shared<CreationInProgress>(ioc_.get_executor());
            it = creation_in_progress_.emplace(key, new_creation).first;

            // 启动一个分离的后台协程去执行耗时的网络 IO
            boost::asio::co_spawn(
                ioc_, // 在 ioc 的通用执行器上运行，避免阻塞 strand
                [self = shared_from_this(), key, state = new_creation, scheme_s = std::string(scheme), host_s = std::string(host), port]() -> boost::asio::awaitable<void> {
                    // 1. 在 ioc_ 上执行创建
                    std::shared_ptr<IConnection> new_conn = nullptr;
                    std::exception_ptr ex_ptr = nullptr;
                    try {
                        new_conn = co_await self->create_new_connection(key, scheme_s, host_s, port);
                    } catch (...) {
                        ex_ptr = std::current_exception();
                    }

                    // 2. 创建完成后，回到 strand 安全地更新共享状态
                    co_await boost::asio::post(self->strand_, boost::asio::use_awaitable);

                    // 如果成功创建了 H2 连接，立即入池以供共享
                    if (new_conn && new_conn->supports_multiplexing()) {
                        self->add_connection_to_pool_h2(key, new_conn);
                    }

                    // 将最终结果（连接或异常）存入 state 对象
                    if (ex_ptr) {
                        state->result = ex_ptr;
                    } else {
                        state->result = new_conn;
                    }

                    // 广播完成信号，唤醒所有等待者
                    state->signal->close();
                    co_return;
                },
                boost::asio::detached);
        }

        // --- 步骤 4: 等待与认领 (创建者和等待者都执行此路径) ---
        const auto state_to_wait_on = it->second;
        const std::string waiting_key = it->first; // 拷贝 key，因为 it 可能在等待期间失效
        SPDLOG_DEBUG("等待host '{}' 的连接创建完成...", key);

        // a. 离开 strand，去异步等待信号，避免阻塞其他操作
        co_await boost::asio::post(ioc_.get_executor(), boost::asio::use_awaitable);
        co_await state_to_wait_on->signal->async_receive(boost::asio::as_tuple(boost::asio::use_awaitable));

        // b. 尝试原子性地“认领”这个新创建的结果
        if (!state_to_wait_on->has_been_claimed_.exchange(true, std::memory_order_acquire)) {
            // *** 胜利者 ***: 你是第一个成功将标志位从 false 换成 true 的协程
            SPDLOG_DEBUG("成功认领新创建任务的结果 '{}'", waiting_key);

            // c. 回到 strand 安全地读取和处理结果
            co_await boost::asio::post(strand_, boost::asio::use_awaitable);
            creation_in_progress_.erase(waiting_key); // 胜利者负责清理创建占位符


            if (const auto* conn_ptr = std::get_if<std::shared_ptr<IConnection>>(&state_to_wait_on->result)) {
                // 无论是 H1.1 还是 H2，胜利者都直接使用这个新连接。
                // H2 已经由后台任务入池了。
                auto& new_conn = *conn_ptr;
                co_return PooledConnection{new_conn, false};
            } else if (const auto* ex_ptr = std::get_if<std::exception_ptr>(&state_to_wait_on->result)) {
                // 如果创建失败，胜利者负责将异常抛出
                std::rethrow_exception(*ex_ptr);
            } else {
                // 健壮性检查，理论上不应发生
                throw std::logic_error("连接创建任务完成，但未设置有效结果。");
            }
        } else {
            // *** 失败者 ***
            // 失败者需要检查占位符是否还在（可能胜利者还没来得及清理）
            SPDLOG_DEBUG("认领新连接 '{}' 失败，已被他人获取。正在重试...", waiting_key);
            co_await boost::asio::post(strand_, boost::asio::use_awaitable);
            if (auto creation_it = creation_in_progress_.find(waiting_key); creation_it != creation_in_progress_.end() && creation_it->second == state_to_wait_on) {
                creation_in_progress_.erase(creation_it);
            }
            // 回到循环顶部，重新开始整个流程。
            // 此时，池中很可能已经有可复用的连接了（比如胜利者创建的 H2 连接）。
            continue; // 重试
        }
    }
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
void HttpClientPool::release_connection(const std::shared_ptr<IConnection>& conn) {
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
            if (usable) {
                //  H1 的归还池中...
                self->pool_[key].push_back(conn); // 放回队尾，实现 LIFO/FIFO 策略
                //SPDLOG_DEBUG("将连接 [{}] {}存入连接池,当前连接 [{}] 的连接池大小: {}", conn->id(), key, pool_[key].back()->get_pool_key(), pool_[key].size());
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
boost::asio::awaitable<void> HttpClientPool::stop() {
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
 * @brief [私有] 将一个新创建的H2连接添加到相应的池中。
 */
void HttpClientPool::add_connection_to_pool_h2(const std::string& key, const std::shared_ptr<IConnection>& conn) {
    if (conn->supports_multiplexing()) {
        h2_pool_[key].push_back(conn);
        SPDLOG_DEBUG("将 {} 添加到h2_pool中，当前连接数量{}", key, h2_pool_[key].size());
    } else {
        // H1 创建后直接给请求者，不入池；用完归还才入池
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
 * @throws std::system_error 如果连接超时
 * @throws std::runtime_error 如果协议不被支持。
 */
boost::asio::awaitable<std::shared_ptr<IConnection>> HttpClientPool::create_new_connection(const std::string& key, const std::string_view scheme, std::string_view host, const uint16_t port) {
    // 初始化变量
    auto protocol = ProtocolKnowledge::Unknown;
    std::shared_ptr<IConnection> new_conn = nullptr;
    std::exception_ptr exception_ptr = nullptr;

    try {
        // 直接在 ioc_ 上创建定时器
        boost::asio::steady_timer timer(ioc_);
        // 1. 设置一个总的连接超时
        //    这涵盖了 DNS、TCP 和 TLS 握手的总和时间。
        timer.expires_after(connect_timeout_);

        // 2. 使用 `||` 操作符，让连接创建逻辑与超时定时器进行“竞赛”。
        auto result = co_await ([this, key, scheme, host, port]() -> boost::asio::awaitable<ConnectionResult> {
                // --- 操作A: 完整的连接创建逻辑，被封装在 lambda 协程中 ---

                // 3. 异步 DNS 解析。 (在 Main Context 执行)
                boost::system::error_code ec;
                const results_type endpoints = co_await resolver_.async_resolve(tcp::v4(), host, std::to_string(port), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                if (ec) throw boost::system::system_error(ec, "DNS resolution failed");

                // 4. 创建连接 为新连接分配一个 Worker IO Context
                auto& io_context = app_.get_ioc();
                if (scheme == "https:") {
                    // a. 创建 HTTP/1.1 socket连接。绑定到 Worker Context, 之后的读写、加密都在 Worker 线程并行执行
                    tcp::socket socket(io_context);

                    // 性能优化: 禁用 Nagle 算法开启TCP_NODELAY，这对 HTTP/1/2 性能至关重要
                    socket.set_option(boost::asio::ip::tcp::no_delay(true), ec);

                    // b. 用 socket 创建 ssl_stream (它会继承 socket 的 executor)
                    auto stream = std::make_shared<boost::beast::ssl_stream<tcp::socket>>(std::move(socket), ssl_ctx_);

                    // c. 建立 TCP 连接，async_connect 会跨线程操作，最终在 io_context 线程完成
                    ec.clear();
                    co_await async_connect(stream->next_layer(), endpoints, boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                    if (ec) throw boost::system::system_error(ec, "TCP connect failed for SSL");

                    // d. 设置 SNI (Server Name Indication)。
                    //    这在 TLS 握手中至关重要，它告诉服务器我们想要访问哪个主机。
                    //    缺少 SNI 会导致使用多域名证书的服务器返回错误的证书。
                    if (!SSL_set_tlsext_host_name(stream->native_handle(), std::string(host).c_str())) {
                        throw boost::system::system_error(static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category());
                    }

                    // e. 执行 TLS 握手。  (计算密集型，将在 io_contexts_ 池里的某一个线程 上执行)
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
                        auto conn = Http2Connection::create(stream, key, http2_max_concurrent_streams_, idle_timeout_for_close_);

                        // 等待 H2 握手完成。
                        co_await conn->run();
                        co_return ConnectionResult{conn, ProtocolKnowledge::SupportsH2}; // co_return 一个包含连接和协议知识的 pair
                    } else {
                        // 协商失败或不支持，回退到 HTTPS/1.1。
                        SPDLOG_DEBUG("ALPN selected HTTP/1.1 for {}.", host);
                        co_return ConnectionResult{std::make_shared<HttpsConnection>(stream, key), ProtocolKnowledge::RequiresH1}; // co_return 一个包含连接和协议知识的 pair
                    }
                }
                if (scheme == "http:") {
                    // 创建纯文本 HTTP/1.1 连接。
                    ec.clear();
                    tcp::socket socket(io_context);

                    // 性能优化: 禁用 Nagle 算法开启TCP_NODELAY，这对 HTTP/1/2 性能至关重要
                    socket.set_option(boost::asio::ip::tcp::no_delay(true), ec);

                    co_await async_connect(socket, endpoints.begin(), endpoints.end(), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                    if (ec) { throw boost::system::system_error(ec, "TCP connect failed"); }

                    co_return ConnectionResult{std::make_shared<HttpConnection>(std::move(socket), key), ProtocolKnowledge::RequiresH1};
                }
                // 确保所有路径都有出口
                throw std::runtime_error("未知协议Http: '" + std::string(scheme) + "'. 仅支持 'http:' and 'https:' 协议");
            }() // <<-- 立即调用 lambda 以创建 awaitable
            ||
            // --- 操作B: 超时定时器 ---
            timer.async_wait(boost::asio::use_awaitable));

        // 5. 检查建立连接是否超时了
        if (result.index() == 1) {
            // .index() == 1 表示定时器获胜
            // 超时发生，抛出明确的超时异常。
            throw std::system_error(aizix_error::network::connection_timeout);
        }

        // 如果能到这里，说明是连接创建操作 (index 0) 获胜。
        // 必须取消仍在后台等待的定时器，以清理资源。
        timer.cancel();

        // 6. 从 variant 中解包出 ConnectionResult (即 std::pair)
        const auto [conn_ptr, proto_knowledge] = std::get<0>(result); // 使用 C++17 结构化绑定
        new_conn = conn_ptr;
        protocol = proto_knowledge;
    } catch (...) {
        exception_ptr = std::current_exception(); // 保存原始异常
    }

    // --- 无论成功或失败，都回到 strand 安全地更新状态 ---
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);

    // --- 检查结果，重新抛出异常或返回连接 ---
    if (exception_ptr) {
        SPDLOG_ERROR("Failed to create new connection to {}", key);
        // 网络连接失败了
        // 合理的策略是，清除host的旧协议，以便下次重试。
        hosts_protocol_.erase(key);            // 安全地清除可能已过时的host 协议
        std::rethrow_exception(exception_ptr); // 既然捕获了异常，就在这里重新抛出它
    }
    // 连接成功
    hosts_protocol_[key] = {protocol, std::chrono::steady_clock::now()};
    co_return new_conn;
}

/**
 * @brief [私有] 启动后台维护任务。
 */
void HttpClientPool::start_maintenance() {
    // 启动后台协程，它将独立运行
    boost::asio::co_spawn(ioc_, run_maintenance(), boost::asio::detached);
}

/**
 * @brief [私有] 后台维护任务的协程实现。
 */
boost::asio::awaitable<void> HttpClientPool::run_maintenance() {
    while (!stopped_) {
        // 1. 设置下一次维护的定时器
        maintenance_timer_.expires_after(maintenance_interval_);
        boost::system::error_code ec;


        // 2. 挂起协程，等待定时器到期或被取消
        co_await maintenance_timer_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        // 如果服务器已停止或定时器被取消，则退出维护循环
        if (stopped_ || ec) { co_return; }

        // 3. 将后续的维护工作调度到 strand_ 上，以保证对连接池的线程安全访问
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);


        if (!pool_.empty() || !h2_pool_.empty()) {
            // SPDLOG_DEBUG("开始维护连接池...");
            const auto now = std::chrono::steady_clock::now();
            // 4. 维护所有的 HTTP/1.1 连接池
            //    使用 C++20 的 ranges::views::values 可以优雅地只遍历 map 中的值（即 deque）。
            for (auto& [key, queue] : pool_) {
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
                    else if (now - conn->get_last_used_timestamp_ms() > idle_timeout_for_close_ && conn->get_active_streams() < 1) {
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
                    else if (now - conn->get_ping_used_timestamp_ms() > idle_timeout_for_ping_) {
                        //SPDLOG_DEBUG("对空闲连接 {} 发送 PING 保活，暂时将其移出池。", conn->id());
                        // 将连接的所有权完全移交给后台任务
                        boost::asio::co_spawn(
                            ioc_, // 在 ioc_ 的通用执行器上运行 PING I/O，
                            [self = shared_from_this(),conn_to_ping = std::move(conn)]() -> boost::asio::awaitable<void> {
                                // 在独立的协程中执行 PING
                                if (!co_await conn_to_ping->ping()) {
                                    SPDLOG_WARN("连接 {} PING 失败，将予以关闭.", conn_to_ping->id());
                                    co_await conn_to_ping->close(); // 如果 ping 失败，就关闭它
                                } else {
                                    self->release_connection(conn_to_ping);
                                }
                            },
                            boost::asio::detached
                        );
                    } else {
                        // 规则5 (最后的 else)：连接健康且空闲，但还不需要保活，直接放回
                        healthy_queue.push_back(std::move(conn));
                    }
                }
                // 用维护过的健康连接队列替换旧的
                queue = std::move(healthy_queue);

                // 限制池中的最大空闲连接数
                if (queue.size() > max_h1_connections_per_host_) {
                    SPDLOG_DEBUG("主机 '{}' 的H1空闲连接数 ({}) 超过限制 ({})，开始高效修剪。",
                                 key, queue.size(), max_h1_connections_per_host_);

                    const size_t to_prune = queue.size() - max_h1_connections_per_host_;

                    // 1. 将连接从 deque 高效移动到临时 vector
                    std::vector<std::shared_ptr<IConnection>> temp_vec;
                    temp_vec.reserve(queue.size());
                    std::ranges::move(queue, std::back_inserter(temp_vec));
                    queue.clear(); // 确保旧 deque 为空

                    // 2. 使用 std::nth_element (O(N) 复杂度) 找到分割点
                    using diff_type = typename std::vector<std::shared_ptr<IConnection>>::iterator::difference_type;
                    auto pruning_threshold_it = temp_vec.begin() + static_cast<diff_type>(to_prune);
                    std::ranges::nth_element(temp_vec, pruning_threshold_it,
                                             [](const auto& a, const auto& b) {
                                                 return a->get_last_used_timestamp_ms() < b->get_last_used_timestamp_ms();
                                             });

                    // 3. 关闭 "to_prune" 个最近最少使用的连接
                    for (auto it = temp_vec.begin(); it != pruning_threshold_it; ++it) {
                        SPDLOG_DEBUG("修剪H1连接 [{}]", (*it)->id());
                        boost::asio::co_spawn(
                            strand_,
                            [conn = std::move(*it)]() -> boost::asio::awaitable<void> {
                                co_await conn->close();
                            },
                            boost::asio::detached);
                    }

                    // 4. 将剩余的健康连接移回 deque
                    queue.assign(std::make_move_iterator(pruning_threshold_it),
                                 std::make_move_iterator(temp_vec.end()));
                }
            }

            // 在维护完所有 H1.1 连接后，统一清理那些已经变空的条目
            std::erase_if(pool_, [](const auto& item) {
                const auto& [key, queue] = item;
                return queue.empty();
            });


            // 5. 维护所有的 HTTP/2 连接池
            for (auto& [key, vec] : h2_pool_) {
                H2ConnectionVector healthy_vec;
                for (auto& conn : vec) {
                    // 这里的逻辑与 H1.1 的非常相似
                    if (!conn->is_usable()) {
                        SPDLOG_DEBUG("抛弃死H2连接 {}.", conn->id());
                        continue; // 直接丢弃
                    }

                    if (now - conn->get_last_used_timestamp_ms() > idle_timeout_for_close_ && conn->get_active_streams() < 1) {
                        SPDLOG_DEBUG("关闭闲置时间过长的H2连接 {}", conn->id());
                        boost::asio::co_spawn(
                            strand_,
                            [conn_to_close = std::move(conn)]() -> boost::asio::awaitable<void> {
                                co_await conn_to_close->close();
                            },
                            boost::asio::detached
                        );
                    } else if (now - conn->get_ping_used_timestamp_ms() > idle_timeout_for_ping_) {
                        //SPDLOG_DEBUG("对空闲H2连接 {} 发送 PING 保活", conn->id());
                        // 同样，应该异步执行 PING，避免阻塞 strand
                        boost::asio::co_spawn(
                            ioc_,
                            [self = shared_from_this(), conn_to_ping = conn]() -> boost::asio::awaitable<void> {
                                if (!co_await conn_to_ping->ping()) {
                                    //SPDLOG_WARN("H2连接 {} PING 失败，将予以关闭.", conn_to_ping->id());
                                    co_await conn_to_ping->close();
                                } else {
                                    co_await post(self->strand_, boost::asio::use_awaitable);
                                    //SPDLOG_DEBUG("H2连接 {} PING 成功，归还到连接池。", conn_to_ping->id());
                                    self->h2_pool_[conn_to_ping->get_pool_key()].push_back(conn_to_ping);
                                }
                            },
                            boost::asio::detached
                        );
                    } else {
                        // 最后的 else：连接健康且空闲，但还不需要保活，直接放回
                        healthy_vec.push_back(conn); // 只有这里才把连接放回
                    }
                }

                // 替换旧的
                vec = std::move(healthy_vec);

                // H2 连接池修剪逻辑
                if (vec.size() > max_h2_connections_per_host_) {
                    SPDLOG_DEBUG("主机 '{}' 的H2连接数 ({}) 超过限制 ({})，开始修剪。",
                                 key, vec.size(), max_h2_connections_per_host_);

                    // 筛选出所有空闲的连接
                    std::vector<std::shared_ptr<IConnection>> idle_connections;
                    for (const auto& conn : vec) {
                        if (conn->get_active_streams() == 0) {
                            idle_connections.push_back(conn);
                        }
                    }

                    if (const size_t to_prune_count = vec.size() - max_h2_connections_per_host_; idle_connections.size() > to_prune_count) {
                        // 1. O(N) 找到要修剪的连接
                        // 找到要修剪的空闲连接的分割点
                        using diff_type = typename std::vector<std::shared_ptr<IConnection>>::iterator::difference_type;
                        const auto pruning_threshold_it = idle_connections.begin() + static_cast<diff_type>(to_prune_count);
                        std::ranges::nth_element(idle_connections, pruning_threshold_it,
                                                 [](const auto& a, const auto& b) {
                                                     return a->get_last_used_timestamp_ms() < b->get_last_used_timestamp_ms();
                                                 });

                        // 2. 使用 Set 进行高效查找 ，以便 O(1) 查找
                        std::unordered_set<std::shared_ptr<IConnection>> connections_to_close(idle_connections.begin(), pruning_threshold_it);

                        // 3. 异步关闭这些连接
                        for (const auto& conn : connections_to_close) {
                            SPDLOG_DEBUG("修剪H2连接 [{}]", conn->id());
                            boost::asio::co_spawn(strand_, [conn]() -> boost::asio::awaitable<void> {
                                co_await conn->close();
                            }, boost::asio::detached);
                        }

                        // 4. O(N) 一次性从主 vector 中移除所有要关闭的连接
                        std::erase_if(vec, [&](const auto& conn) {
                            return connections_to_close.contains(conn);
                        });
                    }
                }
            }

            // 清理 map 中空的 vector
            std::erase_if(h2_pool_, [](const auto& item) {
                return item.second.empty();
            });


            // --- 清理主机的协议缓存 ---

            // 1. 获取当前时间，只调用一次以提高效率
            const auto now_steady = std::chrono::steady_clock::now();

            // 2. 调用 std::erase_if，它会遍历 map 中的每个元素
            std::erase_if(hosts_protocol_,
                          // 3. 这是一个 lambda 表达式，作为“删除条件”
                          [&](const auto& item) -> bool {
                              // 4. 使用结构化绑定，从 item (一个 pair) 中解构出 key 和 info
                              const auto& [key, info] = item;
                              // 5. 计算缓存条目的“年龄”
                              auto age = now_steady - info.last_updated;
                              // 6. 判断年龄是否超过了设定的 TTL (Time-To-Live)
                              if (age > protocol_cache_ttl_) {
                                  // 7. 如果超过，返回 true，告诉 std::erase_if：“请删除这个元素”
                                  SPDLOG_DEBUG("Pruning expired protocol cache for host '{}'.", key);
                                  return true;
                              }
                              // 8. 如果没超过，返回 false，告诉 std::erase_if：“请保留这个元素”
                              return false;
                          }
            );
        }
    }
}
