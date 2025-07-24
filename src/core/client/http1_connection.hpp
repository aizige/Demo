//
// Created by Aiziboy on 2025/7/18.
//

#ifndef UNTITLED1_HTTP1_CONNECTION_HPP
#define UNTITLED1_HTTP1_CONNECTION_HPP




#include "iconnection.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <spdlog/spdlog.h>
#include <uuid.h> // 假设你有一个生成UUID的库，如果没有，可以用简单计数器替代
#include "http/http_common_types.hpp"
#include "core/server.hpp"
#include "utils/decompressor.hpp"


class Http1Connection : public IConnection, public std::enable_shared_from_this<Http1Connection> {
public:

    // 构造函数，接收一个已经连接的 socket
    explicit Http1Connection(tcp::socket socket, std::string pool_key);


    ~Http1Connection() override {
        SPDLOG_DEBUG("Http1Connection [{}] destroyed.", id_);
    }

    // 禁止拷贝和移动，因为我们用 shared_ptr 管理
    Http1Connection(const Http1Connection&) = delete;
    Http1Connection& operator=(const Http1Connection&) = delete;


    boost::asio::awaitable<HttpResponse> execute(HttpRequest request) override;
    bool is_usable() const override;
    boost::asio::awaitable<void> close() override;
    const std::string& id() const override { return id_; }
    const std::string& get_pool_key() const override { return pool_key_; }

    size_t get_active_streams() const override;

    boost::asio::awaitable<std::optional<boost::asio::ip::tcp::socket>> release_socket() override;

    boost::asio::awaitable<bool> ping() override;
    int64_t get_last_used_timestamp_ms() const override{ return last_used_timestamp_ms_; }
private:

    // 辅助函数，生成一个简单的伪UUID
    static std::string generate_simple_uuid() {
        // 在实际项目中，使用一个真正的UUID库
        static std::atomic<uint64_t> counter = 0;
        return "conn-" + std::to_string(++counter);
    }


    boost::beast::tcp_stream socket_;
    boost::beast::flat_buffer buffer_; // 可重用的缓冲区
    std::string id_;
    bool keep_alive_ = true;
    std::string pool_key_;
    std::atomic<size_t> active_streams_{0}; // 0 表示空闲, 1 表示繁忙
    int64_t last_used_timestamp_ms_;
};
#endif //UNTITLED1_HTTP1_CONNECTION_HPP