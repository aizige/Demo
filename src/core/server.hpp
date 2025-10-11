#ifndef SERVER_HPP
#define SERVER_HPP

#include <iostream>
#include <set>
#include <unordered_set>
#include <spdlog/spdlog.h>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>     // Asio 的 SSL/TLS 功能
#include <boost/beast/http.hpp>   // Beast 的 HTTP/1.1 功能
#include <boost/asio/experimental/parallel_group.hpp> // 引入 parallel_group
#include "h2c_session.hpp"
#include "http/router.hpp"
#include "ssl_context_manager.hpp"
#include "h2_session.hpp"
#include "http/handler.hpp"
#include "utils/cert_checker.hpp"  // 证书检查工具
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/system/error_code.hpp> // 通常已经间接包含了
#include <boost/asio/ssl/error.hpp>    // 包含了 SSL 相关的错误码和类别
#include "http/request_context.hpp"

// 命名空间别名，方便使用
namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;

// 将 C++14 的时间字面量（如 30s）引入当前作用域
using namespace std::literals::chrono_literals;
using namespace boost::asio::experimental::awaitable_operators;
/**
 * @class Server
 * @brief 主服务器类，负责网络监听和连接分发。
 * 服务器的“大脑”，负责监听、接受连接、进行协议分发。
 *
 * 这个类是整个应用程序的入口点和事件循环的驱动者。它的职责包括：
 * 1. 监听指定的 TCP 端口，等待客户端连接。
 * 2. 接受新的 TCP 连接。
 * 3. 如果启用了 SSL/TLS，则执行 TLS 握手。
 * 4. 通过 ALPN (Application-Layer Protocol Negotiation) 判断客户端期望使用的协议。
 * 5. 如果是 HTTP/2 (`h2`)，则将连接交给 `Http2Session` 处理。
 * 6. 如果是 HTTP/1.1 或未知协议，则回退到基于 Boost.Beast 的 HTTPS 或 HTTP 处理器。
 * 7. 管理服务器范围内的资源，如路由器 (`Router`) 和 SSL 上下文 (`SslContextManager`)。
 */
class Server {
public:
    /**
     * @brief Server 构造函数。
     * @param ioc 对主 `io_context` 的引用，所有异步操作都在此上运行。
     * @param ip 绑定ip地址
     * @param port 要监听的端口号。
     * @param use_ssl 是否启用 SSL/TLS。
     */
    Server(boost::asio::io_context& ioc, const std::string& ip, const uint16_t port)
        : io_context_(ioc),
          acceptor_(ioc) // 先用 io_context 构造 acceptor
    {
        // 调用一个辅助函数来完成剩下的设置
        setup_acceptor(port, ip);
    }

    Server(boost::asio::io_context& ioc, const uint16_t port)
        : Server(ioc, "0.0.0.0", port) // 调用另一个构造函数
    {
    }

    /**
     * @brief 获取对内部路由器的引用。
     *        允许外部代码（如 `main.cpp`）向服务器注册路由。
     * @return Router&
     */
    Router& router() { return router_; }

    /**
     * @brief 设置并加载 TLS 证书和私钥。
     * @param cert 证书文件路径。
     * @param key 私钥文件路径。
     */
    void set_tls(const std::string& cert, const std::string& key) {
        // 加载证书和私钥到 SSL 上下文管理器
        try {
            ssl_mgr_.load(cert, key);
            use_ssl_ = true;
        } catch (std::exception& e) {
            use_ssl_ = false;
            SPDLOG_ERROR("开启SSL失败: {}", e.what());
        }

        // [可选但推荐] 在启动时检查证书是否包含了所有必需的域名/IP，以进行部署验证
        CertChecker::inspect(cert,
                             {
                                 "dev.myubuntu.com",
                                 "192.168.1.176"
                             });
    }

    /**
     * @brief 启动服务器的监听循环。
     */
    void run() {
        // 启动一个常驻的监听协程
        boost::asio::co_spawn(io_context_, listener(), boost::asio::detached);
    }


    // **注意：返回类型是 void，不再是 awaitable<void>**
    void stop() {
        // 1. 立即停止接受新连接
        if (acceptor_.is_open()) {
            acceptor_.close();
            SPDLOG_INFO("Server stopped accepting new connections.");
        }

        // 2. 在锁的作用域内，为每个会话启动一个独立的关闭协程
        {
            std::lock_guard<std::mutex> lock(session_mutex_);

            SPDLOG_INFO("Spawning shutdown tasks for {} H2 sessions...", h2_sessions_.size());
            // --- 处理 Http2Session ---
            for (const auto& weak_session : h2_sessions_) {
                if (auto session = weak_session.lock()) {
                    // 为每个 session 启动一个独立的、被分离的协程来执行关闭
                    // 它会在后台运行，我们不等待它
                    boost::asio::co_spawn(
                        io_context_, // 在 server 的 io_context 上运行
                        [session]() -> boost::asio::awaitable<void> {
                            try {
                                co_await session->graceful_shutdown();
                            } catch (const std::exception& e) {
                                SPDLOG_WARN("Exception in H2 graceful_shutdown task: {}", e.what());
                            }
                        },
                        boost::asio::detached // 分离协程，我们不关心它的结果
                    );
                }
            }
            h2_sessions_.clear(); // 清空列表
        }

        SPDLOG_INFO("All shutdown tasks have been spawned.");
        // 函数在这里立即返回，不阻塞，不 co_await
    }

private:
    /**
     * @brief 主监听协程，一个无限循环，负责接受新连接。
     */
    boost::asio::awaitable<void> listener() {
        // 获取当前协程的执行器，用于派生新的协程
        auto exec = co_await boost::asio::this_coro::executor;

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
                ssl_mgr_.context()
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


                // **关键**：我们现在拥有底层的 tcp::socket，可以用来发送明文响应
                auto& sock = tls_stream->next_layer();

                HttpResponse resp{http::status::bad_request, 11};
                resp.set(http::field::content_type, "text/html");
                resp.set(http::field::connection, "close");
                //resp.set(http::field::server, "Aiziboyserver/1.0");
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
            std::string_view alpn{reinterpret_cast<const char*>(proto), len};


            // 4. 根据协商的协议，将连接分发给不同的处理器
            if (alpn == "h2") // 如果协商结果是 HTTP/2
            {
                // 创建一个 Http2Session 来处理这个连接
                auto session = Http2Session::create(tls_stream, router_);
                // --- 添加 Session 到列表中 ---
                {
                    std::lock_guard<std::mutex> lock(session_mutex_);
                    h2_sessions_.insert(session);
                }
                // 启动 session 的处理循环
                boost::asio::co_spawn(exec,
                                      [session]() -> boost::asio::awaitable<void> {
                                          // 我们现在 co_await session 的整个生命周期
                                          co_await session->start();
                                      },
                                      boost::asio::detached);
            } else {
                // 回退到处理 HTTPS (HTTP/1.1 TLS) 的逻辑
                boost::asio::co_spawn(exec, handle_https(std::move(tls_stream)), boost::asio::detached);
            }
        }
    }

    /**
     * @brief 处理普通（非加密）的 HTTP/1.1 连接的协程。
     */
    boost::asio::awaitable<void> handle_plain(tcp::socket sock) const {
        try {
            // 使用 Beast 推荐的 flat_buffer 来提高性能
            boost::beast::flat_buffer buf;

            // --- 协议错配检测：检查客户端是否错误地发送了 TLS 握手 ---
            char first_byte;

            // 使用 peek 选项来“窥视”第一个字节，而不从流中消耗它
            // asio::transfer_exactly(1) 是必须的
            // as_tuple 使得超时或错误不会抛出异常
            auto [ec, n] = co_await boost::asio::async_read(
                sock,
                boost::asio::buffer(&first_byte, 1),
                boost::asio::as_tuple(boost::asio::use_awaitable)
            );

            if (ec) {
                // 读取第一个字节就失败了（例如，客户端立即断开），直接返回
                co_return;
            }

            // 检查第一个字节是否是 TLS Handshake (0x16) 或 SSLv2 ClientHello (0x80)
            if (first_byte == 0x16 || first_byte == '\x80') {
                SPDLOG_WARN("Detected TLS/SSL handshake on plain HTTP port from {}. Closing connection.",sock.remote_endpoint().address().to_string());
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


            // 创建一个可复用的定时器
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
            // 为新连接的第一个请求设置一个较短的超时
            constexpr auto initial_timeout = 5s;
            // 为 keep-alive 的空闲连接设置一个较短的超时
            constexpr auto keep_alive_timeout = 10s;


            // --- Keep-Alive 主处理循环 ---
            for (;;) {
                // 创建一个生命周期仅限于单次循环的 parser
                // 这比在循环外创建 parser 更安全，可以避免状态残留
                http::request_parser<http::string_body> parser;

                // 为读取请求设置超时
                // 在循环的第一次迭代中，使用 initial_timeout
                timer.expires_after(buf.size() > 1 ? keep_alive_timeout : initial_timeout);

                // 并发地等待读取完成或超时
                auto read_result = co_await (
                    http::async_read(sock, buf, parser, boost::asio::use_awaitable) ||
                    timer.async_wait(boost::asio::use_awaitable)
                );

                if (read_result.index() == 1) { // index 1 代表定时器先完成
                    SPDLOG_INFO("HTTP/1.1 connection timeout ({}s).",
                               (buf.size() > 1 ? keep_alive_timeout : initial_timeout).count());
                    break; // 超时，退出循环
                }
                timer.cancel(); // 成功读取，取消计时器

                // 从 parser 中取出请求对象
                HttpRequest req = parser.release();

                // 查找匹配的路由和处理器
                auto match = router_.dispatch(req.method(), req.target());

                //  我们在循环的最后需要一个新的 req 对象，所以这里需要先保存 keep-alive 状态。
                bool const keep_alive = req.keep_alive();

                // 2. 创建RequestContext请求上下文
                //    这里很重要：req 会被移动到 ctx 中，原始的 req 变量将不再有效。
                RequestContext ctx(std::move(req), std::move(match.path_params));

                // 执行业务逻辑处理器
                try {
                    co_await match.handler(ctx);
                } catch (const std::exception& e) {
                    SPDLOG_ERROR("Exception in HTTP handler for [{}]: {}",
                                    ctx.request().target(), e.what());
                    // 确保在异常情况下也能返回一个标准的错误响应
                    ctx.string(http::status::internal_server_error, "Internal Server Error");
                }


                //  统一的响应后处理。
                //  从 ctx 中取出最终的响应对象。
                auto& resp = ctx.response();

                // TODO: 最佳压缩时机

                //  根据原始请求的 keep_alive 状态，设置响应的 keep_alive 头部。
                resp.keep_alive(keep_alive);

                resp.prepare_payload(); // 最佳prepare_payload时机而不是在RequestContext::json内


                // 异步发送响应
                co_await http::async_write(sock, resp, boost::asio::use_awaitable);


                if (!keep_alive) { break; } // 客户端不希望或协议不支持保持连接，退出循环

                // 如果 parser 还有未处理的数据（HTTP Pipelining），
                // 我们在这里不做处理，直接关闭连接以简化逻辑。
                // 现代客户端很少使用 Pipelining。
                if(parser.is_done() == false) {
                    SPDLOG_WARN("HTTP pipelining detected, which is not fully supported. Closing connection.");
                    break;
                }
            }
        } catch (const boost::system::system_error& e) {
            // 捕获 Asio/Beast 的 I/O 错误，例如 EOF (客户端正常关闭)
            if (e.code() != http::error::end_of_stream) {
                SPDLOG_DEBUG("Error in plain HTTP session: {}", e.what());
            }
        } catch (const std::exception& e) {
            // 捕获其他非 I/O 异常
            SPDLOG_ERROR("Unexpected exception in plain HTTP session: {}", e.what());
        }

        // --- 最终关闭逻辑 ---
        boost::system::error_code ec;
        // 对于明文 TCP，我们直接 shutdown
        sock.shutdown(tcp::socket::shutdown_send, ec);
        SPDLOG_INFO("HTTP tcp socket closed with code: {}", ec.message());
    }

    /**
     *  @brief 处理加密的 HTTPS (HTTP/1.1) 连接的协程。
     *        逻辑与 handle_plain 基本相同，只是 I/O 操作对象变为了 tls_stream。
     */

    boost::asio::awaitable<void> handle_https(std::shared_ptr<boost::asio::ssl::stream<tcp::socket>> tls_stream) {
        boost::beast::flat_buffer buf;
        try {
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
            // --- 定义不同的超时策略 ---
            // 为新连接的第一个请求设置一个较短的超时
            // 为 keep-alive 的空闲连接设置一个较长的超时
            constexpr auto initial_timeout = 10s;
            constexpr auto keep_alive_timeout = 60s;

            http::request_parser<http::string_body> parser;

            // 1. 读取第一个请求，使用 initial_timeout
            timer.expires_after(initial_timeout);
            auto first_read_result = co_await (
                http::async_read(*tls_stream, buf, parser, boost::asio::use_awaitable)
                ||
                timer.async_wait(boost::asio::use_awaitable)
            );
            if (first_read_result.index() == 1) {
                SPDLOG_INFO("HTTPS initial request timeout.");
                co_return; // 直接返回，外层会关闭连接
            }

            HttpRequest req = parser.release();

            // 2. 进入主处理循环
            for (;;) {
                auto match = router_.dispatch(req.method(), req.target());

                // 2. 创建 RequestContext。
                //    这里很重要：req 会被移动到 ctx 中，原始的 req 变量将不再有效。
                //    我们在循环的最后需要一个新的 req 对象，所以这里需要先保存 keep-alive 状态。
                bool const keep_alive = req.keep_alive();
                RequestContext ctx(std::move(req), std::move(match.path_params));
                try {
                    co_await match.handler(ctx);
                } catch (const std::exception& e) {
                    SPDLOG_ERROR("Exception in HTTPS handler for [{}]: {}",
                                 ctx.request().target(),
                                 e.what());
                    ctx.string(http::status::internal_server_error, "error Internal Server Error");
                }


                auto& resp = ctx.response();

                // TODO: 最佳压缩时机

                // 根据请求决定响应是否 keep-alive
                resp.keep_alive(keep_alive);

                resp.prepare_payload(); // 最佳prepare_payload时机而不是在RequestContext::json内

                // 发送响应
                co_await http::async_write(*tls_stream, resp, boost::asio::use_awaitable);

                // 3. 根据 keep_alive 决定下一步行为
                if (!keep_alive) {
                    // 客户端不希望保持连接，我们也不需要等待，直接退出循环
                    SPDLOG_INFO("HTTPS 客户端不希望保持连接，我们也不需要等待，直接退出循环");
                    break;
                }

                // 4. 客户端希望保持连接，我们为等待下一个请求设置一个较长的空闲超时
                timer.expires_after(keep_alive_timeout);
                http::request_parser<http::string_body> next_parser;

                auto next_read_result = co_await (
                    http::async_read(*tls_stream, buf, next_parser, boost::asio::use_awaitable)
                    ||
                    timer.async_wait(boost::asio::use_awaitable)
                );

                if (next_read_result.index() == 1) {
                    // 空闲超时
                    SPDLOG_DEBUG("HTTPS keep-alive connection idle timeout.");
                    break;
                }

                // 成功读取到新请求，准备下一次循环
                req = next_parser.release();
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("处理Https请求发生错误：{}", e.what());
        }

        // --- 最终关闭逻辑 ---
        boost::system::error_code ec;
        // 尝试优雅关闭
        co_await tls_stream->async_shutdown(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        // 如果优雅关闭失败或被跳过，确保底层套接字被关闭
        if (tls_stream->next_layer().is_open()) {
            tls_stream->next_layer().close(ec);
            SPDLOG_DEBUG("HTTP tcp socket clos {}", ec.message());
        }
    }

    // 辅助函数，封装了 acceptor 的设置逻辑，避免代码重复
    void setup_acceptor(uint16_t port, const std::string& ip) {
        boost::system::error_code ec;

        // 1. 创建 endpoint
        auto const address = boost::asio::ip::make_address(ip, ec);
        if (ec) {
            throw std::runtime_error("Invalid IP address provided: " + ip);
        }
        tcp::endpoint endpoint(address, port);

        // 2. 配置 acceptor
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            throw std::runtime_error("Acceptor failed to open: " + ec.message());
        }

        acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (ec) {
            // 通常这不是一个致命错误，可以只打印警告
            std::cerr << "Warning: Failed to set reuse_address option: " << ec.message() << std::endl;
        }

        acceptor_.bind(endpoint, ec);
        if (ec) {
            throw std::runtime_error("Failed to bind to endpoint " + ip + ":" + std::to_string(port) + ". Error: " + ec.message());
        }

        acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec) {
            throw std::runtime_error("Acceptor failed to listen: " + ec.message());
        }

        std::cout << "Success: Server is listening on " << endpoint << std::endl;
    }


    boost::asio::io_context& io_context_; // 对主 io_context 的引用
    tcp::acceptor acceptor_; // TCP 监听器
    bool use_ssl_ = false; // 是否启用 SSL 的标志
    SslContextManager ssl_mgr_; // SSL 上下文管理器
    Router router_; // 路由器实例

    // --- 新增的成员变量 ---
    std::mutex session_mutex_;
    // **使用 std::set 和 std::owner_less，这是最简单、最正确的方案**
    std::set<std::weak_ptr<Http2Session>, std::owner_less<std::weak_ptr<Http2Session>>> h2_sessions_;
};

#endif // SERVER_HPP
