//
// Created by Aiziboy on 2025/7/18.
//

#include "http1_connection.hpp"
#include <ada.h>
#include <boost/asio/use_awaitable.hpp>

#include "http/http_common_types.hpp"
#include "utils/utils.hpp"


Http1Connection::Http1Connection(tcp::socket socket, std::string pool_key)
    : socket_(std::move(socket)),
      id_(generate_simple_uuid()),
      pool_key_(std::move(pool_key)),
      last_used_timestamp_seconds_(steady_clock_seconds_since_epoch())
// 初始化新成员
{
    SPDLOG_DEBUG("Http1Connection [{}] for pool [{}] created.", id_, pool_key_);
}



boost::asio::awaitable<void> Http1Connection::close() {
    if (socket_.socket().is_open()) {
        boost::beast::error_code ec;
        // Best effort shutdown
        socket_.socket().shutdown(tcp::socket::shutdown_both, ec);
        socket_.close();
        if (ec) {
            SPDLOG_WARN("Http1Connection [{}] error on close: {}", id_, ec.message());
        }
    }
    keep_alive_ = false; // 标记为不可用
    co_return;
}
boost::asio::awaitable<bool> Http1Connection::ping() {

    if (!is_usable()) {
        co_return false;
    }
    if (active_streams_.load() > 0) {
        // 已经有业务在用了，那肯定是活的，直接返回成功，不要去添乱
        co_return true;
    }

    try {
        auto url = ada::parse<ada::url_aggregator>(pool_key_);
        if (!url) {
            co_return false;
        }
        HttpRequest req{http::verb::options, "/", 11};
        req.set(http::field::host, url->get_host());
        req.set(http::field::user_agent, "Chrome/120.0.0.0");
        req.set(http::field::accept, "*/*");
        req.set(http::field::connection, "keep-alive");

        co_await http::async_write(socket_, req, boost::asio::use_awaitable);

        // 3. 读取响应头
        //    我们使用一个独立的 buffer，不干扰 execute() 使用的 buffer_
        boost::beast::flat_buffer ping_buffer;
        HttpResponse response;
        co_await http::async_read(socket_, ping_buffer, response, boost::asio::use_awaitable);
        // 4. 检查响应
        //    只要能收到一个合法的 HTTP 响应（即使是 4xx 或 5xx），
        //    都证明了整个网络链路是通畅的。
        if (response.result_int() > 0) {

            co_return true;
        }
        // 如果收到无效响应，则认为连接有问题
        co_return false;
    } catch (const std::exception& e) {
        keep_alive_ = false;
        co_return false;
    }
}
// 实现 execute 协程
boost::asio::awaitable<HttpResponse> Http1Connection::execute(HttpRequest request) {
    try {
        // **在进入时，增加并发流计数**
        ++active_streams_;
        update_last_used_time();
        // 发送请求
        co_await http::async_write(socket_, request, boost::asio::use_awaitable);
        SPDLOG_DEBUG("Http1Connection [{}] request sent.", id_);

        // 接收响应
        HttpResponse response;
        co_await http::async_read(socket_, buffer_, response, boost::asio::use_awaitable);
        SPDLOG_DEBUG("Http1Connection [{}] response received with status {}.", id_, response.result_int());

        // 检查是否应该保持连接
        if (response.result_int() >= 300) {
            keep_alive_ = false;
        } else {
            keep_alive_ = response.keep_alive();
        }

        // 返回响应
        --active_streams_;
        co_return response;
    } catch (const boost::system::system_error& e) {
        SPDLOG_ERROR("Http1Connection [{}] error: {}", id_, e.what());
        --active_streams_;
        keep_alive_ = false;
        throw;
    }
}

bool Http1Connection::is_usable() const {
    // 连接必须是打开的，并且上一次通信允许 keep-alive
    return socket_.socket().is_open() && keep_alive_;
    // 对于 H1.1 连接，只要 socket 打开并且我们没有主动标记它为关闭，就认为是可用的
}

void Http1Connection::update_last_used_time(){
    last_used_timestamp_seconds_ = steady_clock_ms_since_epoch();
}

size_t Http1Connection::get_active_streams() const {
    return active_streams_.load();
}

boost::asio::awaitable<std::optional<boost::asio::ip::tcp::socket>> Http1Connection::release_socket() {
    // 确保我们在正确的 strand 或 executor 上
    //co_await boost::asio::post(socket_.get_executor(), boost::asio::use_awaitable);

    // 检查 socket 是否仍然可用
    if (socket_.socket().is_open()) {
        // 将 socket 移动出来，留下一个空的、关闭的 socket 在 stream 中
        tcp::socket released_socket = std::move(socket_.socket());
        co_return std::make_optional(std::move(released_socket));
    }

    // 如果 socket 已经关闭，返回空
    co_return std::nullopt;
}
