//
// Created by Aiziboy on 2025/11/16.
//

#include "Server.hpp"
#include <fstream>
#include <iostream>
#include <set>
#include <unordered_set>
#include <spdlog/spdlog.h>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>     // Asio 的 SSL/TLS 功能
#include <boost/beast/http.hpp>   // Beast 的 HTTP/1.1 功能
#include <boost/asio/experimental/parallel_group.hpp> // 引入 parallel_group
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ssl/error.hpp>    // 包含了 SSL 相关的错误码和类别
#include "http/request_context.hpp"
#include "utils/finally.hpp"
#include "http/http_common_types.hpp"
#include "http/network_constants.hpp"
#include "version.hpp"
#include "utils/config/AizixConfig.hpp"
#include "http/router.hpp"
#include "h2_session.hpp"
#include "utils/cert_checker.hpp"  // 证书检查工具

// 命名空间别名
namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;
using namespace std::literals::chrono_literals;
using namespace boost::asio::experimental::awaitable_operators;
/**
 * @brief 构造函数 (带 IP 和端口)。
 * @param ioc 对主 io_context 的引用。
 * @param work_executor 工作线程的executor
 * @param config 配置文件
 */
Server::Server(boost::asio::io_context& ioc, boost::asio::any_io_executor work_executor, const AizixConfig& config)
    : io_context_(ioc),
      work_executor_(std::move(work_executor)),
      ssl_context_(boost::asio::ssl::context::tlsv12_server), // 先用 io_context 构造 acceptor
      acceptor_(ioc),
      use_ssl_(config.server.ssl->enabled),
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

        // [!!! 重新抛出异常，中断启动 !!!]
        // throw std::runtime_error("TLS configuration failed. Server cannot start in HTTPS mode.");
    }

    // 在启动时检查证书是否包含了所有必需的域名/IP，以进行部署验证
    CertChecker::inspect(cert_file, {"dev.myubuntu.com", "192.168.1.176"});
}

/**
 * @brief 启动服务器的监听循环。
 * 这是一个非阻塞操作，它会启动一个后台协程来处理连接接受。
 */
void Server::run() {
    // 启动一个常驻的监听协程
    boost::asio::co_spawn(io_context_, listener(), boost::asio::detached);
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
    // 1. 立即停止接受新连接，防止在关闭过程中有新会话建立。
    if (acceptor_.is_open()) {
        acceptor_.close();
        SPDLOG_INFO("Server stopped accepting new connections.");
    }

    // --- 2. 安全地收集所有需要关闭的会话 ---
    // 创建一个局部 vector 来存储指向活跃会话的 shared_ptr。
    std::vector<std::shared_ptr<Http2Session>> sessions_to_stop;
    {
        // 使用 lock_guard 来保护对共享成员 h2_sessions_ 的访问。
        // 这确保了在收集过程中，listener 协程不会同时修改列表。
        std::lock_guard<std::mutex> lock2(session_mutex_);
        SPDLOG_INFO("Collecting {} active H2 sessions for shutdown...", h2_sessions_.size());

        // 遍历存储 weak_ptr 的集合。

        for (const auto& weak_session : h2_sessions_) {
            // 尝试将 weak_ptr提升为 shared_ptr。
            // 如果 session 对象仍然存活，.lock() 会成功。
            if (auto session = weak_session.lock()) {
                // 提升为 shared_ptr
                sessions_to_stop.push_back(session);
            }
        }
        // 清空列表，以便 Server 对象可以被安全销毁，即使某些关闭任务仍在运行。
        h2_sessions_.clear();
    }

    // 如果没有任何活跃的会话，则无需等待，直接返回。
    if (sessions_to_stop.empty()) {
        SPDLOG_INFO("No active H2 sessions to shut down.");
        co_return;
    }

    // --- 3. 使用 channel 机制来并行关闭并等待所有会话 ---
    // 获取当前协程的执行器，用于创建 channel。
    auto ex = co_await boost::asio::this_coro::executor;

    // 创建一个 channel 作为“计数信号量”。它的容量等于任务数量。
    // 每个关闭任务完成后，都会向 channel 发送一个信号。
    auto completion_channel = std::make_shared<boost::asio::experimental::channel<void(boost::system::error_code)>>(
        ex, sessions_to_stop.size()
    );

    SPDLOG_INFO("Spawning and waiting for {} H2 session shutdown tasks...", sessions_to_stop.size());

    // 4. 为每个需要关闭的会话，并发地启动一个独立的关闭协程。
    for (const auto& session : sessions_to_stop) {
        // [!!! 获取 IP 地址用于日志 !!!]
        // 在启动协程前，在当前作用域安全地获取端点信息
        // 因为 session 对象在协程中可能被析构
        // to_string() 可以将 endpoint 格式化为 "ip:port"
        std::string endpoint_str = session->remote_endpoint().address().to_string() + ":" + std::to_string(session->remote_endpoint().port());


        boost::asio::co_spawn(
            ex, // 在与 stop() 相同的执行器上启动，以简化调度
            [session, channel_ptr = completion_channel,endpoint_str]() -> boost::asio::awaitable<void> {
                try {
                    // 每个协程只负责关闭一个会话。
                    co_await session->graceful_shutdown();
                } catch (const std::exception& e) {
                    // 忽略关闭单个会话时可能发生的异常，以确保整个 stop() 流程不会被中断。
                    SPDLOG_WARN("Exception during H2 session [{}] graceful_shutdown: {}", endpoint_str, e.what());
                }

                // 任务完成，向 channel 发送一个完成信号。
                // 即使关闭失败，也要发送信号，以防止 stop() 协程无限期等待。
                co_await channel_ptr->async_send(boost::system::error_code{}, boost::asio::use_awaitable);
            },
            boost::asio::detached // 分离协程，我们不直接 await 它，而是通过 channel 等待它的完成信号。
        );
    }

    // 5. 在主 stop() 协程中，挂起并等待，直到接收到所有任务发回的完成信号。
    for (size_t i = 0; i < sessions_to_stop.size(); ++i) {
        // 每次 async_receive 都会挂起，直到一个关闭任务完成并向 channel 发送信号。
        // 这个循环会精确地等待 sessions_to_stop.size() 次。
        co_await completion_channel->async_receive(boost::asio::use_awaitable);
    }

    SPDLOG_INFO("All H2 server sessions have been shut down.");
}


/**
 * @brief 主监听协程，一个无限循环，负责接受新连接。
 */
boost::asio::awaitable<void> Server::listener() {
    // 获取当前协程的执行器，用于派生新的协程
    const auto exec = co_await boost::asio::this_coro::executor;


    for (;;) // 无限循环以持续接受连接
    {
        boost::system::error_code ec;

        // 异步等待并接受一个新的 TCP 连接
        tcp::socket raw_sock =
            co_await acceptor_.async_accept(
                boost::asio::redirect_error(boost::asio::use_awaitable, ec)
            );

        if (ec) {
            //SPDLOG_ERROR("Accept failed: {}", ec.message());
            continue; // 接受失败，记录日志并继续等待下一个连接
        }

        // --- 根据是否启用 SSL 进行分支处理 ---
        if (!use_ssl_) {
            // 处理普通的 HTTP/1.1 连接
            boost::asio::co_spawn(exec, handle_plain(std::move(raw_sock)), boost::asio::detached);
            continue;
        }

        // --- SSL/TLS 连接处理流程 ---
        // 1. 创建一个 SSL 流，将原始 TCP 套接字包装起来
        auto tls_stream = std::make_shared<boost::asio::ssl::stream<tcp::socket>>(
            std::move(raw_sock),
            ssl_context_
        );

        // 2. 异步执行 TLS 握手
        co_await tls_stream->async_handshake(
            boost::asio::ssl::stream_base::server,
            // 以服务器模式进行握手
            boost::asio::redirect_error(boost::asio::use_awaitable, ec)
        );

        if (ec) {
            // 这里可以根据具体的 SSL 错误码判断是否是协议错配
            // 为了简单起见，我们假设所有握手失败都是因为这个原因

            // 我们现在拥有底层的 tcp::socket，可以用来发送明文响应
            auto& sock = tls_stream->next_layer();

            HttpResponse resp{http::status::bad_request, 11};
            resp.set(http::field::content_type, "text/html");
            resp.set(http::field::connection, "close");
            resp.set(http::field::server, aizix::framework::name + "/" += aizix::framework::version);
            resp.body() = "<html><body><h1>400 Bad Request</h1>"
                "<p>This port requires HTTPS, but a plain HTTP request was received.</p>"
                "</body></html>";
            resp.prepare_payload();

            // 在原始 socket 上发送明文响应
            // 我们需要忽略写操作的错误，因为对方可能已经不等响应就关闭了连接
            auto [write_ec, bytes] = co_await http::async_write(sock,
                                                                resp,
                                                                boost::asio::as_tuple(boost::asio::use_awaitable));
            SPDLOG_ERROR("TLS handshake failed (likely HTTP request on HTTPS port) from {}: {}", tls_stream->next_layer().remote_endpoint().address().to_string(), ec.message());
            // 无论如何都关闭连接
            sock.close();
            continue; // 握手失败，放弃此连接
        }

        // 3. ALPN 协议协商：检查客户端和服务器共同选择的应用层协议
        const unsigned char* proto = nullptr;
        unsigned int len = 0;
        // 从 SSL 对象中获取协商结果
        SSL_get0_alpn_selected(tls_stream->native_handle(), &proto, &len);
        const std::string_view alpn{reinterpret_cast<const char*>(proto), len};

        SPDLOG_INFO("ALPN 协商结果: {}", alpn);
        // 4. 根据协商的协议，将连接分发给不同的处理器
        if (alpn == "h2") {
            // 如果协商结果是 HTTP/2
            // 创建一个 Http2Session 来处理这个连接
            auto session = Http2Session::create(tls_stream, work_executor_, router_, max_request_body_size_bytes_, keep_alive_timeout);

            // 将 session 的 weak_ptr 转换为 shared_ptr 添加 Session 到列表中存起来
            // 这样我们就有了一个可以安全地从集合中移除自身的 token
            auto weak_session_ptr = std::weak_ptr<Http2Session>(session);
            {
                std::lock_guard<std::mutex> lock(session_mutex_);
                h2_sessions_.insert(weak_session_ptr);
            }

            // 启动 session 的处理循环
            boost::asio::co_spawn(exec, [session, weak_session_ptr, this]() -> boost::asio::awaitable<void> {
                                      try {
                                          // 我们现在 co_await session 的整个生命周期
                                          co_await session->start();
                                      } catch (const std::exception& e) {
                                          SPDLOG_DEBUG("Http2Session ended with exception: {}", e.what());
                                      }

                                      // --- 自动清理 ---
                                      // 当 session->start() 结束后（无论正常或异常），
                                      // 这个协程会继续执行这里的代码。
                                      {
                                          std::lock_guard<std::mutex> lock(session_mutex_);
                                          // 使用 weak_ptr 从集合中安全地移除自己
                                          h2_sessions_.erase(weak_session_ptr);
                                          SPDLOG_DEBUG("Http2Session removed from server's tracking list.");
                                      }
                                  },
                                  boost::asio::detached);
        } else {
            // 回退到处理 HTTPS (HTTP/1.1 TLS) 的逻辑
            boost::asio::co_spawn(exec, handle_https(std::move(tls_stream)), boost::asio::detached);
        }
    }
}

/**
 * @brief [私有] 处理纯文本 HTTP/1.1 连接的协程。
 *        包含 Keep-Alive 逻辑和超时处理。
 */
boost::asio::awaitable<void> Server::handle_plain(tcp::socket sock) const {
    try {
        // 使用 Beast 推荐的 flat_buffer 来提高性能
        boost::beast::flat_buffer buf;

        // --- 协议错配检测：检查客户端是否错误地发送了 TLS 握手 ---
        char first_byte;

        // 使用 peek 选项来“窥视”第一个字节，而不从流中消耗它
        // asio::transfer_exactly(1) 是必须的
        // as_tuple 使得超时或错误不会抛出异常

        if (auto [ec, n] = co_await boost::asio::async_read(sock, boost::asio::buffer(&first_byte, 1), boost::asio::as_tuple(boost::asio::use_awaitable)); ec) {
            // 读取第一个字节就失败了（例如，客户端立即断开），直接返回
            co_return;
        }

        // 检查第一个字节是否是 TLS Handshake (0x16) 或 SSLv2 ClientHello (0x80)
        if (first_byte == 0x16 || first_byte == '\x80') {
            SPDLOG_WARN("Detected TLS/SSL handshake on plain HTTP port from {}. Closing connection.", sock.remote_endpoint().address().to_string());
            // 检测到协议错误，直接关闭连接
            sock.close();
            co_return;
        }

        // 如果不是 TLS 请求，我们需要把刚刚读到的第一个字节“放回”到缓冲区
        // 以便后续的 http::async_read 能读到完整的请求
        auto mutable_buf = buf.prepare(1);
        // 2. 将我们读取到的字节手动拷贝进去
        memcpy(mutable_buf.data(), &first_byte, 1);
        // 3. 提交我们写入的 1 个字节
        buf.commit(1);
        // --- 协议检测结束 ---


        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
        // --- 定义不同的超时策略 ---
        // 为新连接的第一个请求设置一个较短的超时
        // 为 keep-alive 的空闲连接设置一个较长的超时

        // 标志位，用于判断是否是第一个请求
        bool is_first_request = true;

        for (;;) {
            http::request_parser<http::string_body> parser;
            parser.body_limit(max_request_body_size_bytes_);

            // 1. 读取第一个请求，使用 initial_timeout
            timer.expires_after(is_first_request ? std::chrono::milliseconds(initial_timeout_ms_) : keep_alive_timeout); // 根据是否是第一个请求来设置超时
            auto first_read_result = co_await (
                http::async_read(sock, buf, parser, boost::asio::as_tuple(boost::asio::use_awaitable)) ||
                timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable))
            );

            if (first_read_result.index() == 1) {
                SPDLOG_INFO("HTTP initial request timeout.");
                break;
            }

            timer.cancel_one();

            if (auto [ec, n] = std::get<0>(first_read_result); ec) {
                if (ec == http::error::body_limit) {
                    // --- 子情况 2a: 请求体过大 ---
                    SPDLOG_WARN("HTTP/1.1 request body too large from remote endpoint");

                    HttpResponse resp{http::status::payload_too_large, 11};
                    resp.set(http::field::connection, "close");
                    resp.set(http::field::server, aizix::framework::name + "/" += aizix::framework::version);
                    resp.prepare_payload();

                    // 发送 413 响应，然后必须关闭连接
                    co_await http::async_write(sock, resp, boost::asio::use_awaitable);
                    break;
                } else {
                    // --- 子情况: 其他 I/O 错误 ---
                    // 抛出异常，让外层的 try-catch 块来处理
                    throw boost::system::system_error{ec};
                }
            }


            HttpRequest req = parser.release();

            // --- Keep-Alive 主处理循环 ---

            // 第一个请求已经读完，直接处理
            // 查找匹配的路由和处理器
            auto [handler, path_params] = router_.dispatch(req.method(), req.target());

            //  我们在循环的最后需要一个新的 req 对象，所以这里需要先保存 keep-alive 状态。
            bool const keep_alive = req.keep_alive();

            // 创建RequestContext请求上下文
            //    这里很重要：req 会被移动到 ctx 中，原始的 req 变量将不再有效。
            RequestContext ctx(std::move(req), std::move(path_params));

            // 执行业务逻辑处理器
            try {
                co_await handler(ctx);
            } catch (const std::exception& e) {
                SPDLOG_ERROR("Exception in HTTP handler for [{}]: {}",
                             ctx.request().target(), e.what());
                // 确保在异常情况下也能返回一个标准的错误响应
                ctx.string(http::status::internal_server_error, "Internal Server Error");
            }

            //  统一的响应后处理。
            //  从 ctx 中取出最终的响应对象。
            auto& resp = ctx.response();

            //  根据原始请求的 keep_alive 状态，设置响应的 keep_alive 头部。
            resp.keep_alive(keep_alive);
            co_await ctx.compressIfAcceptable(work_executor_); // 压缩响应体
            resp.prepare_payload();

            // 发送响应
            co_await http::async_write(sock, resp, boost::asio::use_awaitable);

            //  根据 keep_alive 决定下一步行为
            if (!keep_alive) {
                // 客户端不希望保持连接，我们也不需要等待，直接退出循环
                SPDLOG_DEBUG("HTTP 客户端不希望保持连接，我们也不需要等待，直接退出循环");
                break;
            }

            // 成功处理完一个请求后，将标志位置为 false
            is_first_request = false;
        }
    } catch (const boost::system::system_error& e) {
        // 捕获 Asio/Beast 的 I/O 错误，例如 EOF (客户端正常关闭)
        if (const auto& code = e.code(); code == http::error::end_of_stream || code == boost::asio::error::connection_reset) {
            // --- 这些都被视为客户端导致的正常或预期的连接中断 ---
            // end_of_stream: 客户端在 keep-alive 循环中正常关闭连接。
            // connection_reset: 客户端突然关闭或崩溃，连接被强制重置。
            SPDLOG_DEBUG("HTTPS session ended by client:{} ", code.message());
        } else {
            // 其他所有 I/O 错误，可能是服务器或网络环境的真正问题。
            SPDLOG_ERROR("Error in HTTPS session ({}): {}", code.message(), e.what());
        }
    } catch (const std::exception& e) {
        // 捕获其他非 I/O 异常
        SPDLOG_ERROR("Unexpected exception in plain HTTP session: {}", e.what());
    }

    // --- 最终关闭逻辑 ---
    boost::system::error_code ec;
    auto error_code = sock.shutdown(tcp::socket::shutdown_send, ec);
    if (error_code) { SPDLOG_DEBUG("HTTP tcp socket closed with code: {}", error_code.message()); }
    // 对于明文 TCP，我们直接 shutdown
    SPDLOG_DEBUG("HTTP tcp socket closed with code: {}", ec.message());
}

/**
 *  @brief 处理加密的 HTTPS (HTTP/1.1) 连接的协程。
 *        逻辑与 handle_plain 基本相同，只是 I/O 操作对象变为了 tls_stream。
 */
boost::asio::awaitable<void> Server::handle_https(std::shared_ptr<boost::asio::ssl::stream<tcp::socket>> tls_stream) const {
    try {
        boost::beast::flat_buffer buf;
        // 创建一个可复用的定时器
        boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
        // --- 定义不同的超时策略 ---
        // 标志位，用于判断是否是第一个请求
        bool is_first_request = true;
        // 2. 进入主处理循环
        for (;;) {
            http::request_parser<http::string_body> parser;
            parser.body_limit(max_request_body_size_bytes_);

            // 1. 读取请求为新连接的第一个请求设置一个较短的超时为 keep-alive 的空闲连接设置一个较长的超时
            timer.expires_after(is_first_request ? std::chrono::milliseconds(initial_timeout_ms_) : keep_alive_timeout); // 根据是否是第一个请求来设置超时
            auto first_read_result = co_await (
                http::async_read(*tls_stream, buf, parser, boost::asio::as_tuple(boost::asio::use_awaitable))
                ||
                timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable))
            );


            if (first_read_result.index() == 1) {
                SPDLOG_INFO("HTTPS initial request timeout.");
                break;
            }
            timer.cancel_one(); // 取消定时器

            if (auto [ec, n] = std::get<0>(first_read_result); ec) {
                if (ec == http::error::body_limit) {
                    // --- 子情况 2a: 请求体过大 ---
                    SPDLOG_WARN("HTTP/1.1 request body too large from remote endpoint");

                    HttpResponse resp{http::status::payload_too_large, 11};
                    resp.set(http::field::connection, "close");
                    resp.set(http::field::server, aizix::framework::name + "/" += aizix::framework::version);
                    resp.prepare_payload();

                    // 发送 413 响应，然后必须关闭连接
                    co_await http::async_write(*tls_stream, resp, boost::asio::use_awaitable);
                    break;
                } else {
                    // --- 子情况: 其他 I/O 错误 ---
                    // 抛出异常，让外层的 try-catch 块来处理
                    throw boost::system::system_error{ec};
                }
            }


            HttpRequest req = parser.release();


            auto [handler, path_params] = router_.dispatch(req.method(), req.target());

            //    创建 RequestContext。
            //    这里很重要：req 会被移动到 ctx 中，原始的 req 变量将不再有效。
            //    我们在循环的最后需要一个新的 req 对象，所以这里需要先保存 keep-alive 状态。
            bool const keep_alive = req.keep_alive();
            RequestContext ctx(std::move(req), std::move(path_params));
            try {
                co_await handler(ctx);
            } catch (const std::exception& e) {
                SPDLOG_ERROR("Exception in HTTPS handler for [{}]: {}",
                             ctx.request().target(),
                             e.what());
                ctx.string(http::status::internal_server_error, "error Internal Server Error");
                break;
            }


            auto& resp = ctx.response();
            // 根据请求决定响应是否 keep-alive
            resp.keep_alive(keep_alive);
            co_await ctx.compressIfAcceptable(work_executor_); // 压缩响应体
            resp.prepare_payload();


            // 发送响应
            co_await http::async_write(*tls_stream, resp, boost::asio::use_awaitable);

            // 3. 根据 keep_alive 决定下一步行为
            if (!keep_alive) {
                // 客户端不希望保持连接，我们也不需要等待，直接退出循环
                SPDLOG_DEBUG("HTTPS 客户端不希望保持连接，我们也不需要等待，直接退出循环");
                break;
            }
            // 成功处理完一个请求后，将标志位置为 false
            is_first_request = false;
        }
    } catch (const boost::system::system_error& e) {
        if (const auto& code = e.code(); code == http::error::end_of_stream || code == boost::asio::error::connection_reset || code == boost::asio::ssl::error::stream_truncated) {
            // --- 这些都被视为客户端导致的正常或预期的连接中断 ---
            // end_of_stream: 客户端在 keep-alive 循环中正常关闭连接。
            // connection_reset: 客户端突然关闭或崩溃，连接被强制重置。
            // stream_truncated: SSL/TLS 层面的正常关闭信号。
            SPDLOG_DEBUG("HTTPS session ended by client: {}", code.message());
        } else {
            // 其他所有 I/O 错误，可能是服务器或网络环境的真正问题。
            SPDLOG_ERROR("Error in HTTPS session ({}): {}", code.message(), e.what());
        }
    } catch (const std::exception& e) {
        // 捕获其他非 boost::system 的异常
        SPDLOG_ERROR("Unexpected exception in HTTPS session: {}", e.what());
    }

    // --- 最终关闭逻辑 ---
    boost::system::error_code ec;
    // 尝试优雅关闭
    co_await tls_stream->async_shutdown(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
    // 如果优雅关闭失败或被跳过，确保底层套接字被关闭
    if (tls_stream->next_layer().is_open()) {
        auto error_code = tls_stream->next_layer().close(ec);
        SPDLOG_DEBUG("HTTPS tcp socket clos {}", error_code.message());
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
