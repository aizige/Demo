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
#include "h2_connection.hpp" // 复用 H2RequestMessage 和 RequestChannel 的定义

/**
 * @class Http2cConnection
 * @brief 管理一个到远程服务器的明文 HTTP/2 (H2C) 客户端连接。
 *
 * 这个类实现为一个 Actor 模型，其中一个名为 actor_loop() 的单一协程
 * 负责管理此连接的所有状态和 I/O 操作，确保线程安全和无死锁。
 * 它支持多路复用，可以在一个 TCP 连接上并行处理多个请求。
 */
class Http2cConnection : public IConnection, public std::enable_shared_from_this<Http2cConnection> {
public:
    using StreamType = boost::beast::tcp_stream;
    using StreamPtr = std::shared_ptr<StreamType>;

    Http2cConnection(StreamPtr stream, std::string pool_key);
    ~Http2cConnection() override;

    Http2cConnection(const Http2cConnection&) = delete;
    Http2cConnection& operator=(const Http2cConnection&) = delete;

    static std::shared_ptr<Http2cConnection> create(StreamPtr stream, std::string key) {
        return std::make_shared<Http2cConnection>(std::move(stream), std::move(key));
    }

    // 同步方法，用于启动后台运行的 Actor。
    void run();

    // --- IConnection 接口实现 ---
    boost::asio::awaitable<HttpResponse> execute(HttpRequest request) override;
    bool is_usable() const override;
    boost::asio::awaitable<void> close() override;
    const std::string& id() const override { return id_; }
    const std::string& get_pool_key() const override { return pool_key_; }
    tcp::socket& lowest_layer_socket();

    // --- 连接池支持 ---
    void update_last_used_time() override;
    int64_t get_last_used_timestamp_seconds() const override { return last_used_timestamp_seconds_.load(); }
    boost::asio::awaitable<bool> ping() override;
    bool supports_multiplexing() const override { return true; }
    size_t get_active_streams() const override;
    size_t get_max_concurrent_streams() const override { return max_concurrent_streams_.load(); }

    // H2C 不涉及 TLS，所以不能“窃取”套接字
    boost::asio::awaitable<std::optional<tcp::socket>> release_socket() override { co_return std::nullopt; }

private:
    // H2 和 H2C 的 StreamContext 是完全一样的
    using StreamContext = Http2Connection::StreamContext;

    // 唯一的、统一的 Actor 协程
    boost::asio::awaitable<void> actor_loop();

    // 写操作辅助函数
    boost::asio::awaitable<void> do_write();

    void prepare_headers(std::vector<nghttp2_nv>& nva, const HttpRequest& req, StreamContext& stream_ctx);
    void handle_stream_close(int32_t stream_id, uint32_t error_code);

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

    RequestChannel request_channel_;
    std::array<char, 8192> read_buffer_{};

    nghttp2_session* session_ = nullptr;
    std::unordered_map<int32_t, std::unique_ptr<StreamContext>> streams_;

    std::atomic<bool> is_closing_{false};
    std::atomic<bool> close_called_{false};
    std::atomic<bool> handshake_completed_{false};

    std::atomic<size_t> max_concurrent_streams_{100};
    std::atomic<int64_t> last_used_timestamp_seconds_;
    std::atomic<size_t> active_streams_{0};

    static std::string generate_simple_uuid();
};


#endif //UNTITLED1_H2C_CONNECTION_HPP