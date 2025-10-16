//
// Created by ubuntu on 2025/7/21.
//

#ifndef UNTITLED1_H2_CONNECTION_HPP
#define UNTITLED1_H2_CONNECTION_HPP
#include "iconnection.hpp"
#include <nghttp2/nghttp2.h>
#include <boost/beast/ssl.hpp>
#include <atomic>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <spdlog/spdlog.h>
#include <iostream>
#include <boost/asio/experimental/channel.hpp>

#include "core/server.hpp"
#include "http/http_common_types.hpp"
// 单协程融合模型 (Single-Coroutine Fused Model)

// 定义 Actor 的“消息”类型。
// 当外部代码调用 execute() 方法时，它会创建一个 H2RequestMessage，
// 并通过 `RequestChannel` 将其发送给 actor_loop() 协程。
struct H2RequestMessage {
    HttpRequest request;
    // 每个请求都包含一个自己的响应通道。这就像是消息的“返回地址”。
    // actor_loop() 完成请求处理后，会通过这个 channel 将最终的 HttpResponse 发回给等待的 execute() 协程。
    std::shared_ptr<boost::asio::experimental::channel<void(boost::system::error_code, HttpResponse)>> response_channel;
};

// 定义 Actor 的“邮箱”（Mailbox）类型。这是一个协程安全的消息队列。
// `execute()` 方法是生产者，`actor_loop()` 是消费者。
using RequestChannel = boost::asio::experimental::channel<void(boost::system::error_code, H2RequestMessage)>;



/**
 * @class Http2Connection
 * @brief 管理一个到远程服务器的 HTTP/2 客户端连接。
 *
 * 这个类实现为一个 Actor 模型，其中一个名为 actor_loop() 的单一协程
 * 负责管理此连接的所有状态和 I/O 操作。这种设计从根本上保证了线程安全，
 * 因为所有对共享状态（如 nghttp2_session, streams_ map）的访问都发生在该协程内部，
 * 无需使用互斥锁或 strand。
 *
 * 它支持 HTTP/2 的多路复用特性，可以在一个 TCP 连接上并行处理多个请求。
 */
class Http2Connection final : public IConnection, public std::enable_shared_from_this<Http2Connection> {
public:
        /**
         * @struct StreamContext
         * @brief 存储单个 HTTP/2 流（即一个请求-响应对）的上下文信息。
         */
    struct StreamContext {
        // 指向该流的响应通道，用于将最终结果发回。
        std::shared_ptr<boost::asio::experimental::channel<void(boost::system::error_code, HttpResponse)>> response_channel;
        // 在接收过程中逐步构建的 HttpResponse 对象。
        HttpResponse response_in_progress;
        // 存储待发送的请求体。
        std::string request_body;
        // 请求体已发送的数据偏移量，用于 nghttp2 的 read_callback。
        size_t request_body_offset = 0;
        // **关键的内存管理**：由于 nghttp2_nv 结构体只存储原始指针，
        // 我们必须将所有 header 的 key-value 字符串存储在这里，以确保在 nghttp2 使用它们时，
        // 这些字符串的内存是有效的，避免悬空指针。
        std::vector<std::string> header_storage;
    };

    // 定义底层流的类型，这里是 SSL 加密的 TCP 流。
    using StreamType = boost::beast::ssl_stream<tcp::socket>;
    using StreamPtr = std::shared_ptr<StreamType>;

    Http2Connection(StreamPtr stream, std::string pool_key);
    ~Http2Connection() override;


    // 禁止拷贝和赋值，因为每个连接都是唯一的。
    Http2Connection(const Http2Connection&) = delete;
    Http2Connection& operator=(const Http2Connection&) = delete;

    // 工厂方法，用于安全地创建被 std::shared_ptr 管理的对象。
    static std::shared_ptr<Http2Connection> create(StreamPtr stream, std::string key) {
        return std::make_shared<Http2Connection>(std::move(stream), std::move(key));
    }

    /**
     * @brief 启动连接的 Actor。
     *        此方法会 `co_spawn` 后台的 `actor_loop` 协程，并等待 HTTP/2 握手完成。
     * @return 一个可等待对象，在握手成功或失败后完成。
     */
    boost::asio::awaitable<void> run();


    // --- IConnection 接口实现 ---

    /**
     * @brief 异步执行一个 HTTP 请求。
     *        此方法将请求打包成消息发送给 `actor_loop`，然后等待响应。
     * @param request 要发送的 HttpRequest 对象。
     * @return 一个可等待对象，其结果是服务器返回的 HttpResponse。
     */
    boost::asio::awaitable<HttpResponse> execute(const HttpRequest& request) override;

    /**
     * @brief 检查连接当前是否可用。
     * @return 如果连接健康、未关闭且握手完成，则返回 true。
     */
    bool is_usable() const override;

    /**
     * @brief 异步关闭连接。
     *        向 `actor_loop` 发出关闭信号，并清理资源。
     */
    boost::asio::awaitable<void> close() override;

    // 获取连接的唯一ID。
    const std::string& id() const override { return id_; }
    // 获取连接所属的连接池的键。
    const std::string& get_pool_key() const override { return pool_key_; }
    // 获取最底层的 TCP socket，用于某些底层操作（如设置 socket 选项）。
    tcp::socket& lowest_layer_socket() const;



    // --- 连接池支持 ---

    // 更新最后使用时间戳，用于连接池的空闲回收策略。
    // 更新最后ping操作的时间戳，用于连接保活
    void update_last_used_time() override;
    void update_ping_used_time() override;
    // 获取最后使用时间戳（秒）。
    int64_t get_last_used_timestamp_seconds() const override { return last_used_timestamp_seconds_.load(); }
    // 更新最后一次进行ping操作的时间戳
    int64_t get_ping_used_timestamp_seconds() const override{return last_ping_timestamp_seconds_.load();}
    // 发送 PING 帧以检查连接的活性。
    boost::asio::awaitable<bool> ping() override;
    // 返回 true，因为 HTTP/2 支持多路复用。
    bool supports_multiplexing() const override { return true; }
    // 获取当前正在处理的请求（流）数量。
    size_t get_active_streams() const override;
    // 获取服务器通告的最大并发流数量。
    size_t get_max_concurrent_streams() const override { return max_concurrent_streams_.load(); }


private:



    /**
     * @brief 唯一的、统一的 Actor 协程，管理所有状态和 I/O。
     *        这是此类的核心，在一个循环中处理网络读、新请求和定时器事件。
     */
    boost::asio::awaitable<void> actor_loop();

    /**
     * @brief 由 actor_loop 调用的写操作辅助函数。
     *        负责从 nghttp2 引擎中取出数据并写入网络。
     */
    boost::asio::awaitable<void> do_write();


    /**
     * @brief 将 HttpRequest 头转换为 nghttp2_nv 格式。
     *        同时负责管理头字符串的生命周期。
     */
    void prepare_headers(std::vector<nghttp2_nv>& nva, const HttpRequest& req, StreamContext& stream_ctx);

    /**
     * @brief 处理流关闭事件的内部函数。
     */
    void handle_stream_close(int32_t stream_id, uint32_t error_code);


    // --- nghttp2 C-style 静态回调函数 ---
    static int on_begin_headers_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);
    static int on_header_callback(nghttp2_session* session, const nghttp2_frame* frame, const uint8_t* name, size_t name_len, const uint8_t* value, size_t value_len, uint8_t flags, void* user_data);
    static int on_data_chunk_recv_callback(nghttp2_session* session, uint8_t flags, int32_t stream_id, const uint8_t* data, size_t len, void* user_data);
    static int on_stream_close_callback(nghttp2_session* session, int32_t stream_id, uint32_t error_code, void* user_data);
    static int on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);
    static ssize_t read_request_body_callback(nghttp2_session* session, int32_t stream_id, uint8_t* buf, size_t length, uint32_t* data_flags, nghttp2_data_source* source, void* user_data);


    // --- 成员变量 ---
    StreamPtr stream_;
    std::string pool_key_;
    std::string id_;

    RequestChannel request_channel_; // Actor 的“邮箱”，用于接收新请求。
    boost::asio::experimental::channel<void(boost::system::error_code)> handshake_signal_; // 用于在 run() 和 actor_loop() 之间同步握手状态的信号。
    std::array<char, 8192> read_buffer_{}; // 网络读取缓冲区。


    nghttp2_session* session_ = nullptr; // nghttp2 的会话状态机。
    std::unordered_map<int32_t, std::unique_ptr<StreamContext>> streams_; // 存储所有活跃流的上下文。

    // 使用原子变量来允许从外部线程（如连接池管理器）安全地读取连接状态。
    std::atomic<bool> is_closing_{false}; // 标记连接是否正在关闭。
    std::atomic<bool> close_called_{false}; // 防止 close() 被多次调用。
    std::atomic<bool> handshake_completed_{false}; // 标记 HTTP/2 握手是否已完成。
    std::atomic<size_t> max_concurrent_streams_{100}; // 服务器允许的最大并发流数。
    std::atomic<int64_t> last_used_timestamp_seconds_; // 连接最后一次被使用的时间戳（秒）
     std::atomic<int64_t>  last_ping_timestamp_seconds_; // 上次PING的时间戳秒数（秒）。
    std::atomic<size_t> active_streams_{0}; // 当前活跃的流数量。
    std::atomic<bool> remote_goaway_received_{false}; // 标记是否已收到服务器的 GOAWAY 帧。

    boost::asio::steady_timer idle_timer_; // 用于在连接空闲时触发超时的计时器。

    /**
     * @brief 生成一个简单的、用于调试的唯一ID。
     */
    static std::string generate_connection_id();

};


#endif //UNTITLED1_H2_CONNECTION_HPP
