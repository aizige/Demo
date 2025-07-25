//
// Created by ubuntu on 2025/7/21.
//

#ifndef UNTITLED1_H2C_CONNECTION_HPP
#define UNTITLED1_H2C_CONNECTION_HPP
#include "iconnection.hpp"
#include <nghttp2/nghttp2.h>
#include <boost/asio/experimental/promise.hpp>
#include <boost/beast/ssl.hpp>
#include <atomic>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <spdlog/spdlog.h>
#include <iostream>
#include <boost/asio/experimental/channel.hpp>

class Http2cConnection : public IConnection, public std::enable_shared_from_this<Http2cConnection> {
public:
    using StreamType = boost::beast::tcp_stream;
    using StreamPtr = std::shared_ptr<StreamType>;
    using ResponseChannel = boost::asio::experimental::channel<void(boost::system::error_code, HttpResponse)>;

    Http2cConnection(StreamPtr stream, std::string pool_key);
    ~Http2cConnection() override;

    Http2cConnection(const Http2cConnection&) = delete;
    Http2cConnection& operator=(const Http2cConnection&) = delete;

    static std::shared_ptr<Http2cConnection> create(StreamPtr stream, std::string key) {
        return std::make_shared<Http2cConnection>(std::move(stream), std::move(key));
    }

    // --- IConnection 接口实现 ---
    boost::asio::awaitable<HttpResponse> execute(HttpRequest request) override;
    bool is_usable() const override;
    boost::asio::awaitable<void> close() override;
    const std::string& id() const override { return id_; }
    const std::string& get_pool_key() const override { return pool_key_; }
    size_t get_active_streams() const override { return active_streams_.load(); }

    boost::asio::awaitable<std::optional<boost::asio::ip::tcp::socket>> release_socket() override {co_return std::nullopt;}  // 明确地 override，并返回 nullopt

    // --- H2 客户端特定方法 ---
    void start(); // 启动器，非协程

    boost::asio::awaitable<bool> ping() override;
    int64_t get_last_used_timestamp_seconds() const override{ return last_used_timestamp_seconds_; }

    size_t get_max_concurrent_streams() const override{return max_concurrent_streams_.load();};

    void update_last_used_time() override;
    bool supports_multiplexing() const override { return true; }

private:
    int64_t last_used_timestamp_seconds_;

    // 上下文，用于存储一个客户端发起的流（请求-响应对）的状态
    struct StreamContext {
        // 构造函数现在接收 executor 来初始化 channel
        explicit StreamContext(const boost::asio::any_io_executor& ex)
            : response_channel(ex, 1) // 容量为 1 的 channel
        {}

        ResponseChannel response_channel;
        HttpResponse response_in_progress; // 正在组装的响应
        std::string request_body; // 请求体的拷贝，用于数据提供者回调
        size_t request_body_offset = 0; // 请求体发送偏移量


    };

    void init_nghttp2_session();
    boost::asio::awaitable<void> session_loop(); // 负责网络 I/O 的主协程
    boost::asio::awaitable<void> do_write();
    void prepare_headers(std::vector<nghttp2_nv>& nva, const HttpRequest& req);

    // --- nghttp2 C-style 静态回调函数 ---
    static int on_begin_headers_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);
    static int on_header_callback(nghttp2_session* session, const nghttp2_frame* frame, const uint8_t* name, size_t namelen, const uint8_t* value, size_t valuelen, uint8_t flags, void* user_data);
    static int on_data_chunk_recv_callback(nghttp2_session* session, uint8_t flags, int32_t stream_id, const uint8_t* data, size_t len, void* user_data);
    static int on_stream_close_callback(nghttp2_session* session, int32_t stream_id, uint32_t error_code, void* user_data);
    static int on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);
    static ssize_t read_request_body_callback(nghttp2_session* session, int32_t stream_id, uint8_t* buf, size_t length, uint32_t* data_flags, nghttp2_data_source* source, void* user_data);


    // --- 成员变量 ---
    StreamPtr stream_;
    std::string pool_key_;
    std::string id_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    nghttp2_session* session_ = nullptr;
    std::unordered_map<int32_t, std::unique_ptr<StreamContext>> streams_;
    std::atomic<bool> is_closing_ = false;

    static std::string generate_simple_uuid();
    std::atomic<size_t> active_streams_{0};
    std::atomic<size_t> max_concurrent_streams_{100}; // 默认值
};


#endif //UNTITLED1_H2C_CONNECTION_HPP