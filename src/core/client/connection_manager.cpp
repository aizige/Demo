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


ConnectionManager::ConnectionManager(boost::asio::io_context &ioc, bool enable_maintenance)
    : ioc_(ioc),
      strand_(ioc.get_executor()),
      ssl_ctx_(boost::asio::ssl::context::sslv23_client),
      resolver_(ioc),
      maintenance_timer_(ioc) {
    // 配置 SSL 上下文
    ssl_ctx_.set_options(
        boost::asio::ssl::context::default_workarounds |
        boost::asio::ssl::context::no_sslv2 |
        boost::asio::ssl::context::no_sslv3 |
        boost::asio::ssl::context::no_tlsv1 |
        boost::asio::ssl::context::no_tlsv1_1 |
        boost::asio::ssl::context::single_dh_use
    );
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
    stop();
}

void ConnectionManager::stop() {
    // 使用 post 确保在 strand 上安全地修改 stopped_ 标志
    boost::asio::post(strand_, [this] {
        if (!stopped_) {
            stopped_ = true;
            maintenance_timer_.cancel();
        }
    });
}


void ConnectionManager::start_maintenance() {
    // 启动后台协程，它将独立运行
    boost::asio::co_spawn(ioc_, run_maintenance(), boost::asio::detached);
}


boost::asio::awaitable<void> ConnectionManager::run_maintenance() {
    while (!stopped_) {
        maintenance_timer_.expires_after(std::chrono::seconds(35)); // 每 15 秒维护一次

        // 使用 redirect_error 来忽略 timer 被 cancel 时的异常
        boost::system::error_code ec;
        co_await maintenance_timer_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        // 检查是否是由于 stop() 导致的退出
        if (stopped_) co_return;

        // 将维护工作 post 到 strand 上
        co_await boost::asio::post(strand_, boost::asio::use_awaitable);


        // 遍历所有连接池
        for (auto &[key, queue]: pool_) {
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


boost::asio::awaitable<PooledConnection> ConnectionManager::get_connection(std::string_view scheme, std::string_view host, uint16_t port) {
    std::string key = std::string(scheme) + "//" + std::string(host) + ":" + std::to_string(port);

    // **核心**: 将所有操作都调度到 strand 上，保证绝对的线程安全
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);

    // --- 我们现在在 strand 上，可以安全地访问 pool_ ---
    // --- 策略 1: 优先复用一个现有的 H2 连接 ---
    auto h2_it = h2_pool_.find(key);
    if (h2_it != h2_pool_.end()) {
        auto &h2_conns = h2_it->second;
        SPDLOG_DEBUG("H2 connection pool [{}], size = {}", key, h2_conns.size());
        std::shared_ptr<IConnection> best_conn = nullptr;
        size_t min_streams = SIZE_MAX;

        // 遍历该主机的所有 H2 连接，找到最空闲的一个
        for (const auto &conn: h2_conns) {
            if (conn->is_usable()) {
                size_t current_streams = conn->get_active_streams();
                if (current_streams < conn->get_max_concurrent_streams() && current_streams < min_streams) {
                    best_conn = conn;
                    min_streams = current_streams;
                }
            }
        }
        if (best_conn) {
            SPDLOG_DEBUG("Multiplexing on H2 connection {} ({} active streams)", best_conn->id(), min_streams);
            co_return PooledConnection{best_conn, true};
        }
    }

    // --- 策略 2: 其次，从池中取一个空闲的 H1.1 连接 ---
    auto h1_it = pool_.find(key);
    if (h1_it != pool_.end()) {
        auto &h1_queue = h1_it->second;
        SPDLOG_DEBUG("H1 connection pool [{}], size = {}", key, h1_queue.size());
        while (!h1_queue.empty()) {
            auto conn = std::move(h1_queue.front());
            h1_queue.pop_front();
            if (conn->is_usable() && conn->get_active_streams() <= 0) {
                SPDLOG_DEBUG("复用池中的 H1 连接 {}", conn->id());
                co_return PooledConnection{conn, true};
            }
            SPDLOG_DEBUG("从连接池中删除为 {} 的陈旧连接 {}", conn->id(), key);
        }
    }

    // --- 池子为空，进入受保护的创建流程 ---

    // 3. 检查是否已有协程正在为这个 key 创建连接
    if (auto it = creation_channels_.find(key); it != creation_channels_.end()) {
        auto channel = it->second;
        SPDLOG_DEBUG("Connection for '{}' is being created by another task, waiting...", key);

        // **离开 strand 去等待结果**，避免死锁
        co_await boost::asio::post(ioc_, boost::asio::use_awaitable);

        auto [ec, result] = co_await channel->async_receive(boost::asio::as_tuple(boost::asio::use_awaitable));

        if (ec) {
            throw boost::system::system_error(ec, "Failed while waiting for connection creation");
        }

        // 检查创建结果
        if (auto e_ptr_ptr = std::get_if<std::exception_ptr>(&result)) {
            std::rethrow_exception(*e_ptr_ptr); // 第一个创建者失败了，我们也跟着失败
        }

        // 第一个创建者成功了，它已经把连接放入了池中。
        // 我们需要重新调用 get_connection 来公平地从池中获取它。
        // 再次调用会重新进入 strand，是安全的。
        co_return co_await get_connection(scheme, host, port);
    }

    // 4. 我是第一个创建者，上锁！
    auto ex = co_await boost::asio::this_coro::executor;
    auto channel = std::make_shared<CreationChannel>(ex, 1); // 容量为 1 即可
    creation_channels_[key] = channel;

    // 使用 RAII guard 确保 channel 一定会被从 map 中移除
    auto guard = Finally([this, key] {
        boost::asio::post(strand_, [this, key] {
            post(strand_, [this, key] { creation_channels_.erase(key); }); // 确保 channel 会被清理
            SPDLOG_DEBUG("Creation channel for '{}' removed.", key);
        });
    });

    // 4. 离开 strand 去执行耗时的 I/O
    co_await boost::asio::post(ioc_, boost::asio::use_awaitable);


    SPDLOG_DEBUG("连接池中没有可用于主机: {} 的连接，正在创建新的连接。", key);
    auto new_conn = co_await create_new_connection(key, scheme, host, port);
    try {
        new_conn = co_await create_new_connection(key, scheme, host, port);
    } catch (...) {
        // **创建失败**: 通过 channel 广播异常
        auto e_ptr = std::current_exception();
        post(strand_, [channel, e_ptr] {
            channel->try_send(boost::system::error_code{}, ConnectionResult(e_ptr));
        });
        std::rethrow_exception(e_ptr); // 重新抛出，让当前协程失败
    }
    // **成功**: 回到 strand 上，把新连接放入池中，并通过 channel 通知所有等待者
    post(strand_, [this, new_conn, channel, key] {
        // 在 strand 上安全地更新共享状态
        if (new_conn->supports_multiplexing()) {
            h2_pool_[key].push_back(new_conn);
        } else {
           // h1_pool_[key].push_back(new_conn);
        }
        // 广播成功结果
        channel->try_send(boost::system::error_code{}, ConnectionResult(new_conn));
    });
    // 返回给我们自己的这个协程
    co_return PooledConnection{new_conn, true};
}

void ConnectionManager::release_connection(const std::shared_ptr<IConnection> &conn) {
    // 同样，将释放操作调度到 strand 上
    boost::asio::post(strand_, [this, conn]() {
        // 对于 H2 连接，release 实际上是空操作，因为它的状态由内部计数器管理
        // 我们只需要处理丢弃逻辑
        if (conn->supports_multiplexing()) {
            if (!conn->is_usable()) {
                SPDLOG_DEBUG("Discarding H2 connection {}.", conn->id());
                // 从 H2 列表中移除
                auto it = h2_pool_.find(conn->get_pool_key());
                if (it != h2_pool_.end()) {
                    auto &h2_conns = it->second;
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
        const auto &key = conn->get_pool_key();

        pool_[key].push_back(conn); // 放回队尾，实现 LIFO/FIFO 策略
        SPDLOG_DEBUG("将连接 {} 存入连接池 [{}],当前连接池状况Key = {}， {}", conn->id(), key, pool_[key].size(), pool_[key].back()->get_pool_key());
    });
}


boost::asio::awaitable<std::shared_ptr<IConnection> > ConnectionManager::create_new_connection(const std::string &key, std::string_view scheme, std::string_view host, uint16_t port) {
    // 这个函数总是被 get_connection 在 strand 上调用，所以内部是安全的

    try {
        // 1. DNS 解析
        auto endpoints = co_await resolver_.async_resolve(host, std::to_string(port), boost::asio::use_awaitable);

        if (scheme == "http:") {
            tcp::socket socket(ioc_);
            co_await async_connect(socket, endpoints, boost::asio::use_awaitable);
            co_return std::make_shared<Http1Connection>(std::move(socket), key);
        } else if (scheme == "https:") {
            // 2. 创建 stream
            auto stream = std::make_shared<boost::beast::ssl_stream<tcp::socket> >(ioc_, ssl_ctx_);

            // 3. 建立 TCP 连接
            co_await async_connect(get_lowest_layer(*stream), endpoints, boost::asio::use_awaitable);

            if (!SSL_set_tlsext_host_name(stream->native_handle(), host.data())) {
                throw boost::system::system_error(static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category());
            }

            co_await stream->async_handshake(boost::asio::ssl::stream_base::client, boost::asio::use_awaitable);

            const unsigned char *proto = nullptr;
            unsigned int len = 0;
            SSL_get0_alpn_selected(stream->native_handle(), &proto, &len);
            if (proto && std::string_view((const char *) proto, len) == "h2") {
                SPDLOG_INFO("ALPN selected h2 for {}. Creating Http2Connection.", host);
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
    } catch (const boost::system::system_error &e) {
        SPDLOG_ERROR("Failed to create new connection to {}: {}", key, e.what());
        throw;
    }
}
