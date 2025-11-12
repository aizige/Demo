//
// Created by ubuntu on 2025/7/21.
//

#ifndef UNTITLED1_HTTPS_CONNECTION_HPP
#define UNTITLED1_HTTPS_CONNECTION_HPP

#include <boost/asio/awaitable.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include "iconnection.hpp"
#include <atomic>
#include <memory>
#include <spdlog/spdlog.h>

/**
 * @class HttpsConnection
 * @brief IConnection 的具体实现，用于处理基于 TLS 的 HTTP/1.1 连接。
 *
 * 这个类封装了通过 boost::beast::ssl_stream 进行的 HTTPS 通信的
 * 所有细节，包括请求的发送、响应的接收、Keep-Alive 管理以及
 * 应用层心跳 (PING) 的实现。
 *
 * 它的生命周期由 std::shared_ptr 管理，以确保在所有异步操作（如 execute, ping）
 * 完成之前，连接对象不会被销毁。
 */
class HttpsConnection final : public IConnection, public std::enable_shared_from_this<HttpsConnection> {
public:
    using StreamType = boost::beast::ssl_stream<boost::asio::ip::tcp::socket>;
    using StreamPtr = std::shared_ptr<StreamType>;

    /**
     * @brief 构造函数。
     * @param stream 一个已经完成 TLS 握手的 SSL 流。
     * @param pool_key 此连接所属的连接池键。
     */
    explicit HttpsConnection(StreamPtr stream, std::string pool_key);

    /**
     * @brief HttpsConnection 类的析构函数。
     *        目前为空，因为所有资源都由智能指针和 RAII 对象管理。
     */
    ~HttpsConnection() override{
        SPDLOG_DEBUG("HttpsConnection [{}] destroyed.", id_);
    }



    // --- IConnection 接口实现 ---
    // (以下 override 的方法，其核心文档在 IConnection.hpp 中，此处不再重复)

    boost::asio::awaitable<HttpResponse> execute(const HttpRequest& request) override;
    bool is_usable() const override;
    boost::asio::awaitable<void> close() override;
    const std::string& id() const override;
    const std::string& get_pool_key() const override;
    boost::asio::awaitable<bool> ping() override;
    void update_last_used_time() override;
    void update_ping_used_time() override;

    // 对于 HTTP/1.1, 并发流总是 1，且不支持多路复用。
    size_t get_max_concurrent_streams() const override { return 1; }
    bool supports_multiplexing() const override { return false; }


    size_t get_active_streams() const override{ return active_streams_.load(); }
    int64_t get_last_used_timestamp_ms() const override{return last_used_timestamp_ms_;}
    int64_t get_ping_used_timestamp_ms() const override{return last_ping_timestamp_ms_;}

private:
    /**
     * @brief 生成一个简单的、用于调试的唯一ID。
     */
    static std::string generate_connection_id();

    // --- 成员变量 ---

    /// @brief 指向底层 SSL/TLS 流的共享指针，是所有 I/O 操作的对象。
    StreamPtr stream_;

    /// @brief 用于 Beast 读写操作的可复用缓冲区，以提高性能。
    boost::beast::flat_buffer buffer_;

    /// @brief 此连接实例的唯一标识符，主要用于日志输出。
    std::string id_;

    /// @brief 此连接所属的连接池键 (e.g., "https://example.com:443")。
    std::string pool_key_;

    /// @brief 协议层面的 Keep-Alive 状态标志。
    /// 如果收到 "Connection: close" 头部或发生不可恢复的错误，此标志会变为 false。
    bool keep_alive_ = false;

    /// @brief 原子计数器，记录当前正在处理的请求数 (对于 H1.1 永远是 0 或 1)。
    /// 用于 `is_usable()` 和 `ping()` 判断连接是否繁忙。
    std::atomic<size_t> active_streams_{0};

    /// @brief 连接最后一次被使用的时间戳（ms）。
    /// 由 ConnectionManager 的后台维护任务使用，以判断连接是否空闲。
    int64_t last_used_timestamp_ms_;

    /// @brief 上次PING的时间戳（ms）。
    /// 由 ConnectionManager 的后台维护任务使用，以判断连接多久没ping过了
    int64_t last_ping_timestamp_ms_;
};


#endif //UNTITLED1_HTTPS_CONNECTION_HPP