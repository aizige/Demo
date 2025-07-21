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
    explicit HttpSslConnection(boost::asio::ip::tcp::socket socket, boost::asio::ssl::context& ctx, std::string pool_key);
    ~HttpSslConnection() override;

    boost::asio::awaitable<void> handshake(std::string_view host);
    boost::asio::awaitable<HttpResponse> execute(HttpRequest& request) override;
    bool is_usable() const override;
    void close() override;
    const std::string& id() const override;
    const std::string& get_pool_key() const override;

    // in HttpSslConnection
    boost::asio::ip::tcp::socket& lowest_layer_socket() override {
        // beast::ssl_stream -> beast::tcp_stream -> tcp::socket
        return stream_.next_layer().socket();
    }

private:
    static std::string generate_simple_uuid();
    boost::beast::ssl_stream<boost::beast::tcp_stream> stream_;

    boost::beast::flat_buffer buffer_;
    std::string id_;
    std::string pool_key_;
    bool keep_alive_ = true;
};


#endif //UNTITLED1_HTTP_SSL_CONNECTION_HPP