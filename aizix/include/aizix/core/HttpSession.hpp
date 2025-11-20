//
// Created by Aiziboy on 2025/11/20.
//

#ifndef AIZIX_HTTP_SESSION_HPP
#define AIZIX_HTTP_SESSION_HPP
#include <aizix/http/router.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <spdlog/spdlog.h>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>     // Asio 的 SSL/TLS 功能
#include <boost/beast/http.hpp>   // Beast 的 HTTP/1.1 功能
#include <boost/asio/experimental/parallel_group.hpp> // 引入 parallel_group
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <aizix/version.hpp>
#include <aizix/http/request_context.hpp>
#include <boost/beast/core/detect_ssl.hpp>

using namespace boost::asio::experimental::awaitable_operators;

class HttpSession  : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(
        tcp::socket socket,
        Router& router,
        const boost::asio::any_io_executor& work_exec,
        const size_t body_limit,
        const std::chrono::milliseconds timeout,
        const std::chrono::milliseconds keep_alive
    ) : socket_(std::move(socket)), router_(router), work_exec_(work_exec),
        body_limit_(body_limit), initial_timeout_(timeout), keep_alive_timeout_(keep_alive) {
    }

    // 启动会话
    boost::asio::awaitable<void> run() {
        auto self = this->shared_from_this(); // 保活
        try {

            // 使用 Beast 推荐的 flat_buffer 来提高性能
            boost::beast::flat_buffer buf;

            boost::system::error_code ec;
            bool is_ssl = co_await boost::beast::async_detect_ssl(
                socket_, buf, boost::asio::redirect_error(boost::asio::use_awaitable, ec)
            );

            if (ec) {
                // 读取错误或客户端断开
                co_return;
            }

            if (is_ssl) {
                SPDLOG_WARN("Detected TLS on plain HTTP port. Closing.");
                co_return; // 析构关闭 socket
            }


            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
            // 标志位，用于判断是否是第一个请求
            bool is_first_request = true;

            for (;;) {
                if (stopped_) break; // 检查停止标志

                http::request_parser<http::string_body> parser;
                parser.body_limit(body_limit_);

                timer.expires_after(is_first_request ? initial_timeout_ : keep_alive_timeout_); // 为新连接的第一个请求设置一个较短的超时 为 keep-alive 的空闲连接设置一个较长的超时

                // 使用通用流操作
                // 注意：对于 SSL stream，async_read 会自动处理解密
                auto result = co_await (
                    http::async_read(socket_, buf, parser, boost::asio::as_tuple(boost::asio::use_awaitable)) ||
                    timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable))
                );


                if (result.index() == 1) {
                    // 超时
                    if (!stopped_) SPDLOG_DEBUG("HTTP/1.1 request timeout.");
                    break;
                }

                timer.cancel_one();


                // 示例：解析完成
                if (auto [ec, n] = std::get<0>(result); ec) {
                    if (ec == http::error::body_limit) {
                        // --- 子情况 2a: 请求体过大 ---
                        SPDLOG_WARN("HTTP/1.1 request body too large from remote endpoint");

                        HttpResponse resp{http::status::payload_too_large, 11};
                        resp.set(http::field::connection, "close");
                        resp.set(http::field::server, aizix::framework::name + "/" += aizix::framework::version);
                        resp.prepare_payload();

                        // 发送 413 响应，然后必须关闭连接
                        co_await http::async_write(socket_, resp, boost::asio::use_awaitable);
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
                co_await ctx.compressIfAcceptable(work_exec_); // 压缩响应体
                resp.prepare_payload();

                // 发送响应
                co_await http::async_write(socket_, resp, boost::asio::use_awaitable);

                //  根据 keep_alive 决定下一步行为
                if (!keep_alive) {
                    // 客户端不希望保持连接，我们也不需要等待，直接退出循环
                    SPDLOG_DEBUG("HTTP 客户端不希望保持连接，我们也不需要等待，直接退出循环");
                    break;
                }

                // 成功处理完一个请求后，将标志位置为 false
                is_first_request = false;
            }
        }catch (const boost::system::system_error& e) {
            // 捕获 Asio/Beast 的 I/O 错误，例如 EOF (客户端正常关闭)
            if (const auto& code = e.code(); code == http::error::end_of_stream || code == boost::asio::error::connection_reset || code == boost::asio::error::operation_aborted) {
                // --- 这些都被视为客户端导致的正常或预期的连接中断 ---
                // end_of_stream: 客户端在 keep-alive 循环中正常关闭连接。
                // connection_reset: 客户端突然关闭或崩溃，连接被强制重置。
                // operation_aborted: 即操作被取消(优雅关机的时候会触发)
                SPDLOG_DEBUG("HTTPS session ended by client:{} ", code.message());
            } else {
                // 其他所有 I/O 错误，可能是服务器或网络环境的真正问题。
                SPDLOG_ERROR("Error in HTTPS session ({}): {}", code.message(), e.what());
            }
        } catch (const std::exception& e) {
            // 捕获其他非 I/O 异常
            SPDLOG_ERROR("Unexpected exception in plain HTTP session: {}", e.what());
        }

        // 关闭连接
        close_socket();
    }

    // 优雅关闭接口
    void stop() {
        stopped_ = true;
        close_socket(); // 强制关闭 Socket 会让 async_read 返回 operation_aborted，从而退出协程
    }

private:
    void close_socket() {
        // 使用 get_lowest_layer 获取最底层的 tcp::socket 进行关闭
        if (auto& socket = boost::beast::get_lowest_layer(socket_); socket.is_open()) {
            boost::system::error_code ec;
            socket.shutdown(tcp::socket::shutdown_send, ec);
            socket.close(ec);
            if (ec) { SPDLOG_DEBUG("HTTP tcp socket closed with code: {}", ec.message()); }
        }

    }

    tcp::socket socket_;
    Router& router_;
    boost::asio::any_io_executor work_exec_;
    size_t body_limit_;
    std::chrono::milliseconds initial_timeout_;
    std::chrono::milliseconds keep_alive_timeout_;
    bool stopped_ = false;
};


#endif //AIZIX_HTTP_SESSION_HPP
