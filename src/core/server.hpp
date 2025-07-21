#ifndef SERVER_HPP
#define SERVER_HPP

#include <iostream>
#include <set>
#include <unordered_set>
#include <spdlog/spdlog.h>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>     // Asio 的 SSL/TLS 功能
#include <boost/beast/http.hpp>   // Beast 的 HTTP/1.1 功能

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
     * @param io 对主 `io_context` 的引用，所有异步操作都在此上运行。
     * @param port 要监听的端口号。
     * @param use_ssl 是否启用 SSL/TLS。
     */
    Server(boost::asio::io_context& io, uint16_t port)
        : io_(io),
          // 创建一个 acceptor，绑定到指定端口的 IPv4 地址上
          acceptor_(io, tcp::endpoint(tcp::v4(), port)) {
        use_ssl_ = false;
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
        // 启动一个常驻的监听协程，并将其与主 io_context 分离。
        // 服务器将在此协程中无限期地接受新连接。
        boost::asio::co_spawn(io_, listener(), boost::asio::detached);
    }

    /**
     * @brief 启动优雅关闭服务器的流程。
     */
    // 在 Server 类的实现中
    void stop() {
        // 1. 停止接受新连接
        acceptor_.close();
        SPDLOG_INFO("Server stopped accepting new connections.");

        // 2. 优雅地关闭所有活跃的 H2 Session
        std::lock_guard<std::mutex> lock(session_mutex_);
        SPDLOG_INFO("Initiating graceful shutdown for {} H2 sessions...", h2_sessions_.size());
        for (auto const& weak_session : h2_sessions_) {
            // 尝试从 weak_ptr 获取 shared_ptr
            if (auto session = weak_session.lock()) {
                session->graceful_shutdown();
            }
        }
        h2_sessions_.clear(); // 清空列表

        // 3. 优雅地关闭所有活跃的 H2C Session
        SPDLOG_INFO("Initiating graceful shutdown for {} H2C sessions...", h2c_sessions_.size());
        for (auto const& weak_session : h2c_sessions_) {
            if (auto session = weak_session.lock()) {
                session->graceful_shutdown();
            }
        }
        h2c_sessions_.clear();
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
                SPDLOG_ERROR("Accept failed: {}", ec.message());
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
                    resp.set(http::field::server, "Aiziboyserver/1.0");
                    resp.body() = "<html><body><h1>400 Bad Request</h1>"
                        "<p>This port requires HTTPS, but a plain HTTP request was received.</p>"
                        "</body></html>";
                    resp.prepare_payload();

                    // 在原始 socket 上发送明文响应
                    // 我们需要忽略写操作的错误，因为对方可能已经不等响应就关闭了连接
                    auto [write_ec, bytes] = co_await http::async_write(sock,
                                                                        resp,
                                                                        boost::asio::as_tuple(boost::asio::use_awaitable));
                    // 无论如何都关闭连接
                    sock.close();

                SPDLOG_ERROR("TLS handshake failed (likely HTTP request on HTTPS port) from {}: {}",tls_stream->next_layer().remote_endpoint().address().to_string(),ec.message());
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

    boost::asio::awaitable<void> handle_plain(tcp::socket sock) {
        // 将 socket 包装在 shared_ptr 中，以便在 H2C 升级时安全地转移所有权
        auto sock_ptr = std::make_shared<tcp::socket>(std::move(sock));
        boost::beast::flat_buffer buf;

        try {
            // --- [新增] 协议检测逻辑 ---
            char first_byte;
            auto [ec, n] = co_await boost::asio::async_read(
                *sock_ptr,
                boost::asio::buffer(&first_byte, 1),
                boost::asio::as_tuple(boost::asio::use_awaitable)
            );

            if (ec) {
                // 读取第一个字节就失败了，直接返回
                co_return;
            }

            // 检查第一个字节是否是 TLS Handshake (0x16)
            // 0x80 是 SSLv2 ClientHello 的第一个字节，也可以加上
            if (first_byte == 0x16 || first_byte == '\x80') {
                // 关闭连接并立即返回
                sock_ptr->close();
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
            constexpr auto initial_timeout = 10s; // 定义超时策略
            constexpr auto keep_alive_timeout = 60s;
            http::request_parser<http::string_body> parser; // 创建一个可复用的解析器

            //  1. 设置初始超时并一次性读取整个请求

            timer.expires_after(initial_timeout);
            auto read_result = co_await (http::async_read(*sock_ptr, buf, parser, boost::asio::use_awaitable) || timer.async_wait(boost::asio::use_awaitable));

            // 如果是超时（计时器先完成），则直接退出
            if (read_result.index() == 1) {
                SPDLOG_INFO("HTTP initial request header timeout.");
                co_return;
            }
            timer.cancel(); // 成功读取，取消计时器

            // 2. 获取请求对象并检查 H2C Upgrade
            HttpRequest req = parser.release(); // 先把请求拿出来
            if (req.count(http::field::upgrade) && boost::beast::iequals(req.at(http::field::upgrade), "h2c")) {
                SPDLOG_INFO("HTTP/1.1 to H2C upgrade requested.");

                HttpResponse resp{http::status::switching_protocols, req.version()};
                resp.set(http::field::connection, "Upgrade");
                resp.set(http::field::upgrade, "h2c");
                resp.prepare_payload();

                co_await http::async_write(*sock_ptr, resp, boost::asio::use_awaitable);

                // ... 启动 H2cSession ...
                auto session = H2cSession::create(sock_ptr, router_);
                {
                    std::lock_guard<std::mutex> lock(session_mutex_);
                    h2c_sessions_.insert(session);
                }
                session->start();
                co_return; // 协议已切换，此协程任务完成
            }

            // --- 3. 如果不是 H2C 升级，进入keep-alive 主处理循环 ---
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
                    spdlog::error("Exception in HTTP handler for [{}]: {}",
                                  ctx.request().target(),
                                  e.what());
                    ctx.string(http::status::internal_server_error, "error, Internal Server Error");
                }


                // 5. 统一的响应后处理。
                //    从 ctx 中取出最终的响应对象。
                auto& resp = ctx.response();
                // 6. 根据原始请求的 keep_alive 状态，设置响应的 keep_alive 头部。
                resp.keep_alive(keep_alive);
                co_await http::async_write(*sock_ptr, resp, boost::asio::use_awaitable);

                if (!keep_alive) { break; } // 客户端不希望或协议不支持保持连接，退出循环

                // --- 5. 为等待下一个请求设置 keep-alive 空闲超时 ---
                timer.expires_after(keep_alive_timeout);
                http::request_parser<http::string_body> next_parser;

                auto next_read_result = co_await (http::async_read(*sock_ptr, buf, next_parser, boost::asio::use_awaitable) || timer.async_wait(boost::asio::use_awaitable));

                if (next_read_result.index() == 1) {
                    // 空闲超时
                    SPDLOG_INFO("HTTP/1.1 keep-alive connection idle timeout.");
                    break;
                }

                // 成功读取到新请求，准备下一次循环
                req = next_parser.release();
            }
        } catch (const std::exception& e) {
            // 捕获所有 I/O 错误（如 EOF）或其它异常，准备关闭连接
            SPDLOG_ERROR("Error in plain HTTP session: {}", e.what());
        }

        // --- 最终关闭逻辑 ---
        boost::system::error_code ec;
        // 对于明文 TCP，我们直接 shutdown
        sock_ptr->shutdown(tcp::socket::shutdown_send, ec);
        //sock_ptr->shutdown(tcp::socket::shutdown_both, ec);
        SPDLOG_INFO("HTTP tcp socket closed with code: {}", ec.message());
        co_return;
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
                    spdlog::error("Exception in HTTPS handler for [{}]: {}",
                                  ctx.request().target(),
                                  e.what());
                    ctx.string(http::status::internal_server_error, "error Internal Server Error");
                }


                auto& resp = ctx.response();
                // 根据请求决定响应是否 keep-alive
                resp.keep_alive(keep_alive);

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
                    SPDLOG_INFO("HTTPS keep-alive connection idle timeout.");
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
            SPDLOG_INFO("HTTP tcp socket clos {}", ec.message());
        }
    }


    boost::asio::io_context& io_; // 对主 io_context 的引用
    tcp::acceptor acceptor_; // TCP 监听器
    bool use_ssl_; // 是否启用 SSL 的标志
    SslContextManager ssl_mgr_; // SSL 上下文管理器
    Router router_; // 路由器实例

    // --- 新增的成员变量 ---
    std::mutex session_mutex_;
    // **使用 std::set 和 std::owner_less，这是最简单、最正确的方案**
    std::set<std::weak_ptr<Http2Session>, std::owner_less<std::weak_ptr<Http2Session>>> h2_sessions_;
    std::set<std::weak_ptr<H2cSession>, std::owner_less<std::weak_ptr<H2cSession>>> h2c_sessions_;
};

#endif // SERVER_HPP
