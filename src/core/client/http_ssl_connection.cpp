//
// Created by ubuntu on 2025/7/21.
//

#include "http_ssl_connection.hpp"

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>
#include <ada.h>
#include "utils/utils.hpp"

HttpSslConnection::HttpSslConnection(StreamPtr  stream, std::string pool_key)
    : stream_(std::move(stream)), // 直接移动传入的 stream
      id_(generate_simple_uuid()),
      pool_key_(std::move(pool_key)),
      last_used_timestamp_seconds_(steady_clock_seconds_since_epoch()) {
    SPDLOG_DEBUG("HttpSslConnection [{}] for pool [{}] created.", id_, pool_key_);
}

HttpSslConnection::~HttpSslConnection() {
    /* ... */
}

boost::asio::awaitable<bool> HttpSslConnection::ping() {
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

        co_await http::async_write(*stream_, req, boost::asio::use_awaitable);

        // 3. 读取响应头
        //    我们使用一个独立的 buffer，不干扰 execute() 使用的 buffer_
        boost::beast::flat_buffer ping_buffer;
        HttpResponse response;
        co_await http::async_read(*stream_, ping_buffer, response, boost::asio::use_awaitable);
        // 4. 检查响应
        //    只要能收到一个合法的 HTTP 响应（即使是 4xx 或 5xx），
        //    都证明了整个网络链路是通畅的。
        if (response.result_int() > 0) {
            co_return true;
        }
        // 如果收到无效响应，则认为连接有问题
        co_return false;
    } catch (const std::exception &e) {
        keep_alive_ = false;
        co_return false;
    }
}

boost::asio::awaitable<HttpResponse> HttpSslConnection::execute(HttpRequest request) {
    try {
        ++active_streams_;
        update_last_used_time();
        // 发送请求
        co_await http::async_write(*stream_, request, boost::asio::use_awaitable);
        SPDLOG_DEBUG("HttpsConnection [{}] request sent.", id_);

        // 接收响应
        HttpResponse response;
        co_await http::async_read(*stream_, buffer_, response, boost::asio::use_awaitable);
        SPDLOG_DEBUG("HttpsConnection [{}] response received with status {}.", id_, response.result_int());

        // 检查是否应该保持连接
        if (response.result_int() >= 300) {
            keep_alive_ = false;
        } else {
            keep_alive_ = response.keep_alive();
        }
        SPDLOG_DEBUG("HttpsConnection [{}] response received with status {}.keep_alive_ = {}", id_, response.result_int(), keep_alive_);
        // 返回响应
        --active_streams_;
        co_return response;
    } catch (const boost::system::system_error &e) {
        SPDLOG_ERROR("HttpsConnection [{}] error: {}", id_, e.what());
        keep_alive_ = false;
        --active_streams_;
        // 将异常重新抛出，让调用者知道操作失败了
        throw;
    }
}

bool HttpSslConnection::is_usable() const {
    return stream_ && stream_->next_layer().is_open() && keep_alive_;
}

boost::asio::awaitable<void> HttpSslConnection::close() {
    keep_alive_ = false;
    // **修复**: 同样，先检查指针，然后使用 ->
    if (stream_ && stream_->next_layer().is_open()) {
        boost::system::error_code ec;

        // 尝试优雅关闭 SSL，这对于 SSL 连接很重要
        co_await stream_->async_shutdown(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        // 即使 shutdown 失败，也继续关闭底层 socket

        stream_->next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        if (ec) {
            // Log the error but continue
            spdlog::warn("HttpSslConnection [{}]: Socket shutdown failed: {}", id_, ec.message());
        }

        stream_->next_layer().close(ec);
        if (ec) {
            spdlog::warn("HttpSslConnection [{}]: Socket close failed: {}", id_, ec.message());
        }

    }
    co_return;
}

void HttpSslConnection::update_last_used_time() {
    last_used_timestamp_seconds_ = steady_clock_seconds_since_epoch();
}

const std::string &HttpSslConnection::id() const { return id_; }
const std::string &HttpSslConnection::get_pool_key() const { return pool_key_; }

std::string HttpSslConnection::generate_simple_uuid() {
    static std::atomic<uint64_t> counter = 0;
    return "conn-ssl-" + std::to_string(++counter);
}
