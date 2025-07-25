//
// Created by ubuntu on 2025/7/21.
//

#ifndef UNTITLED1_HTTP_SSL_CONNECTION_HPP
#define UNTITLED1_HTTP_SSL_CONNECTION_HPP


#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>

#include "iconnection.hpp"


class HttpSslConnection : public IConnection, public std::enable_shared_from_this<HttpSslConnection> {
public:
    using StreamType = boost::beast::ssl_stream<boost::beast::tcp_stream>;

    //explicit HttpSslConnection(boost::asio::ip::tcp::socket socket, boost::asio::ssl::context& ctx, std::string pool_key);
    explicit HttpSslConnection(StreamType stream, std::string pool_key);

    ~HttpSslConnection() override;


    boost::asio::awaitable<HttpResponse> execute(HttpRequest request) override;

    bool is_usable() const override;

    boost::asio::awaitable<void> close() override;

    const std::string &id() const override;

    const std::string &get_pool_key() const override;

    size_t get_active_streams() const override { return active_streams_.load(); }

    boost::asio::awaitable<bool> ping() override;

    int64_t get_last_used_timestamp_seconds() const override { return last_used_timestamp_seconds_; }

    size_t get_max_concurrent_streams() const override { return 1; }

    void update_last_used_time() override;

    bool supports_multiplexing() const override { return false; }

private:
    static std::string generate_simple_uuid();

    StreamType stream_;

    boost::beast::flat_buffer buffer_;
    std::string id_;
    std::string pool_key_;
    bool keep_alive_ = true;
    std::atomic<size_t> active_streams_{0}; // 0 表示空闲, 1 表示繁忙
    int64_t last_used_timestamp_seconds_;
};


#endif //UNTITLED1_HTTP_SSL_CONNECTION_HPP
