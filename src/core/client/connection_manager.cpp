//
// Created by Aiziboy on 2025/7/18.
//

#include "connection_manager.hpp"
#include "http1_connection.hpp" // 需要包含具体的连接实现
#include <spdlog/spdlog.h>
#include "iconnection.hpp"         // << 提供 IConnection 的完整定义
#include "http_ssl_connection.hpp"
#include "h2_connection.hpp"
#include "utils/utils.hpp"
#include "utils/finally.hpp" // 假设你有一个 RAII guard
#include <boost/beast/ssl.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

using namespace std::literals::chrono_literals;
using results_type = boost::asio::ip::tcp::resolver::results_type;

ConnectionManager::ConnectionManager(boost::asio::io_context& ioc, bool enable_maintenance)
    : ioc_(ioc),
      strand_(ioc.get_executor()),
      ssl_ctx_(boost::asio::ssl::context::tls_client),
      resolver_(ioc),
      maintenance_timer_(ioc) {
    // 明确禁用 TLS 1.0 和 1.1，从而只留下 1.2 和 1.3
    ssl_ctx_.set_options(
        boost::asio::ssl::context::default_workarounds |
        boost::asio::ssl::context::no_sslv2 |
        boost::asio::ssl::context::no_sslv3 |
        boost::asio::ssl::context::no_tlsv1 |
        boost::asio::ssl::context::no_tlsv1_1
    );

    // [可选，但推荐] 设置一个现代化的加密套件列表。
    // 这个列表是从 Mozilla Intermediate compatibility 推荐中提取的，兼容性很好。
    const char* modern_ciphers =
        "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
        "DHE-RSA-AES128-GCM-SHA256:DHE-RSA-AES256-GCM-SHA384";

    if (SSL_CTX_set_cipher_list(ssl_ctx_.native_handle(), modern_ciphers) != 1) {
        // 如果设置失败，可以记录一个错误，但不一定要中断程序
        SPDLOG_ERROR("Could not set SSL cipher list.");
    }

    // 显式设置验证回调 (set_verify_callback) 并不是严格必须的
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(boost::asio::ssl::verify_peer);

    const unsigned char supported_protos[] = {
        2, 'h', '2',
        8, 'h', 't', 't', 'p', '/', '1', '.', '1'
    };
    SSL_CTX_set_alpn_protos(ssl_ctx_.native_handle(), supported_protos, sizeof(supported_protos));

    if (enable_maintenance) {
        start_maintenance();
    }
}

ConnectionManager::~ConnectionManager() {
    stop_internal();
}

void ConnectionManager::stop_internal() {
    // 使用 post 确保在 strand 上安全地修改 stopped_ 标志
    post(strand_, [self = shared_from_this()] {
        if (self->stopped_) return;
        self->stopped_ = true;
        self->maintenance_timer_.cancel();
        // 如果需要在析构时也清理连接，可以在这里添加
    });
}


boost::asio::awaitable<void> ConnectionManager::stop() {
    // 1. 进入 strand，以保证对 stopped_ 和连接池的访问是线程安全的。
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);

    // 2. 使用 std::atomic::exchange 来实现“只执行一次”的逻辑。
    //    这可以防止多个协程并发调用 stop() 时，清理逻辑被执行多次。
    if (stopped_) co_return;
    stopped_ = true;

    // 3. 取消后台维护计时器。
    maintenance_timer_.cancel();

    SPDLOG_INFO("ConnectionManager: Shutting down all connections...");

    // 4. 从所有池中收集需要关闭的连接的共享指针。
    std::vector<std::shared_ptr<IConnection>> all_conns_to_close;

    for (auto const& [key, queue] : pool_) {
        all_conns_to_close.insert(all_conns_to_close.end(), queue.begin(), queue.end());
    }
    pool_.clear(); // 清空池

    for (auto const& [key, vec] : h2_pool_) {
        all_conns_to_close.insert(all_conns_to_close.end(), vec.begin(), vec.end());
    }
    h2_pool_.clear(); // 清空池

    if (all_conns_to_close.empty()) {
        SPDLOG_INFO("ConnectionManager: No active connections to close.");
        co_return;
    }

    SPDLOG_INFO("ConnectionManager: Closing {} connections in parallel...", all_conns_to_close.size());

    // --- 使用 co_spawn + channel 实现健壮的并行等待 ---

    auto ex = co_await boost::asio::this_coro::executor;

    // 5. 创建一个 channel 的【共享指针】。channel 本身不可拷贝，但它的 shared_ptr 可以。
    //    channel 用于从关闭任务中接收“完成”信号。
    auto completion_channel = std::make_shared<
        boost::asio::experimental::channel<void(boost::system::error_code, bool)>
    >(ex, all_conns_to_close.size());

    // 6. 并发地为每个连接启动一个独立的关闭协程。
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
                    SPDLOG_WARN("Exception during connection [{}] close: {}", conn->id(), e.what());
                }

                // c. 任务完成，向 channel 发送一个信号。
                //    我们通过共享指针调用 async_send，这是非 const 操作，但指针本身是 const 的，所以没问题。
                boost::system::error_code ignored_ec;
                co_await channel_ptr->async_send(ignored_ec, true, boost::asio::use_awaitable);
            },

            boost::asio::detached // 分离协程，我们在这里不等待它，而是通过 channel 等待。
        );
    }

    // 7. 在主 stop() 协程中，循环等待，直到收到所有任务的完成信号。
    SPDLOG_DEBUG("ConnectionManager: Waiting for {} close tasks to complete...", all_conns_to_close.size());
    for (size_t i = 0; i < all_conns_to_close.size(); ++i) {
        // 每次 async_receive 都会挂起，直到一个关闭任务完成并发送信号。
        co_await completion_channel->async_receive(boost::asio::use_awaitable);
    }

    SPDLOG_INFO("ConnectionManager: All connections have been successfully closed.");
}

void ConnectionManager::start_maintenance() {
    // 启动后台协程，它将独立运行
    boost::asio::co_spawn(ioc_, run_maintenance(), boost::asio::detached);
}


boost::asio::awaitable<void> ConnectionManager::run_maintenance() {
    while (!stopped_) {
        maintenance_timer_.expires_after(std::chrono::seconds((35s))); // 每 15 秒维护一次
        // 使用 redirect_error 来忽略 timer 被 cancel 时的异常
        boost::system::error_code ec;
        co_await maintenance_timer_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));


        // 将维护工作 post 到 strand 上
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);


        // 遍历所有连接池
        for (auto& [key, queue] : pool_) {
            H1ConnectionDeque healthy_queue;
            while (!queue.empty()) {
                auto conn = std::move(queue.front());
                queue.pop_front();

                // 规则1：如果连接已经不可用，直接丢弃
                if (!conn->is_usable()) {
                    SPDLOG_DEBUG("Pruning dead connection {}.", conn->id());
                    continue;
                }

                // 我们通过 active_streams() 来判断连接是否空闲
                if (conn->get_active_streams() > 0) {
                    // 连接正在处理一个或多个请求，绝对是活的，直接保留
                    healthy_queue.push_back(std::move(conn));
                    continue;
                }

                // 规则2：如果连接空闲时间太长（例如超过60秒），主动关闭它
                const auto now = steady_clock_seconds_since_epoch();
                if (now - conn->get_last_used_timestamp_seconds() > 60) {
                    SPDLOG_INFO("关闭闲置时间过长的连接 {}", conn->id());
                    boost::asio::co_spawn(
                        strand_,
                        // 这个 lambda 会捕获 conn 的拷贝，延长其生命周期
                        [conn_to_close = std::move(conn)]() -> boost::asio::awaitable<void> {
                            co_return co_await conn_to_close->close();
                        },
                        boost::asio::detached
                    );
                    continue;
                }

                // 规则3：如果连接空闲超过一个阈值（例如10秒），发送 PING 保活
                if (now - conn->get_last_used_timestamp_seconds() > 30) {
                    SPDLOG_INFO("ping {} ", conn->id());
                    co_await conn->ping();
                }
                healthy_queue.push_back(std::move(conn));
            }
            // 用维护过的健康连接队列替换旧的
            queue = std::move(healthy_queue);
        }
    }
}


/* 旧版本能用get_connection
 *
 *boost::asio::awaitable<PooledConnection> ConnectionManager::get_connection(std::string_view scheme, std::string_view host, uint16_t port) {
    std::string key = std::string(scheme) + "//" + std::string(host) + ":" + std::to_string(port);

    // 使用一个 for 循环来实现“轮询等待”的逻辑，避免无限等待。
    // 100次 * 100ms = 最多等待 10 秒。
    for (int attempts = 0; attempts < 100; ++attempts) {
        // 1. 进入 strand 以安全地访问共享状态
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);

        // --- 策略 1: 检查 H2 连接池 ---
        if (auto it = h2_pool_.find(key); it != h2_pool_.end()) {
            auto& h2_conns = it->second;
            std::shared_ptr<IConnection> best_conn = nullptr;
            size_t min_streams = SIZE_MAX;

            for (const auto& conn : h2_conns) {
                if (conn->is_usable()) {
                    size_t current_streams = conn->get_active_streams();
                    if (current_streams < conn->get_max_concurrent_streams() && current_streams < min_streams) {
                        best_conn = conn;
                        min_streams = current_streams;
                    }
                }
            }

            // 清理所有不可用的连接 (使用 C++20 的 std::erase_if)
            std::erase_if(h2_conns, [](const auto& conn) {
                return !conn->is_usable();
            });

            if (best_conn) {
                SPDLOG_DEBUG("Multiplexing on H2 connection {} ({} active streams)", best_conn->id(), min_streams);
                co_return PooledConnection{best_conn, true};
            }
        }

        // --- 策略 2: 检查 H1.1 连接池 ---
        if (auto it = pool_.find(key); it != pool_.end()) {
            auto& h1_queue = it->second;
            while (!h1_queue.empty()) {
                auto conn = std::move(h1_queue.front());
                h1_queue.pop_front();
                if (conn->is_usable() && conn->get_active_streams() == 0) {
                    SPDLOG_DEBUG("Reusing H1 connection [{}] from pool.", conn->id());
                    co_return PooledConnection{conn, true};
                }
            }
        }

        // --- 策略 3: 处理连接创建 ---

        // 检查是否已有其他协程正在为这个 key 创建连接
        if (creation_in_progress_.contains(key)) {
            // 别人正在创建，离开 strand，异步等待一小段时间，然后重新尝试（下次循环）
            SPDLOG_DEBUG("Connection for '{}' is being created by another task, waiting for 10ms...", key);

            // 允许其他协程在 ioc 线程上运行
            co_await boost::asio::post(ioc_, boost::asio::use_awaitable);

            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
            timer.expires_after(std::chrono::milliseconds(10ms));
            co_await timer.async_wait(boost::asio::use_awaitable);

            // 等待结束后，继续下一次 for 循环，重新检查所有池
            continue;
        }

        // --- 如果代码执行到这里，说明我是第一个创建者 ---

        // a. 在 strand 上“占坑”，标记我正在创建
        SPDLOG_DEBUG("'{}' 没有可用的连接，创建新的连接", key);
        creation_in_progress_.insert(key);

        // b. 使用 RAII guard 确保“坑”一定会被释放，即使发生异常
        auto guard = Finally([self = shared_from_this(), key] {
            // 清理操作也必须 post 到 strand
            post(self->strand_, [self, key] {
                self->creation_in_progress_.erase(key);
                SPDLOG_DEBUG("Creation placeholder for '{}' removed.", key);
            });
        });
        boost::asio::experimental::channel<void(boost::system::error_code)> notify_channel{ioc_, 1};
        std::unordered_map<std::string, std::shared_ptr<boost::asio::experimental::channel<void(boost::system::error_code)>>> wait_channels_;


        try {
            // c. 离开 strand 去执行耗时的网络 I/O
            co_await boost::asio::post(ioc_, boost::asio::use_awaitable);
            auto new_conn = co_await create_new_connection(key, std::string(scheme), std::string(host), port);

            // d. 成功了，回到 strand 将新连接放入池中
            co_await boost::asio::post(strand_, boost::asio::use_awaitable);

            if (new_conn->supports_multiplexing()) {
                h2_pool_[key].push_back(new_conn);
            } else {
                //pool_[key].push_back(new_conn);
            }

            co_return PooledConnection{new_conn, false};
        } catch (const boost::system::system_error& e) {
            // e. 失败了，`Finally` guard 会自动清理标记。
            //    我们只需重新抛出异常，让调用者 (HttpClient) 知道失败了。
            SPDLOG_ERROR("Failed to create new connection for '{}'.", key);
            std::rethrow_exception(std::current_exception());
            //throw boost::system::system_error(e);
        }
    }

    // 如果循环了 100 次还没拿到连接，说明可能有问题，抛出超时错误
    throw std::runtime_error("Failed to get connection for " + key + " after multiple retries (timeout).");
}*/


boost::asio::awaitable<PooledConnection> ConnectionManager::get_connection(std::string_view scheme, std::string_view host, uint16_t port) {
    std::string key = std::string(scheme) + "//" + std::string(host) + ":" + std::to_string(port);
    auto ex = co_await boost::asio::this_coro::executor;

    // --- 进入 strand 以安全地访问共享状态 ---
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);

    // 1. 检查连接池
    // --- 策略 1: 检查 H2 连接池 ---
    if (auto it = h2_pool_.find(key); it != h2_pool_.end()) {
        auto& h2_conns = it->second;
        std::shared_ptr<IConnection> best_conn = nullptr;
        size_t min_streams = SIZE_MAX;

        for (const auto& conn : h2_conns) {
            if (conn->is_usable()) {
                size_t current_streams = conn->get_active_streams();
                if (current_streams < conn->get_max_concurrent_streams() && current_streams < min_streams) {
                    best_conn = conn;
                    min_streams = current_streams;
                }
            }
        }

        // 清理所有不可用的连接 (使用 C++20 的 std::erase_if)
        std::erase_if(h2_conns, [](const auto& conn) {
            return !conn->is_usable();
        });

        if (best_conn) {
            SPDLOG_DEBUG("Multiplexing on H2 connection {} ({} active streams)", best_conn->id(), min_streams);
            co_return PooledConnection{best_conn, true};
        }
    }

    // --- 策略 2: 检查 H1.1 连接池 ---
    if (auto it = pool_.find(key); it != pool_.end()) {
        auto& h1_queue = it->second;
        while (!h1_queue.empty()) {
            auto conn = std::move(h1_queue.front());
            h1_queue.pop_front();
            if (conn->is_usable() && conn->get_active_streams() == 0) {
                SPDLOG_DEBUG("Reusing H1 connection [{}] from pool.", conn->id());
                co_return PooledConnection{conn, true};
            }
        }
    }


    // 2. 检查是否已经有协程在创建
    auto it = creation_in_progress_.find(key);
    if (it == creation_in_progress_.end()) {
        // --- A. 如果没有，当前协程成为“创建者” ---
        SPDLOG_DEBUG("没有连接 '{}'，发起新的创建任务。", key);

        // a. 创建共享状态并放入 map
        auto new_creation = std::make_shared<CreationInProgress>(ex);
        it = creation_in_progress_.emplace(key, new_creation).first;

        // b. 启动一个分离的任务来执行创建工作
        boost::asio::co_spawn(
            ioc_, // 在 ioc_ 的上下文中执行网络操作
            [self = shared_from_this(), key, state = new_creation, scheme_s = std::string(scheme), host_s = std::string(host), port]() -> boost::asio::awaitable<void> {
                // 定义一个局部变量来捕获异常
                std::exception_ptr exception = nullptr;
                std::shared_ptr<IConnection> new_conn = nullptr;

                auto guard = Finally([self, key] {
                    // 任务结束后，回到 strand 清理 map
                    boost::asio::post(self->strand_, [self, key] {
                        self->creation_in_progress_.erase(key);
                        SPDLOG_DEBUG("连接 '{}' 的创建占位符已被移除。", key);
                    });
                });

                try {
                    new_conn = co_await self->create_new_connection(key, scheme_s, host_s, port);
                } catch (...) {
                    // 3. 在 catch 块中，只捕获异常指针，不做任何协程操作
                    exception = std::current_exception();
                }

                // 回到 strand 来处理（无论是成功还是失败
                co_await boost::asio::post(self->strand_, boost::asio::use_awaitable);
                if (exception) {
                    // 如果捕获到了异常，设置异常结果
                    state->result = exception;
                } else {
                    self->add_connection_to_pool(key, new_conn);
                    state->result = new_conn;
                }
                // c. 工作完成，关闭 channel 作为广播信号，唤醒所有等待者
                state->signal->close();
            },
            boost::asio::detached
        );
    }

    // --- B. 无论是创建者还是等待者，都在这里等待结果 ---
    auto state_to_wait_on = it->second;
    SPDLOG_DEBUG("等待连接 '{}' 创建完成...", key);

    // 离开 strand 去等待 channel 信号
    co_await boost::asio::post(ioc_, boost::asio::use_awaitable);

    // 等待 channel 被关闭。我们忽略接收到的错误码(通常是 eof)。
    // as_tuple 确保即使 channel 关闭，协程也能正常恢复而不是抛异常。
    co_await state_to_wait_on->signal->async_receive(boost::asio::as_tuple(boost::asio::use_awaitable));

    // 信号已收到，回到 strand 上安全地读取结果
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);

    // 检查结果
    if (auto* conn_ptr = std::get_if<std::shared_ptr<IConnection>>(&state_to_wait_on->result)) {
        // 成功
        co_return PooledConnection{*conn_ptr, false};
    } else if (auto* ex_ptr = std::get_if<std::exception_ptr>(&state_to_wait_on->result)) {
        // 失败
        std::rethrow_exception(*ex_ptr);
    } else {
        // 逻辑错误，不应该发生
        throw std::logic_error("Connection creation finished but no result was set.");
    }
}

void ConnectionManager::add_connection_to_pool(const std::string& key, std::shared_ptr<IConnection> conn) {
    if (conn->supports_multiplexing()) {
        h2_pool_[key].push_back(conn);
    } else {
        // pool_[key].push_back(conn);
    }
}

void ConnectionManager::release_connection(const std::shared_ptr<IConnection>& conn) {
    // 同样，将释放操作调度到 strand 上
    if (!conn) { return; }
    post(strand_, [self = shared_from_this(), conn, this]() {
        // 对于 H2 连接，release 实际上是空操作，因为它的状态由内部计数器管理
        // 我们只需要处理丢弃逻辑
        if (conn->supports_multiplexing()) {
            if (!conn->is_usable()) {
                SPDLOG_DEBUG("Discarding H2 connection {}.", conn->id());
                // 从 H2 列表中移除
                auto it = h2_pool_.find(conn->get_pool_key());
                if (it != h2_pool_.end()) {
                    auto& h2_conns = it->second;
                    std::erase(h2_conns, conn);

                    // 如果 vector 变为空，选择删除整个 key
                    if (h2_conns.empty()) {
                        h2_pool_.erase(it);
                    }
                }
            }
            return; // H2 连接不归还，它一直在“池”里
        }

        // 对于 H1.1 连接
        if (!conn->is_usable()) {
            SPDLOG_DEBUG("丢弃连接 {}", conn->id());
            // conn 的 shared_ptr 在 lambda 结束时被销毁，自动触发关闭
            return;
        }
        const auto& key = conn->get_pool_key();

        pool_[key].push_back(conn); // 放回队尾，实现 LIFO/FIFO 策略
        SPDLOG_DEBUG("将连接 {} 存入连接池 [{}],当前连接池状况Key = {}， {}", conn->id(), key, pool_[key].size(), pool_[key].back()->get_pool_key());
    });
}


boost::asio::awaitable<std::shared_ptr<IConnection>> ConnectionManager::create_new_connection(const std::string& key, std::string_view scheme, std::string_view host, uint16_t port) {
    // 这个函数总是被 get_connection 在 strand 上调用，所以内部是安全的

    try {
        // 1. DNS 解析
        //auto endpoints = co_await resolver_.async_resolve(tcp::v4(), host, std::to_string(port), boost::asio::use_awaitable);
        boost::system::error_code ec;
        results_type endpoints = co_await resolver_.async_resolve(tcp::v4(), host, std::to_string(port), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec) throw boost::system::system_error(ec, "DNS resolution failed");

        if (scheme == "http:") {
            // 2) TCP 连接（迭代器重载 + redirect_error）
            ec.clear();
            tcp::socket socket(ioc_);

            co_await async_connect(socket, endpoints.begin(), endpoints.end(), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            if (ec) { throw boost::system::system_error(ec, "async_connect failed"); }

            co_return std::make_shared<Http1Connection>(std::move(socket), key);
        } else if (scheme == "https:") {
            // 2. 创建 stream
            auto stream = std::make_shared<boost::beast::ssl_stream<tcp::socket>>(ioc_, ssl_ctx_);

            // 3. 建立 TCP 连接
            ec.clear();
            co_await async_connect(get_lowest_layer(*stream), endpoints.begin(), endpoints.end(), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            if (ec) throw boost::system::system_error(ec, "tcp async_connect failed");

            std::string host_str(host);
            if (!SSL_set_tlsext_host_name(stream->native_handle(), host_str.c_str())) {
                throw boost::system::system_error(static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category());
            }

            // TLS 握手（redirect_error）
            ec.clear();
            co_await stream->async_handshake(boost::asio::ssl::stream_base::client, boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            if (ec) throw boost::system::system_error(ec, "tls async_handshake failed");

            const unsigned char* proto = nullptr;
            unsigned int len = 0;
            SSL_get0_alpn_selected(stream->native_handle(), &proto, &len);

            if (proto && std::string_view((const char*)proto, len) == "h2") {
                SPDLOG_INFO("ALPN为 {} 选择了H2协议创建连接.", host);
                // a. 创建 Actor 对象
                auto conn = Http2Connection::create(stream, key);

                // b. 调用同步的 run() 方法来启动后台 actor_loop
                conn->run();

                // c. **立即返回**。我们不再等待 H2 握手完成。
                //    H2 握手现在是 actor_loop 内部的第一阶段。
                //    第一个 `execute` 请求会被 channel 自动“缓冲”，
                //    直到 actor_loop 完成握手并开始处理请求消息。
                co_return conn;
            } else {
                SPDLOG_INFO("ALPN selected http/1.1 for {}. Creating HttpSslConnection.", host);
                co_return std::make_shared<HttpSslConnection>(stream, key);
            }
        }
        throw std::runtime_error("Unsupported scheme: " + std::string(scheme));
    } catch (const boost::system::system_error& e) {
        SPDLOG_ERROR("Failed to create new connection to {}: {}", key, e.what());
        throw;
    }
}
