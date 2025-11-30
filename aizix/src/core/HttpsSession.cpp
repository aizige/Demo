//
// Created by Aiziboy on 2025/11/20.
//
#include <aizix/core/HttpsSession.hpp>

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>   // Beast 的 HTTP/1.1 功能
#include <boost/asio/experimental/parallel_group.hpp> // 引入 parallel_group
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/ssl/error.hpp>    // 包含了 SSL 相关的错误码和类别
#include <aizix/version.hpp>
#include <utility>

using namespace boost::asio::experimental::awaitable_operators;
HttpsSession::HttpsSession(std::shared_ptr<boost::asio::ssl::stream<tcp::socket>> stream,
                           Router& router,
                           boost::asio::any_io_executor work_exec,
                           const size_t body_limit,
                           const std::chrono::milliseconds timeout,
                           const std::chrono::milliseconds keep_alive)
    : stream_(std::move(stream)),
      router_(router),
      work_exec_(std::move(work_exec)),
      body_limit_(body_limit),
      initial_timeout_(timeout),
      keep_alive_timeout_(keep_alive) {
    SPDLOG_DEBUG("Create a connection [{}:{}] ", remote_endpoint().address().to_string(), remote_endpoint().port());
}

HttpsSession::~HttpsSession() {
    SPDLOG_DEBUG("Close a connection [{}:{}] ", remote_endpoint().address().to_string(), remote_endpoint().port());
}

boost::asio::awaitable<void> HttpsSession::run()  {
        auto self = this->shared_from_this(); // 保活
        try {
            // 使用 Beast 推荐的 flat_buffer 来提高性能
            boost::beast::flat_buffer buf;

            // 创建一个可复用的定时器
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);

            // 标志位，用于判断是否是第一个请求
            bool is_first_request = true;

            for (;;) {
                if (stopped_) break; // 检查停止标志

                http::request_parser<http::string_body> parser;
                parser.body_limit(body_limit_);

                timer.expires_after(is_first_request ? initial_timeout_ : keep_alive_timeout_); // 根据是否是第一个请求来设置超时

                // 使用通用流操作
                // 注意：对于 SSL stream，async_read 会自动处理解密
                auto result = co_await (
                    http::async_read(*stream_, buf, parser, boost::asio::as_tuple(boost::asio::use_awaitable))
                    ||
                    timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable))
                );


                if (result.index() == 1) {
                    // 超时
                    if (!stopped_)
                        SPDLOG_DEBUG("HTTPS/1.1 request timeout.");
                    break;
                }

                timer.cancel_one(); // 取消定时器


                if (auto [ec, n] = std::get<0>(result); ec) {
                    if (ec == http::error::body_limit) {
                        // --- 子情况 2a: 请求体过大 ---
                        SPDLOG_WARN("HTTPS/1.1 request body too large from remote endpoint");

                        HttpResponse resp{http::status::payload_too_large, 11};
                        resp.set(http::field::connection, "close");
                        resp.set(http::field::server, aizix::framework::name + "/" += aizix::framework::version);
                        resp.prepare_payload();

                        // 发送 413 响应，然后必须关闭连接
                        co_await http::async_write(*stream_, resp, boost::asio::use_awaitable);
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
                RequestContext ctx(std::move(req), std::move(path_params), remote_endpoint().address().to_string());

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
                co_await http::async_write(*stream_, resp, boost::asio::use_awaitable);

                //  根据 keep_alive 决定下一步行为
                if (!keep_alive) {
                    // 客户端不希望保持连接，我们也不需要等待，直接退出循环
                    SPDLOG_DEBUG("HTTPS 客户端不希望保持连接，我们也不需要等待，直接退出循环");
                    break;
                }

                // 成功处理完一个请求后，将标志位置为 false
                is_first_request = false;
            }
        } catch (const boost::system::system_error& e) {
            if (const auto& code = e.code(); code == http::error::end_of_stream || code == boost::asio::error::connection_reset || code == boost::asio::ssl::error::stream_truncated || code == boost::asio::error::operation_aborted) {
                // --- 这些都被视为客户端导致的正常或预期的连接中断 ---
                // end_of_stream: 客户端在 keep-alive 循环中正常关闭连接。
                // connection_reset: 客户端突然关闭或崩溃，连接被强制重置。
                // stream_truncated: SSL/TLS 层面的正常关闭信号。
                // operation_aborted: 即操作被取消(优雅关机的时候会触发)
                SPDLOG_DEBUG("HTTPS session ended by client: {}", code.message());
            } else {
                // 其他所有 I/O 错误，可能是服务器或网络环境的真正问题。
                SPDLOG_ERROR("Error in HTTPS session ({}): {}", code.message(), e.what());
            }
        } catch (const std::exception& e) {
            // 捕获其他非 boost::system 的异常
            SPDLOG_ERROR("Unexpected exception in HTTPS session: {}", e.what());
        }

        // 正常结束用优雅关闭
        co_await graceful_close();
    }

void HttpsSession::stop()  {
    stopped_ = true;
    // 直接关闭底层 TCP，这会立即中断正在进行的 async_read/write
    force_close();
}

// ReSharper disable once CppMemberFunctionMayBeConst
void HttpsSession::force_close() {
    if (stream_ && stream_->next_layer().is_open()) {
        boost::system::error_code ec;
        stream_->next_layer().close(ec);
        // 直接关 Socket
        SPDLOG_DEBUG("HTTPS socket forced closed.");
    }
}

boost::asio::awaitable<void> HttpsSession::graceful_close() {
    boost::system::error_code ec;
    // 尝试 TLS shutdown，带超时控制最好，这里简化
    co_await stream_->async_shutdown(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
    force_close(); // 无论 shutdown 成功与否，最后都要关 socket
    // TLS 的 shutdown 是双向的。如果客户端（浏览器）已经关闭了 TCP 连接（这在非 Keep-Alive 或者超时时很常见），服务器调用 async_shutdown 往往会失败（Broken pipe）。这是正常的，忽略错误码，这是对的。但是如果日志级别较低，可能会看到很多 SSL 错误。
}

/// @brief 获取远程客户端的端点信息
boost::asio::ip::tcp::endpoint HttpsSession::remote_endpoint() const {
    try {
        // stream_ 是 ssl::stream，其 next_layer() 是 tcp::socket。
        return stream_->next_layer().remote_endpoint();
    } catch (const boost::system::system_error&) {
        // 如果 socket 已关闭，调用 remote_endpoint() 会抛出异常。
        // 在这种情况下，返回一个默认构造的、无效的 endpoint。
        return {};
    }
}
