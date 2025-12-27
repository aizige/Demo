//
// Created by Aiziboy on 2025/11/16.
//

#include <aizix/core/Server.hpp>

#include <aizix/http/request_context.hpp>
#include <aizix/utils/finally.hpp>
#include <aizix/http/http_common_types.hpp>
#include <aizix/http/network_constants.hpp>
#include <aizix/version.hpp>
#include <aizix/utils/config/AizixConfig.hpp>
#include <aizix/http/router.hpp>
#include <aizix/core/h2_session.hpp>
#include <aizix/core/HttpSession.hpp>
#include <aizix/core/HttpsSession.hpp>

#include <fstream>
#include <iostream>
#include <vector>
#include <set>
#include <unordered_set>
#include <spdlog/spdlog.h>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>     // Asio 的 SSL/TLS 功能
#include <boost/beast/http.hpp>   // Beast 的 HTTP/1.1 功能
#include <boost/asio/experimental/parallel_group.hpp> // 引入 parallel_group
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <aizix/App.hpp>


// 命名空间别名
namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;
using namespace std::literals::chrono_literals;
using namespace boost::asio::experimental::awaitable_operators;
/**
 * @brief 构造函数 (带 IP 和端口)。
 * @param app 应用程序实例引用 (用于获取 IO Context 池)
 */
Server::Server(aizix::App& app)
    : app_(app), // 保存 App 引用
      work_executor_(app.worker_pool_executor()),
      ssl_context_(boost::asio::ssl::context::tlsv12_server), // 先用 io_context 构造 acceptor
      use_ssl_(app.config().server.ssl->enabled),
      max_request_body_size_bytes_(app.config().server.max_request_size_bytes),
      http2_enabled_(app.config().server.http2_enabled),
      tls_versions_(app.config().server.ssl->tls_versions),
      initial_timeout_ms_(std::chrono::seconds(10s)),
      keep_alive_timeout(app.config().server.keep_alive_ms) {
    // 1. 设置 TLS (如果启用)
    if (app.config().server.ssl && app.config().server.ssl->enabled) {
        set_tls(app.config().server.ssl->cert.value(), app.config().server.ssl->cert_private_key.value());
    }

    // 2. 初始化所有 Acceptor (SO_REUSEPORT 核心逻辑)
    setup_acceptor(app.config().server.port, app.config().server.ip_v4);
}

/**
 *  @brief 获取对内部路由器的引用。
 *  允许外部代码（如 `main.cpp`）向服务器注册路由。
 *  @return Router&
 */
Router& Server::router() {
    return router_;
}

/**
 * @brief 配置服务器以启用 TLS (HTTPS, H2)。
 * @param cert_file PEM 格式的证书链文件路径。
 * @param key_file PEM 格式的私钥文件路径。
 * @note [逻辑问题]：此函数在加载失败时不会抛出异常，而是静默地将服务器
 * 降级为 HTTP 模式，这在生产环境中可能导致严重的安全风险。
*/
void Server::set_tls(const std::string& cert_file, const std::string& key_file) {
    // 加载证书和私钥到 SSL 上下文管理器
    try {
        // 启动前检查文件是否存在，如果不存在则抛出异常，使服务器启动失败
        if (!std::ifstream(cert_file))
            throw std::runtime_error("未找到证书: " + cert_file);
        if (!std::ifstream(key_file))
            throw std::runtime_error("未找到证书密钥: " + key_file);


        // --- 设置安全选项 ---
        // 这是增强服务器安全性的重要步骤
        ssl_context_.set_options(network::ssl::CONTEXT_OPTIONS);

        // 动态设置协议版本范围

        network::ssl::configure_tls_versions(ssl_context_.native_handle(), tls_versions_);

        // --- 加载证书和私钥 ---
        ssl_context_.use_certificate_chain_file(cert_file);
        ssl_context_.use_private_key_file(key_file, boost::asio::ssl::context::pem);

        // 加入额外的 TLS 安全选项
        // 禁止 TLS 会话重协商，这可以防止一种潜在的 DoS 攻击
        SSL_CTX_set_options(ssl_context_.native_handle(), SSL_OP_NO_RENEGOTIATION);

        // --- 设置 ALPN (Application-Layer Protocol Negotiation) 回调函数 ---
        // ALPN 是 TLS 握手期间的一个扩展，允许客户端和服务器协商接下来要使用的应用层协议
        // (例如，是使用 HTTP/2 还是 HTTP/1.1)。
        // 我们需要直接调用 OpenSSL 的底层 API 来设置这个回调。
        // `native_handle()` 返回底层的 `SSL_CTX*` 指针。
        SSL_CTX_set_alpn_select_cb(ssl_context_.native_handle(), alpn_select_callback, this);

        use_ssl_ = true;
    } catch (std::exception& e) {
        use_ssl_ = false;
        SPDLOG_ERROR("开启SSL失败，已经默认使用HTTP: {}", e.what());
        throw std::runtime_error("TLS 配置失败。服务器无法以 HTTPS 模式启动。");
    }
}

/**
 * @brief 启动服务器的监听循环。
 * 这是一个非阻塞操作，它会为每个 Acceptor 启动一个监听协程来处理新连接
 */
void Server::run() {
    // 遍历所有 acceptor
    for (size_t i = 0; i < acceptors_.size(); ++i) {
        auto& acceptor = acceptors_[i];
        // 获取该线程专属的 Context 指针
        // thread_contexts_ 在 setup_acceptor 中已经按顺序初始化
        ThreadContext* ctx = thread_contexts_[i].get();

        auto& ioc = acceptor.get_executor();

        // 传递 ctx 指针
        boost::asio::co_spawn(ioc, listener(acceptor, ctx), boost::asio::detached);
    }
    SPDLOG_INFO("服务器在 {} 个 IO 线程上运行，并启用了 SO_REUSEPORT。", acceptors_.size());
}


/**
 * @brief 分布式优雅停机
 *
 * 难点：如何安全地停止并销毁运行在多个线程上的成千上万个协程？
 * 解决方案：
 * 1. 禁止新连接 (Close Acceptors)。
 * 2. 广播任务：要求每个线程清理自己的 Session。
 * 3. 引用计数栅栏 (active_coroutine_count)：主线程必须等待所有子协程（包括 detached 的关闭任务）
 *    彻底析构栈内存后，才能返回。否则 App 析构导致 io_context 销毁，会导致 SEGV。
 */
boost::asio::awaitable<void> Server::stop() {
    // 1. 设置停止标志，通知 listener、handle_connection 不要再处理新连接了
    is_stopping_ = true;

    // 2. 立即关闭所有 Acceptor，即刻停止接受新连接，防止在关闭过程中有新会话建立。
    SPDLOG_INFO("停止 {} acceptors...", acceptors_.size());
    for (auto& acceptor : acceptors_) {
        if (acceptor.is_open()) {
            boost::system::error_code ec;
            acceptor.close(ec); // 忽略错误
        }
    }
    SPDLOG_INFO("Server 已停止接受新连接");

    // 3. 准备收集所有线程的关闭任务
    const auto& io_contexts = app_.get_io_contexts();
    std::vector<boost::asio::awaitable<void>> stop_tasks;
    stop_tasks.reserve(io_contexts.size());

    // 4. 为每个 IO 线程派发清理任务
    for (size_t i = 0; i < io_contexts.size(); ++i) {
        ThreadContext* ctx = thread_contexts_[i].get();
        auto& ioc = *io_contexts[i];

        // 在目标 IO 线程上执行清理逻辑
        // 这是一个运行在目标 IO 线程上的主控协程
        auto task = co_spawn(ioc, [ctx, i]() -> boost::asio::awaitable<void> {
            // --- Step A: 暴力关闭 HTTP/1 和 HTTPS ---
            // 它们通常无状态或不支持应用层 GOAWAY，直接断开 TCP
            // 关闭 HTTP/1
            for (auto& s : ctx->h1_sessions) s->stop();
            ctx->h1_sessions.clear();

            // 关闭 HTTPS
            for (auto& s : ctx->https_sessions) s->stop();
            ctx->https_sessions.clear();

            // --- Step B: 收集活跃的 H2 Session ---
            // 将 weak_ptr 转为 shared_ptr，只有活着的 session 才需要优雅关闭
            std::vector<std::shared_ptr<Http2Session>> h2_alive;
            for (auto& w : ctx->h2_sessions) {
                if (auto s = w.lock()) h2_alive.push_back(s);
            }

            // 清空容器，断开引用
            ctx->h2_sessions.clear();

            // 如果没有 H2 会话，本线程的任务直接结束
            if (h2_alive.empty()) co_return;

            // 如果有活跃的 H2 会话，启动并行关闭流程
            if (!h2_alive.empty()) {
                SPDLOG_INFO("IO 线程 {} 正在关闭 {} 个 H2 session...", i, h2_alive.size());

                auto ex = co_await boost::asio::this_coro::executor;

                // Channel 用于同步：等待所有 detached 协程发回“我做完了”的信号
                auto completion_channel = std::make_shared<boost::asio::experimental::channel<void(boost::system::error_code)>>(ex, h2_alive.size());

                // --- Step C: 为每个 H2 Session 启动一个 Detached 协程进行关闭 ---
                for (const auto& session : h2_alive) {
                    co_spawn(ex,
                             [session, completion_channel, i, ctx]() -> boost::asio::awaitable<void> {
                                 // RAII 计数器保护
                                 // 只要这个协程还在栈上（还在运行或析构中），计数器就 > 0
                                 // 计数器 +1
                                 ++ctx->active_coroutine_count;
                                 // 计数器 -1 (无论如何都会执行)
                                 [[maybe_unused]] auto guard = Finally([ctx] { --ctx->active_coroutine_count; });

                                 // 尝试优雅关闭
                                 // 我们给优雅关闭本身加一个超时，而不是在外部加总超时
                                 // 这样每个协程都是独立的，都能保证在有限时间内结束
                                 boost::asio::steady_timer self_timer(co_await boost::asio::this_coro::executor);
                                 self_timer.expires_after(5s);

                                 try {
                                     // 调用 Session 的优雅关闭 (发送 GOAWAY, 等待 Stream 结束), 竞争：优雅关闭 vs 超时
                                     const auto res = co_await (session->graceful_shutdown() || self_timer.async_wait(boost::asio::use_awaitable));
                                     if (res.index() == 1) session->stop(); // 自身超时，强制关闭
                                 } catch (const std::exception& e) {
                                     SPDLOG_DEBUG("IO 线程 {} 关闭 H2 session 发生错误: {}", i, e.what());
                                     // 如果优雅关闭失败，强制关闭
                                     session->stop();
                                 } catch (...) {
                                     SPDLOG_ERROR("IO 线程 {} 关闭 H2 session 发生未知错误...", i);
                                     session->stop();
                                 }
                                 // 无论成功失败，发送完成信号
                                 co_await completion_channel->async_send(boost::system::error_code{}, boost::asio::use_awaitable);
                             },
                             boost::asio::detached
                    );
                }

                // --- Step D: 等待收到所有信号 ---
                // 此时，所有 detached 协程的逻辑已跑完，但栈变量可能还在析构中
                for (size_t k = 0; k < h2_alive.size(); ++k) {
                    co_await completion_channel->async_receive(boost::asio::use_awaitable);
                }
            }

            // --- Step E: 最后的防线 (Reference Counting Barrier) ---
            // 即使收到了所有信号，detached 协程的栈帧可能还没完全释放。
            // 必须轮询计数器直到归零。如果这里直接返回，io_context 销毁会导致 Use-After-Free 崩溃。
            if (ctx->active_coroutine_count > 0) {
                SPDLOG_DEBUG("IO 线程 {} 正在等待 {} 个协程终止……", i, ctx->active_coroutine_count);
                boost::asio::steady_timer flush_timer(co_await boost::asio::this_coro::executor);
                while (ctx->active_coroutine_count > 0) {
                    // 短轮询，等待栈展开完成
                    flush_timer.expires_after(10ms);
                    co_await flush_timer.async_wait(boost::asio::use_awaitable);
                }
            }
            SPDLOG_DEBUG("IO 线程 {} 退场...", i);
        }, boost::asio::use_awaitable);

        stop_tasks.push_back(std::move(task));
    }

    // 4. 主线程阻塞等待所有任务完成
    for (auto& task : stop_tasks) {
        co_await std::move(task);
    }

    SPDLOG_INFO("所有 IO 线程中的所有连接均正常停止。");
}


/**
 * @brief 监听协程
 * 职责单一化：只负责 Accept，然后立即分连接
 */
boost::asio::awaitable<void> Server::listener(boost::asio::ip::tcp::acceptor& acceptor, ThreadContext* ctx) {
    // 获取当前协程的执行器，用于派生新的协程
    const auto exec = co_await boost::asio::this_coro::executor;

    // 定义一个定时器用于处理 accept 失败时的休眠
    boost::asio::steady_timer timer(exec);

    for (;;) // 无限循环以持续接受连接
    {
        try {
            // 循环开始前检查停止标志
            if (is_stopping_) co_return;
            boost::system::error_code ec;

            // 直接在当前线程 accept。
            // 因为 acceptor 已经绑定到了当前线程的 io_context，
            // 且使用了 SO_REUSEPORT，内核会把连接分发到这里。

            // 构造一个 socket，绑定到当前线程的 executor (当前 io_context)
            tcp::socket socket(exec);

            // 异步等待并接受一个新的 TCP 连接
            // 直接把 socket 传给 accept
            // 这样 accept 成功后，socket 既拥有连接句柄，又绑定在 IO Worker context 线程池的线程上
            co_await acceptor.async_accept(socket, boost::asio::redirect_error(boost::asio::use_awaitable, ec));

            // 接受失败，记录日志并继续等待下一个连接
            if (ec) {
                if (ec == boost::asio::error::operation_aborted) {
                    SPDLOG_INFO("Listener 函数停止 (acceptor closed)");
                    co_return;
                }

                SPDLOG_WARN("与客户端建立新的连接失败: {}", ec.message());

                if (ec == boost::asio::error::no_descriptors ||
                    ec == boost::asio::error::no_buffer_space ||
                    ec == boost::asio::error::connection_aborted ||
                    ec == boost::system::errc::too_many_files_open) {
                    timer.expires_after(100ms);
                    co_await timer.async_wait(boost::asio::use_awaitable);
                }
                continue;
            }

            // 启动连接处理协程
            // 使用 detached 模式，因为每个连接独立运行，不阻塞 listener
            boost::asio::co_spawn(
                exec, // 使用了 SO_REUSEPORT 继续在当前线程运行
                [this, s = std::move(socket),ctx]() mutable -> boost::asio::awaitable<void> {
                    return handle_connection(std::move(s), ctx);
                },
                boost::asio::detached
            );
        } catch (const std::exception& e) {
            // 捕获异常后只打印日志，循环继续执行，确保服务器继续监听
            SPDLOG_ERROR("Server 监听循环迭代中发生异常: {}", e.what());
        }
    }
}

/**
 * @brief 连接处理协程
 */
boost::asio::awaitable<void> Server::handle_connection(boost::asio::ip::tcp::socket socket, ThreadContext* ctx) {
    try {
        // [RAII 计数器]
        // 确保此协程存在期间，ThreadContext 中的 active_coroutine_count 不为 0
        // 这会阻止 Server::stop() 提前退出，防止崩溃
        ++ctx->active_coroutine_count; // 计数器 +1
        // 使用 finally 确保协程退出时计数器 -1
        [[maybe_unused]] auto guard = Finally([ctx] { --ctx->active_coroutine_count; });

        // 早期检查：如果正在停止，直接退出，不要进行握手
        if (is_stopping_) co_return;

        // 性能优化: TCP_NODELAY 对 HTTP/2 性能至关重要 (减少小包延迟)
        boost::system::error_code ec;
        socket.set_option(boost::asio::ip::tcp::no_delay(true), ec);
        if (ec) {
            SPDLOG_WARN("性能优化, 设置 TCP_NODELAY 失败: {}", ec.message());
        }

        // SPDLOG_INFO("Handle connection start. Socket open: {}", socket.is_open());

        if (!use_ssl_) {
            // ... HTTP/1 处理 ...
            if (is_stopping_) co_return;

            const auto session = std::make_shared<HttpSession>(
                std::move(socket), router_, work_executor_,
                max_request_body_size_bytes_, initial_timeout_ms_, keep_alive_timeout);

            // 无锁插入
            ctx->h1_sessions.insert(session);

            // 启动/运行 Session(当前线程)
            try {
                co_await session->run();
            } catch (const std::exception& e) {
                SPDLOG_ERROR("HttpSession run error: {}", e.what());
            }

            // 无锁移除
            ctx->h1_sessions.erase(session);

            co_return;
        }

        // --- SSL/TLS 连接处理流程 ---
        // 1. 创建一个 SSL 流，将原始 TCP 套接字包装起来
        auto tls_stream = std::make_shared<boost::asio::ssl::stream<tcp::socket>>(std::move(socket), ssl_context_);

        // 在 Worker 线程执行握手，不阻塞 Main Thread
        co_await tls_stream->async_handshake(boost::asio::ssl::stream_base::server, // 以服务器模式进行握手
                                             boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        if (ec) {
            // 这里可以根据具体的 SSL 错误码判断是否是协议错配
            // 这里管你三七二十一只要遇到错误就返回这个

            // 我们现在拥有底层的 tcp::socket，可以用来发送明文响应
            auto& sock = tls_stream->next_layer();

            HttpResponse resp{http::status::bad_request, 11};
            resp.set(http::field::content_type, "text/html");
            resp.set(http::field::connection, "close");
            resp.set(http::field::server, aizix::framework::name + "/" + aizix::framework::version);
            resp.body() = "<html><body><h1>400 Bad Request</h1>"
                "<p>This port requires HTTPS, but a plain HTTP request was received.</p>"
                "</body></html>";
            resp.prepare_payload();

            // 在原始 socket 上发送明文响应
            // 我们需要忽略写操作的错误，因为对方可能已经不等响应就关闭了连接
            co_await http::async_write(sock, resp, boost::asio::as_tuple(boost::asio::use_awaitable));
            SPDLOG_ERROR("TLS 握手失败（可能是 HTTP 请求在 HTTPS 端口上发起），来自 {}: {}", tls_stream->next_layer().remote_endpoint().address().to_string(), ec.message());
            // 无论如何都关闭连接
            sock.close();
            co_return; // 握手失败，放弃此连接
        }

        // 握手是耗时的，握手回来后再次检查是否停止
        // 如果此时 Server 已经开始 stop，这个连接就没有注册到 sessions 列表中
        // 我们必须在这里拦截它，否则它会变成“僵尸连接”导致崩溃
        // 必须检查！否则会发生 Use-After-Free 崩溃
        if (is_stopping_) co_return;

        // 3. ALPN 协议协商：检查客户端和服务器共同选择的应用层协议
        const unsigned char* proto = nullptr;
        unsigned int len = 0;
        // 从 SSL 对象中获取协商结果
        SSL_get0_alpn_selected(tls_stream->native_handle(), &proto, &len);
        // ReSharper disable once CppTooWideScopeInitStatement
        const std::string_view alpn{reinterpret_cast<const char*>(proto), len};

        SPDLOG_INFO("ALPN 协商结果: {}", alpn);
        // 4. 根据协商的协议，将连接分发给不同的处理器
        if (alpn == "h2") {
            const auto session = Http2Session::create(tls_stream, work_executor_, router_, max_request_body_size_bytes_, keep_alive_timeout);

            SPDLOG_INFO("H2: 准备注册 Session...");

            // 将 session 的 weak_ptr 转换为 shared_ptr 添加 Session 到列表中存起来
            // 这样我们就有了一个可以安全地从集合中移除自身的 token
            const auto weak_session_ptr = std::weak_ptr<Http2Session>(session);


            // --- 直接注册 ---
            ctx->h2_sessions.insert(weak_session_ptr);

            SPDLOG_INFO("H2: 注册完成，准备 Start...");

            // 启动 session 的处理循环 (Worker Thread)
            try {
                co_await session->start();
            } catch (const std::exception& e) {
                SPDLOG_DEBUG("H2 error: {}", e.what());
                throw;
            }

            // 直接清理
            ctx->h2_sessions.erase(weak_session_ptr);
        } else {
            // 回退到处理 HTTPS的逻辑

            // HTTPS 逻辑
            const auto session = std::make_shared<HttpsSession>(std::move(tls_stream), router_, work_executor_, max_request_body_size_bytes_, initial_timeout_ms_, keep_alive_timeout);

            // 注册
            ctx->https_sessions.insert(session);

            //  启动session
            try {
                co_await session->run();
            } catch (const std::exception& e) {
                SPDLOG_ERROR("Https Session run error: {}", e.what());
                throw;
            }

            // --- 直接移除 ---
            ctx->https_sessions.erase(session);
        }
    } catch (...) {
        throw;
    }
}

/**
 * @brief 初始化 Acceptor vector数组 (SO_REUSEPORT)
 *
 * 传统的 "One Acceptor dispatch to Workers" 模式在高并发下存在锁竞争和跨线程开销。
 * 这里为 App 中的 **每个 IO 线程** 创建一个独立的 acceptor，并全部监听 **同一个端口**。
 */
void Server::setup_acceptor(const uint16_t port, const std::string& ip) {
    boost::system::error_code ec;

    // 1. 创建 endpoint
    auto const address = boost::asio::ip::make_address(ip, ec);
    if (ec) {
        throw std::runtime_error("Invalid IP address provided: " + ip);
    }
    const tcp::endpoint endpoint(address, port);

    // 获取 App 管理的所有 IO Context (每个都绑定了一个 OS 线程)
    const auto& io_contexts = app_.get_io_contexts();

    // 预分配 vector
    acceptors_.reserve(io_contexts.size());
    thread_contexts_.reserve(io_contexts.size());

    // 遍历每一个 io_context
    for (const auto& ioc : io_contexts) {
        // 1. 初始化 Acceptor，绑定到特定的 io_context
        acceptors_.emplace_back(*ioc);
        auto& acceptor = acceptors_.back();

        // 2. 初始化该线程的无锁上下文
        thread_contexts_.push_back(std::make_unique<ThreadContext>());

        // 3. 打开并配置 Socket
        acceptor.open(endpoint.protocol(), ec);
        if (ec) throw std::runtime_error("Acceptor open failed: " + ec.message());
        // 配置 Socket 选项
        acceptor.set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (ec) {
            std::cerr << "Warning: Failed to set reuse_address option: " << ec.message() << std::endl; // 通常这不是一个致命错误，可以只打印警告
            ec.clear();
        }

        // --- 设置 SO_REUSEPORT ---
        // 允许不同线程的 socket 绑定到完全相同的 IP:PORT。
        // Linux 内核会自动进行负载均衡 (Sharding)，将连接分发给不同线程的 accept 队列。
        #if defined(SO_REUSEPORT)
        // 使用 setsockopt 的底层封装
        using reuse_port = boost::asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>;
        acceptor.set_option(reuse_port(true), ec);
        if (ec) {
            SPDLOG_CRITICAL("SO_REUSEPORT set failed! Kernel too old? Error: {}", ec.message());
            throw std::runtime_error("SO_REUSEPORT required.");
        }
        #else
        #error "SO_REUSEPORT not supported on this platform."
        #endif

        // 绑定
        acceptor.bind(endpoint, ec);
        if (ec) throw std::runtime_error("Failed to bind to endpoint " + ip + ":" + std::to_string(port) + ". Error: " + ec.message());

        // 监听
        acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec) throw std::runtime_error("Listen failed: " + ec.message());
    }

    std::cout << "Success: Server listening on " << endpoint << " with " << acceptors_.size() << " acceptors (SO_REUSEPORT)." << std::endl;
}

/**
* @brief 回调函数，用于在 TLS 握手期间选择一个应用层协议。
* @param ssl OpenSSL 的 SSL 对象指针。
* @param out 用于存放服务器选择的协议的指针。
* @param out_len 用于存放服务器选择的协议的长度。
* @param in 客户端提供的协议列表。
* @param in_len 客户端协议列表的总长度。
* @param arg 用户自定义参数（在此未使用）。
* @return `SSL_TLSEXT_ERR_OK` 表示成功选择了一个协议，
*         `SSL_TLSEXT_ERR_NOACK` 表示没有找到共同支持的协议。
*/
int Server::alpn_select_callback(SSL* ssl, const unsigned char** out, unsigned char* out_len, const unsigned char* in, const unsigned int in_len, void* arg) {
    const Server* self = static_cast<Server*>(arg);
    if (!self) {
        SPDLOG_ERROR("ALPN FATAL: `this` pointer (arg) is NULL!");
        return SSL_TLSEXT_ERR_NOACK;
    }
    //SPDLOG_DEBUG("ALPN INFO: `this` pointer is valid: {}", fmt::ptr(self));

    // 2. 打印客户端提供的协议列表
    //SPDLOG_DEBUG("ALPN INFO: Client offered protocols ({} bytes): {}", in_len, log_alpn_protos(in, in_len));

    // 3. 确定并打印服务器将要提供的协议列表
    const unsigned char* server_protos_data;
    size_t server_protos_len;

    //SPDLOG_DEBUG("ALPN INFO: Server's http2_enabled_ flag is: {}", self->http2_enabled_);

    if (self->http2_enabled_) {
        server_protos_data = network::alpn::PROTOS_H2_PREFERRED;
        server_protos_len = sizeof(network::alpn::PROTOS_H2_PREFERRED);
    } else {
        server_protos_data = network::alpn::PROTOS_H1_ONLY;
        server_protos_len = sizeof(network::alpn::PROTOS_H1_ONLY);
    }
    //SPDLOG_DEBUG("ALPN INFO: Server will offer protocols ({} bytes): {}", server_protos_len, log_alpn_protos(server_protos_data, server_protos_len));

    // 4. 调用 OpenSSL 函数进行协商
    //SPDLOG_DEBUG("ALPN INFO: Calling SSL_select_next_proto...");
    if (SSL_select_next_proto(const_cast<unsigned char**>(out), out_len, server_protos_data, server_protos_len, in, in_len) == OPENSSL_NPN_NEGOTIATED) {
        //SPDLOG_INFO("ALPN SUCCESS: A common protocol was selected: {}", std::string(reinterpret_cast<const char*>(*out), *out_len));
        //SPDLOG_DEBUG("--- ALPN Callback Finished (Success) ---");
        return SSL_TLSEXT_ERR_OK;
    }

    // 5. 如果协商失败
    SPDLOG_WARN("ALPN FAILURE: No common protocol could be selected by SSL_select_next_proto.");
    SPDLOG_DEBUG("--- ALPN Callback Finished (Failure) ---");
    return SSL_TLSEXT_ERR_NOACK;
}
