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
#include <aizix/utils/cert_checker.hpp>  // 证书检查工具

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
 * @param config 配置文件
 */
Server::Server(aizix::App& app, const AizixConfig& config)
    : app_(app), // 保存 App 引用
      work_executor_(app.worker_pool_executor()),
      ssl_context_(boost::asio::ssl::context::tlsv12_server), // 先用 io_context 构造 acceptor
      acceptor_(app.get_main_ioc()),                          // Acceptor 必须绑定在 Main Context 上
      use_ssl_(config.server.ssl->enabled),
      // Strand 保护全局集合，绑定在 Main Context 上
      h2_strand_(boost::asio::make_strand(app.get_main_ioc())),
      https_strand_(boost::asio::make_strand(app.get_main_ioc())),
      h1_strand_(boost::asio::make_strand(app.get_main_ioc())),
      max_request_body_size_bytes_(config.server.max_request_size_bytes),
      http2_enabled_(config.server.http2_enabled),
      tls_versions_(config.server.ssl->tls_versions),
      initial_timeout_ms_(std::chrono::seconds(10s)),
      keep_alive_timeout(config.server.keep_alive_ms) {
    // 调用一个辅助函数来完成剩下的设置
    setup_acceptor(config.server.port, config.server.ip_v4);
    if (config.server.ssl && config.server.ssl->enabled) {
        set_tls(config.server.ssl->cert.value(), config.server.ssl->cert_private_key.value());
    }
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
        throw std::runtime_error("TLS configuration failed. Server cannot start in HTTPS mode.");
    }

    // 在启动时检查证书是否包含了所有必需的域名/IP，以进行部署验证
    CertChecker::inspect(cert_file, {"dev.myubuntu.com", "192.168.1.176"});
}

/**
 * @brief 启动服务器的监听循环。
 * 这是一个非阻塞操作，它会启动一个后台协程来处理连接接受。
 */
void Server::run() {
    // 启动一个常驻的监听协程 在 Main Context 上运行
    boost::asio::co_spawn(app_.get_main_ioc(), listener(), boost::asio::detached);
}

/**
 *
 * @brief [修复后] 异步地、优雅地关闭服务器上的所有活跃会话。
 * 这个协程是服务器优雅停机流程的关键部分。它会执行以下步骤：
 * 立即关闭 acceptor，停止接受任何新的客户端连接。
 * 安全地从会话管理列表中收集所有当前活跃的会话。
 * 并发地为每个活跃会话启动一个独立的 graceful_shutdown 协程。
 * 异步地等待所有这些关闭任务都完成后，此协程才会返回。
 * @note 调用者应该 co_await 这个函数，以确保在继续执行后续的清理
 * 操作（如停止 `io_context`）之前，所有网络会话都已完全关闭。
 * @note 目前此方法只处理 Http2Session。如果未来添加了其他需要
 * 优雅关闭的会话类型 (如 H2cSession)，应考虑引入一个
 * 通用的 IStoppable 接口来统一管理和关闭所有会话，
 * 以避免此函数中的代码重复。
 * @return 一个协程句柄，表示整个关闭流程。
 */
boost::asio::awaitable<void> Server::stop() {
    // 设置标志位，通知 listener 不要再处理新连接了
    is_stopping_ = true;

    // 1. 立即停止接受新连接，防止在关闭过程中有新会话建立。
    if (acceptor_.is_open()) {
        acceptor_.close();
        SPDLOG_INFO("Server stopped accepting new connections.");
    }
    // ========================================================================
    //  HTTP/1.1 普通会话的安全关闭
    // ========================================================================
    {
        auto [e_ptr, sessions_snapshot] = co_await boost::asio::co_spawn(h1_strand_, [&]() -> boost::asio::awaitable<std::vector<std::shared_ptr<HttpSession>>> {
            std::vector<std::shared_ptr<HttpSession>> snapshot;
            snapshot.reserve(http_sessions_.size());
            for (const auto& s : http_sessions_) {
                snapshot.push_back(s);
            }
            http_sessions_.clear();
            co_return snapshot;
        }, boost::asio::as_tuple(boost::asio::use_awaitable));

        // 在 strand 外部（并发地）调用 stop
        for (const auto& s : sessions_snapshot) {
            s->stop();
        }
        SPDLOG_INFO("Stopped {} HTTP sessions.", sessions_snapshot.size());
    }

    // ========================================================================
    //  HTTPS 会话的安全关闭
    // ========================================================================
    {
        auto [e_ptr, sessions_snapshot] = co_await boost::asio::co_spawn(https_strand_, [&]() -> boost::asio::awaitable<std::vector<std::shared_ptr<HttpsSession>>> {
            std::vector<std::shared_ptr<HttpsSession>> snapshot;
            snapshot.reserve(https_sessions_.size());
            for (const auto& s : https_sessions_) {
                snapshot.push_back(s);
            }
            https_sessions_.clear();
            co_return snapshot;
        }, boost::asio::as_tuple(boost::asio::use_awaitable));

        if (e_ptr) {
            SPDLOG_ERROR("Error retrieving HTTPS sessions snapshot.");
        } else {
            for (const auto& s : sessions_snapshot) s->stop();
            SPDLOG_INFO("Stopped {} HTTPS TLS sessions.", sessions_snapshot.size());
        }
    }

    // ========================================================================
    //  HTTP/2 会话的安全收集与优雅等待
    // ========================================================================
    // 获取快照

    auto [e_ptr, h2_sessions_to_stop] = co_await boost::asio::co_spawn(
        h2_strand_,
        [&]() -> boost::asio::awaitable<std::vector<std::shared_ptr<Http2Session>>> {
            std::vector<std::shared_ptr<Http2Session>> snapshot;
            for (const auto& weak_session : h2_sessions_) {
                if (auto session = weak_session.lock()) {
                    snapshot.push_back(session);
                }
            }
            h2_sessions_.clear();
            co_return snapshot;
        }, boost::asio::as_tuple(boost::asio::use_awaitable));

    // 如果出错或 H2 会话列表为空，直接返回
    if (e_ptr || h2_sessions_to_stop.empty()) {
        co_return;
    }


    // --- 并行等待所有 H2 会话优雅关闭 (代码逻辑复用你原有的 Channel 机制) ---

    auto ex = co_await boost::asio::this_coro::executor;
    auto completion_channel = std::make_shared<boost::asio::experimental::channel<void(boost::system::error_code)>>(
        ex, h2_sessions_to_stop.size()
    );

    SPDLOG_INFO("Waiting for {} H2 sessions to graceful shutdown...", h2_sessions_to_stop.size());

    for (const auto& session : h2_sessions_to_stop) {
        // 获取 endpoint 字符串用于日志，需防范 session 已失效（虽然 shared_ptr 保证了存活）
        std::string endpoint_str = "unknown";
        try { endpoint_str = session->remote_endpoint().address().to_string(); } catch (...) {
        }

        boost::asio::co_spawn(ex,
                              [session, channel_ptr = completion_channel, endpoint_str]() -> boost::asio::awaitable<void> {
                                  try {
                                      // 执行优雅关闭
                                      co_await session->graceful_shutdown();
                                  } catch (const std::exception& e) {
                                      SPDLOG_WARN("H2 shutdown exception [{}]: {}", endpoint_str, e.what());
                                  }
                                  // 无论成功失败，发送完成信号
                                  co_await channel_ptr->async_send(boost::system::error_code{}, boost::asio::use_awaitable);
                              },
                              boost::asio::detached
        );
    }

    // 等待所有信号返回
    // 设置 10 秒总超时
    auto wait_logic = [&]() -> boost::asio::awaitable<void> {
        for (size_t i = 0; i < h2_sessions_to_stop.size(); ++i) {
            co_await completion_channel->async_receive(boost::asio::use_awaitable);
        }
    };

    boost::asio::steady_timer stop_timer(co_await boost::asio::this_coro::executor);
    stop_timer.expires_after(10s);

    // 等待 "所有任务完成" 或者 "超时"
    const auto result = co_await (
        wait_logic() ||
        stop_timer.async_wait(boost::asio::use_awaitable)
    );

    if (result.index() == 1) {
        SPDLOG_WARN("Server stop timed out. Some H2 sessions might not have closed gracefully.");
    } else {
        SPDLOG_INFO("All sessions stopped gracefully.");
    }
}


/**
 * @brief 主监听协程 (运行在 Main Thread)
 * 职责单一化：只负责 Accept，然后立即分发给 Worker
 */
boost::asio::awaitable<void> Server::listener() {
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

            // 从 Pool 获取下一个 IO Context
            auto& client_ioc = app_.get_ioc();

            // 创建 Socket 并绑定到该 Context
            // 之后的读写操作都会自动在这个 client_ioc 的线程上执行
            tcp::socket socket(client_ioc);

            // 异步等待并接受一个新的 TCP 连接
            // 直接把 socket 传给 accept
            // 这样 accept 成功后，socket 既拥有连接句柄，又绑定在 IO Worker context 线程池的线程上
            co_await acceptor_.async_accept(
                socket,
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));

            // 接受失败，记录日志并继续等待下一个连接
            if (ec) {
                if (ec == boost::asio::error::operation_aborted) {
                    SPDLOG_INFO("Listener stopped (acceptor closed).");
                    co_return;
                }

                SPDLOG_WARN("Accept failed: {}", ec.message());

                if (ec == boost::asio::error::no_descriptors ||
                    ec == boost::asio::error::no_buffer_space ||
                    ec == boost::asio::error::connection_aborted ||
                    ec == boost::system::errc::too_many_files_open) {
                    timer.expires_after(100ms);
                    co_await timer.async_wait(boost::asio::use_awaitable);
                }
                continue;
            }
            // 此时 socket 是 valid 的，且 get_executor() 返回的是 client_ioc
            // socket.get_executor() 绑定的是 io_contexts_ 池子里的某一个 IO Context
            /*boost::asio::co_spawn(
                socket.get_executor(),
                handle_connection(std::move(socket)),
                boost::asio::detached
            );*/
            auto executor = socket.get_executor();
            boost::asio::co_spawn(
                executor,
                [this, s = std::move(socket)]() mutable -> boost::asio::awaitable<void> {
                    return handle_connection(std::move(s));
                },
                boost::asio::detached
            );

        } catch (const std::exception& e) {
            // 捕获异常后只打印日志，循环继续执行，确保服务器继续监听
            SPDLOG_ERROR("Exception in listener loop iteration: {}", e.what());
        }
    }
}

/**
 * @brief 连接处理协程 (运行在 Worker Thread)
 * 包含原 listener 中繁重的握手和 Session 创建逻辑
 */
boost::asio::awaitable<void> Server::handle_connection(boost::asio::ip::tcp::socket socket) {
    try {
        // 性能优化: 禁用 Nagle 算法开启TCP_NODELAY，这对 HTTP/2/1 性能至关重要
        boost::system::error_code ec;
        socket.set_option(boost::asio::ip::tcp::no_delay(true), ec);
        if (ec) {
            SPDLOG_WARN("Failed to set TCP_NODELAY: {}", ec.message());
        }

        SPDLOG_INFO("Handle connection start. Socket open: {}", socket.is_open());

        if (!use_ssl_) {
            if (is_stopping_) co_return;



            const auto session = std::make_shared<HttpSession>(
                std::move(socket), router_, work_executor_,
                max_request_body_size_bytes_, initial_timeout_ms_, keep_alive_timeout);

            // 注册到全局集合
            // 使用 co_spawn 临时跳到 Main H1 Strand  上执行，执行完后 co_await 返回
            co_await boost::asio::co_spawn(h1_strand_, [&]() -> boost::asio::awaitable<void> {
                if (!is_stopping_) http_sessions_.insert(session);
                co_return;
            }, boost::asio::use_awaitable);

            // 启动/运行 Session(当前线程)
            try {
                co_await session->run();
            } catch (const std::exception& e) {
                SPDLOG_ERROR("HttpSession run error: {}", e.what());
            }

            // 清理集合 (切回 Main Strand)
            co_await boost::asio::co_spawn(h1_strand_, [&]() -> boost::asio::awaitable<void> {
                http_sessions_.erase(session);
                co_return;
            }, boost::asio::use_awaitable);

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
            SPDLOG_ERROR("TLS handshake failed (likely HTTP request on HTTPS port) from {}: {}", tls_stream->next_layer().remote_endpoint().address().to_string(), ec.message());
            // 无论如何都关闭连接
            sock.close();
            co_return; // 握手失败，放弃此连接
        }

        // 握手是个耗时操作，握手回来后，服务器可能已经 stop 了
        // 必须检查！否则会发生上述的 Use-After-Free 崩溃
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

            //  H2 Strand 插入
            // 注册 (Main Strand)
            co_await boost::asio::co_spawn(h2_strand_, [&]() -> boost::asio::awaitable<void> {
                if (!is_stopping_) h2_sessions_.insert(weak_session_ptr);
                co_return;
            }, boost::asio::use_awaitable);

            SPDLOG_INFO("H2: 注册完成，准备 Start...");

            // 启动 session 的处理循环 (Worker Thread)
            try {
                co_await session->start();
            } catch (const std::exception& e) {
                SPDLOG_DEBUG("H2 error: {}", e.what());
            }

            // 清理 (Main h2 Strand)
            co_await boost::asio::co_spawn(h2_strand_, [&]() -> boost::asio::awaitable<void> {
                h2_sessions_.erase(weak_session_ptr);
                co_return;
            }, boost::asio::use_awaitable);
        } else {
            // 回退到处理 HTTPS的逻辑

            // HTTPS 逻辑
            const auto session = std::make_shared<HttpsSession>(std::move(tls_stream), router_, work_executor_, max_request_body_size_bytes_, initial_timeout_ms_, keep_alive_timeout);

            // 注册
            co_await boost::asio::co_spawn(https_strand_, [&]() -> boost::asio::awaitable<void> {
                if (!is_stopping_) https_sessions_.insert(session);
                co_return;
            }, boost::asio::use_awaitable);

            //  启动session
            try {
                co_await session->run();
            } catch (...) {
            }

            // 清理
            co_await boost::asio::co_spawn(https_strand_, [&]() -> boost::asio::awaitable<void> {
                https_sessions_.erase(session);
                co_return;
            }, boost::asio::use_awaitable);
        }
    } catch (...) {
        throw;
    }
}

/**
 * @brief [私有] 配置并启动 TCP acceptor。
 */
void Server::setup_acceptor(const uint16_t port, const std::string& ip) {
    boost::system::error_code ec;

    // 1. 创建 endpoint
    auto const address = boost::asio::ip::make_address(ip, ec);
    if (ec) {
        throw std::runtime_error("Invalid IP address provided: " + ip);
    }
    const tcp::endpoint endpoint(address, port);

    // 2. 配置 acceptor
    if (const auto error_code = acceptor_.open(endpoint.protocol(), ec)) {
        throw std::runtime_error("Acceptor failed to open: " + error_code.message());
    }

    if (const auto error_code = acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec)) {
        // 通常这不是一个致命错误，可以只打印警告
        std::cerr << "Warning: Failed to set reuse_address option: " << error_code.message() << std::endl;
        ec.clear();
    }

    if (const auto error_code = acceptor_.bind(endpoint, ec)) {
        throw std::runtime_error("Failed to bind to endpoint " + ip + ":" + std::to_string(port) + ". Error: " + error_code.message());
    }

    if (const auto error_code = acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec)) {
        throw std::runtime_error("Acceptor failed to listen: " + error_code.message());
    }

    std::cout << "Success: Server is listening on " << endpoint << std::endl;
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
