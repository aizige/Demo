//
// Created by [Your Name] on [Date].
//

#ifndef UNTITLED1_H2C_CONNECTION_HPP
#define UNTITLED1_H2C_CONNECTION_HPP

#include "iconnection.hpp"
#include <nghttp2/nghttp2.h>
// [!!! 关键区别 1: 包含 tcp_stream 而不是 ssl_stream !!!]
#include <boost/beast/core/tcp_stream.hpp>
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <spdlog/spdlog.h>
#include <iostream>
#include <boost/asio/experimental/channel.hpp>

#include "h2_connection.hpp"
#include "http/http_common_types.hpp"

// 复用 H2Connection 的消息结构体
struct H2RequestMessage;
using RequestChannel = boost::asio::experimental::channel<void(boost::system::error_code, H2RequestMessage)>;

class Http2cConnection : public IConnection, public std::enable_shared_from_this<Http2cConnection> {
public:
    // H2 和 H2C 的 StreamContext 是完全一样的
    using StreamContext = Http2Connection::StreamContext;

    // [!!! 关键区别 2: StreamType 现在是 tcp_stream !!!]
    using StreamType = boost::beast::tcp_stream;
    using StreamPtr = std::shared_ptr<StreamType>;

    Http2cConnection(StreamPtr stream, std::string pool_key);
    ~Http2cConnection() override;

    Http2cConnection(const Http2cConnection&) = delete;
    Http2cConnection& operator=(const Http2cConnection&) = delete;

    static std::shared_ptr<Http2cConnection> create(StreamPtr stream, std::string key) {
        return std::make_shared<Http2cConnection>(std::move(stream), std::move(key));
    }

    // run() 方法现在是同步的，因为它不需要等待TLS握手
    void run();

    // --- IConnection 接口实现 (与 H2Connection 签名完全相同) ---
    boost::asio::awaitable<HttpResponse> execute(HttpRequest request) override;
    bool is_usable() const override;
    boost::asio::awaitable<void> close() override;
    const std::string& id() const override { return id_; }
    const std::string& get_pool_key() const override { return pool_key_; }
    tcp::socket& lowest_layer_socket();
    void update_last_used_time() override;
    int64_t get_last_used_timestamp_seconds() const override{ return last_used_timestamp_seconds_.load(); }
    boost::asio::awaitable<bool> ping() override;
    bool supports_multiplexing() const override { return true; }
    size_t get_active_streams() const override;
    size_t get_max_concurrent_streams() const override{ return max_concurrent_streams_.load(); }



private:
    // ... 所有私有方法和成员变量与 H2Connection 完全相同 ...
    // ... 唯一的区别是 stream_ 的类型 ...
    boost::asio::awaitable<void> actor_loop();
    boost::asio::awaitable<void> do_write();
    void prepare_headers(std::vector<nghttp2_nv>& nva, const HttpRequest& req, StreamContext& stream_ctx);
    void handle_stream_close(int32_t stream_id, uint32_t error_code);

    // C-style 回调
    static int on_begin_headers_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);
    static int on_header_callback(nghttp2_session* session, const nghttp2_frame* frame, const uint8_t* name, size_t namelen, const uint8_t* value, size_t valuelen, uint8_t flags, void* user_data);
    static int on_data_chunk_recv_callback(nghttp2_session* session, uint8_t flags, int32_t stream_id, const uint8_t* data, size_t len, void* user_data);
    static int on_stream_close_callback(nghttp2_session* session, int32_t stream_id, uint32_t error_code, void* user_data);
    static int on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);
    static ssize_t read_request_body_callback(nghttp2_session* session, int32_t stream_id, uint8_t* buf, size_t length, uint32_t* data_flags, nghttp2_data_source* source, void* user_data);

    StreamPtr stream_;
    std::string pool_key_;
    std::string id_;
    RequestChannel request_channel_;
    std::array<char, 8192> read_buffer_{};
    nghttp2_session* session_ = nullptr;
    std::unordered_map<int32_t, std::unique_ptr<StreamContext>> streams_; // 复用类型
    std::atomic<bool> is_closing_{false};
    std::atomic<bool> close_called_{false};
    std::atomic<bool> handshake_completed_{false}; // 在 H2C 中，这代表 preface 已发送
    std::atomic<size_t> max_concurrent_streams_{100};
    std::atomic<int64_t> last_used_timestamp_seconds_;
    std::atomic<size_t> active_streams_{0};
    std::atomic<bool> remote_goaway_received_{false};

    static std::string generate_simple_uuid();
};


#endif //UNTITLED1_H2C_CONNECTION_HPP