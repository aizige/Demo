//
// Created by ubuntu on 2025/7/21.
//

#include "http_ssl_connection.hpp"

#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>

HttpSslConnection::HttpSslConnection(boost::asio::ip::tcp::socket socket, boost::asio::ssl::context& ctx, std::string pool_key)
    : stream_(std::move(socket), ctx),
      id_(generate_simple_uuid()),
      pool_key_(std::move(pool_key)) {}

HttpSslConnection::~HttpSslConnection() { /* ... */ }

boost::asio::awaitable<void> HttpSslConnection::handshake(std::string_view host) {
    // 设置 SNI，这对于现代 HTTPS 至关重要
    if (!SSL_set_tlsext_host_name(stream_.native_handle(), host.data())) {

        // 1. 先创建一个 error_code 对象
        boost::system::error_code ec{static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category()};
        // 2. 然后用这个 error_code 对象来构造 system_error
        throw boost::system::system_error{ec};
    }
    co_await stream_.async_handshake(boost::asio::ssl::stream_base::client, boost::asio::use_awaitable);
}

boost::asio::awaitable<HttpResponse> HttpSslConnection::execute(HttpRequest& request) {
    try {
        co_await http::async_write(stream_, request, boost::asio::use_awaitable);
        HttpResponse response;
        co_await http::async_read(stream_, buffer_, response, boost::asio::use_awaitable);
        keep_alive_ = response.keep_alive();



        co_return response;
    } catch (const std::exception& e) {
        this->close();
        throw;
    }
}

bool HttpSslConnection::is_usable() const {
    return stream_.next_layer().socket().is_open() && keep_alive_;
}

void HttpSslConnection::close() {
    keep_alive_ = false;
    boost::system::error_code ec;
    stream_.next_layer().socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    stream_.next_layer().close();
}

const std::string& HttpSslConnection::id() const { return id_; }
const std::string& HttpSslConnection::get_pool_key() const { return pool_key_; }

std::string HttpSslConnection::generate_simple_uuid() {
    static std::atomic<uint64_t> counter = 0;
    return "conn-ssl-" + std::to_string(++counter);
}