//
// Created by Aiziboy on 2025/7/18.
//

#include "http1_connection.hpp"

#include <boost/asio/use_awaitable.hpp>

#include "http/http_common_types.hpp"


Http1Connection::Http1Connection(tcp::socket socket, std::string pool_key)
    : socket_(std::move(socket)),
      id_(generate_simple_uuid()),
      pool_key_(std::move(pool_key))
// 初始化新成员
{
    SPDLOG_DEBUG("Http1Connection [{}] for pool [{}] created.", id_, pool_key_);
}



// 实现 execute 协程
boost::asio::awaitable<HttpResponse> Http1Connection::execute(HttpRequest& request) {
    try {
        // 发送请求
        co_await http::async_write(socket_, request, boost::asio::use_awaitable);
        SPDLOG_DEBUG("Http1Connection [{}] request sent.", id_);

        // 接收响应
        HttpResponse response;
        co_await http::async_read(socket_, buffer_, response, boost::asio::use_awaitable);
        SPDLOG_DEBUG("Http1Connection [{}] response received with status {}.", id_, response.result_int());

        // 检查是否应该保持连接
       if ( response.result_int() >= 300) {
           keep_alive_ = false;
       } else {
           keep_alive_ = response.keep_alive();
       }

        // 返回响应
        co_return response;
    } catch (const boost::system::system_error& e) {
        SPDLOG_ERROR("Http1Connection [{}] error: {}", id_, e.what());
        close(); // 出错时关闭连接
        // 将异常重新抛出，让调用者知道操作失败了
        throw;
    }
}

bool Http1Connection::is_usable() const {
    // 连接必须是打开的，并且上一次通信允许 keep-alive
    return socket_.socket().is_open() && keep_alive_;
}

void Http1Connection::close() {
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
}
