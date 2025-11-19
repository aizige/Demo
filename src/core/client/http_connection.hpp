//
// Created by Aiziboy on 2025/7/18.
//

#ifndef UNTITLED1_HTTP_CONNECTION_HPP
#define UNTITLED1_HTTP_CONNECTION_HPP

#include "iconnection.hpp"
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <spdlog/spdlog.h>
#include <atomic>
#include <memory>
#include <string>
#include <boost/beast/core/flat_buffer.hpp>

#include "http/http_common_types.hpp"


/**
 * @class HttpConnection
 * @brief IConnection 的具体实现，用于处理纯文本的 HTTP/1.1 连接。
 *
 * 这个类封装了通过 boost::beast::tcp_stream 进行的普通 HTTP 通信的
 * 所有细节。它是 HttpsConnection 的非加密版本。
 *
 * 它的生命周期由 std::shared_ptr 管理，以确保在所有相关的异步操作
 * 完成之前，连接对象不会被销毁。
 */
class HttpConnection final : public IConnection, public std::enable_shared_from_this<HttpConnection> {
public:
    /**
     * @brief 构造函数。
     * @param socket 一个已经建立的 TCP socket。所有权被转移给 HttpConnection。
     * @param pool_key 此连接所属的连接池键 (e.g., "http://example.com:80")。
     */
    explicit HttpConnection(tcp::socket socket, std::string pool_key);

    /**
     * @brief 析构函数。在对象销毁时记录日志。
     */
    ~HttpConnection() override {
        SPDLOG_DEBUG("[{}]-[{}] 销毁", id_,pool_key_);
    }

    // 禁止拷贝构造和拷贝赋值，因为每个连接都是唯一的资源。
    HttpConnection(const HttpConnection&) = delete;
    HttpConnection& operator=(const HttpConnection&) = delete;

    // --- IConnection 接口实现 ---
    // (以下 override 的方法，其核心文档在 IConnection.hpp 中)

    boost::asio::awaitable<HttpResponse> execute(const HttpRequest& request) override;

    bool is_usable() const override;

    boost::asio::awaitable<void> close() override;

    const std::string& id() const override { return id_; }

    const std::string& get_pool_key() const override { return pool_key_; }

    size_t get_active_streams() const override;

    boost::asio::awaitable<bool> ping() override;

    std::chrono::steady_clock::time_point get_last_used_timestamp_ms() const override { return last_used_timestamp_ms_; }
    std::chrono::steady_clock::time_point get_ping_used_timestamp_ms() const override{return last_ping_timestamp_ms_;}

    void update_last_used_time() override;
    void update_ping_used_time() override;

    // 对于 HTTP/1.1, 并发流总是 1，且不支持多路复用。
    size_t get_max_concurrent_streams() const override { return 1; }
    bool supports_multiplexing() const override { return false; }

private:
    /**
     * @brief 生成一个简单的、用于调试的唯一ID。
     */
    static std::string generate_connection_id();

    // --- 成员变量 ---

    /// @brief Beast 的 TCP 流，封装了 socket 并提供了面向流的 I/O 操作。
    boost::beast::tcp_stream socket_;

    /// @brief 用于 Beast 读写操作的可复用缓冲区，以提高性能，避免重复内存分配。
    boost::beast::flat_buffer buffer_;

    /// @brief 此连接实例的唯一标识符，主要用于日志输出以追踪连接行为。
    std::string id_;

    /// @brief 协议层面的 Keep-Alive 状态标志。
    /// 若为 false，is_usable() 将返回 false，连接将在使用后被丢弃。
    bool keep_alive_ = true;

    /// @brief 此连接所属的连接池键。
    std::string pool_key_;

    /// @brief 原子计数器，记录当前是否正在处理请求 (0 表示空闲, 1 表示繁忙)。
    /// 用于 `is_usable()` 和 `ping()` 判断连接是否繁忙。
    std::atomic<size_t> active_streams_{0};

    /// @brief 连接最后一次被使用的时间点，用于连接池的空闲超时管理。
    std::chrono::steady_clock::time_point last_used_timestamp_ms_;

    /// @brief 上次PING的时间点
    /// 由 ConnectionManager 的后台维护任务使用，以判断连接多久没ping过了
    std::chrono::steady_clock::time_point last_ping_timestamp_ms_;
};

#endif //UNTITLED1_HTTP_CONNECTION_HPP
