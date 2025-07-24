//
// Created by ubuntu on 2025/7/21.
//

#include "http_ssl_connection.hpp"

#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>


HttpSslConnection::HttpSslConnection(StreamType stream, std::string pool_key)
    : stream_(std::move(stream)), // 直接移动传入的 stream
      id_(generate_simple_uuid()),
      pool_key_(std::move(pool_key)),
      last_used_time_(std::chrono::steady_clock::now()) {
    SPDLOG_DEBUG("HttpSslConnection [{}] for pool [{}] created.", id_, pool_key_);
}

HttpSslConnection::~HttpSslConnection() {
    /* ... */
}

boost::asio::awaitable<bool> HttpSslConnection::ping() {
    co_return is_usable();
}

boost::asio::awaitable<HttpResponse> HttpSslConnection::execute(HttpRequest request) {
    try {
        update_last_used_time();
        // 发送请求
        co_await http::async_write(stream_, request, boost::asio::use_awaitable);
        SPDLOG_DEBUG("HttpsConnection [{}] request sent.", id_);

        // 接收响应
        HttpResponse response;
        co_await http::async_read(stream_, buffer_, response, boost::asio::use_awaitable);
        SPDLOG_DEBUG("HttpsConnection [{}] response received with status {}.", id_, response.result_int());

        // 检查是否应该保持连接
        if (response.result_int() >= 300) {
            keep_alive_ = false;
        } else {
            keep_alive_ = response.keep_alive();
        }
        SPDLOG_DEBUG("HttpsConnection [{}] response received with status {}.keep_alive_ = {}", id_, response.result_int(), keep_alive_);
        // 返回响应
        co_return response;
    } catch (const boost::system::system_error& e) {
        SPDLOG_ERROR("HttpsConnection [{}] error: {}", id_, e.what());
        close(); // 出错时关闭连接
        // 将异常重新抛出，让调用者知道操作失败了
        throw;
    }
}

bool HttpSslConnection::is_usable() const {
    return stream_.next_layer().socket().is_open() && keep_alive_;
}

boost::asio::awaitable<void> HttpSslConnection::close() {
    keep_alive_ = false;
    boost::system::error_code ec;
    stream_.next_layer().socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    stream_.next_layer().close();
    co_return;
}

void HttpSslConnection::update_last_used_time() {
    last_used_time_ = std::chrono::steady_clock::now();
}

const std::string& HttpSslConnection::id() const { return id_; }
const std::string& HttpSslConnection::get_pool_key() const { return pool_key_; }

std::string HttpSslConnection::generate_simple_uuid() {
    static std::atomic<uint64_t> counter = 0;
    return "conn-ssl-" + std::to_string(++counter);
}
