//
// Created by ubuntu on 2025/7/21.
//

#ifndef UNTITLED1_H2_CONNECTION_HPP
#define UNTITLED1_H2_CONNECTION_HPP
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

using ResponseChannel = boost::asio::experimental::channel<void(boost::system::error_code, HttpResponse)>;

class Http2Connection : public IConnection, public std::enable_shared_from_this<Http2Connection> {
public:
    using StreamType = boost::beast::ssl_stream<boost::beast::tcp_stream>;
    using StreamPtr = std::shared_ptr<StreamType>;

    Http2Connection(StreamPtr stream, std::string pool_key);
    ~Http2Connection() override;

    Http2Connection(const Http2Connection&) = delete;
    Http2Connection& operator=(const Http2Connection&) = delete;
    Http2Connection(Http2Connection&&) = delete;
    Http2Connection& operator=(Http2Connection&&) = delete;

    static std::shared_ptr<Http2Connection> create(StreamPtr stream, std::string key) {
        return std::make_shared<Http2Connection>(std::move(stream), std::move(key));
    }

    // --- IConnection 接口实现 ---
    boost::asio::awaitable<HttpResponse> execute(HttpRequest request) override;
    bool is_usable() const override;
    boost::asio::awaitable<void> close() override;
    const std::string& id() const override { return id_; }
    const std::string& get_pool_key() const override { return pool_key_; }
    size_t get_active_streams() const override { return active_streams_.load(); }
    int64_t get_last_used_timestamp_seconds() const override{ return last_used_timestamp_seconds_; }
    boost::asio::awaitable<bool> ping() override;

    bool supports_multiplexing() const override { return true; }
    size_t get_max_concurrent_streams() const override{ return max_concurrent_streams_.load();}

    void update_last_used_time() override;

    // --- H2 客户端特定方法 ---
    boost::asio::awaitable<void> start();


private:
    struct StreamContext {
        explicit StreamContext(const boost::asio::any_io_executor& ex)
            : response_channel(ex, 1) {}

        ResponseChannel response_channel;
        HttpResponse response_in_progress;
        std::string request_body;
        size_t request_body_offset = 0;
        std::vector<std::string> header_storage;
    };


    void init_nghttp2_session();
    boost::asio::awaitable<void> session_loop();
    boost::asio::awaitable<void> do_read();
    boost::asio::awaitable<void> do_write();
    void prepare_headers(std::vector<nghttp2_nv>& nva, const HttpRequest& req, StreamContext& stream_ctx);

    // --- nghttp2 C-style 静态回调函数 ---
    static int on_begin_headers_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);
    static int on_header_callback(nghttp2_session* session, const nghttp2_frame* frame, const uint8_t* name, size_t namelen, const uint8_t* value, size_t valuelen, uint8_t flags, void* user_data);
    static int on_data_chunk_recv_callback(nghttp2_session* session, uint8_t flags, int32_t stream_id, const uint8_t* data, size_t len, void* user_data);
    static int on_stream_close_callback(nghttp2_session* session, int32_t stream_id, uint32_t error_code, void* user_data);
    static int on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);
    static ssize_t read_request_body_callback(nghttp2_session* session, int32_t stream_id, uint8_t* buf, size_t length, uint32_t* data_flags, nghttp2_data_source* source, void* user_data);


    // --- 成员变量 ---
    StreamPtr   stream_;
    std::string pool_key_;
    std::string id_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    nghttp2_session* session_ = nullptr;
    std::unordered_map<int32_t, std::unique_ptr<StreamContext>> streams_;

    std::atomic<bool> is_closing_{false};
    std::atomic<bool> handshake_completed_{false};
    std::atomic<bool> close_called_{false}; // 用于防止 close() 重入



    static std::string generate_simple_uuid();

    std::atomic<size_t> active_streams_{0}; // 0 表示空闲, 1 表示繁忙

    // 用于记录最后一次活动时间
    int64_t last_used_timestamp_seconds_;

    std::atomic<size_t> max_concurrent_streams_{100}; // 默认值

};


#endif //UNTITLED1_H2_CONNECTION_HPP