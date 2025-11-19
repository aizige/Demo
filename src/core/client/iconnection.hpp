//
// Created by Aiziboy on 2025/7/18.
//

#ifndef ICONNECTION_HPP
#define ICONNECTION_HPP

#include <boost/asio/awaitable.hpp>
#include "http/http_common_types.hpp"
#include <cstdint> // for int64_t
#include <cstddef> // for size_t

/**
 * @interface IConnection
 * @brief 定义了一个通用客户端网络连接的抽象接口。
 *
 * 这个接口是连接池管理的核心抽象，它统一了所有具体连接实现
 * (如 HTTP/1.1, HTTPS, HTTP/2) 的公共行为。
 * 所有实现都必须是可共享的（通过 std::shared_ptr 管理）并且是线程安全的。
 */
class IConnection {
public:
    /**
     * @brief 虚析构函数，允许通过基类指针安全地销毁派生类对象。
     */
    virtual ~IConnection() = default;

    /**
     * @brief 异步地在此连接上执行一个 HTTP 请求。
     * @param request 要发送的 HttpRequest 对象。
     * @return 一个协程句柄 (awaitable)，其最终结果是一个 HttpResponse 对象。
     */
    virtual boost::asio::awaitable<HttpResponse> execute(const HttpRequest& request) = 0;

    /**
     * @brief 判断此连接当前是否健康且可供连接池复用。
     *
     * 这是连接池管理的关键函数。一个可用的连接通常意味着其底层的网络套接字
     * 仍然打开，协议层也处于可以发起新请求的正常状态。
     *
     * @return 如果连接可用于处理新请求，则返回 `true`；否则返回 `false`。
     */
    [[nodiscard]] virtual bool is_usable() const = 0;

    /**
     * @brief 异步地、主动地关闭此连接。
     *
     * 这个函数应该实现一个优雅的关闭流程，例如发送必要的关闭通知
     * (如 TLS close_notify, HTTP/2 GOAWAY)，并关闭底层套接字。
     *
     * @return 一个协程句柄，调用者可以 `co_await` 它来等待关闭完成。
     */
    virtual boost::asio::awaitable<void> close() = 0;

    /**
     * @brief 获取此连接实例的唯一标识符。
     * 主要用于日志和调试。
     * @return 对连接唯一ID字符串的 const 引用。
     */
    [[nodiscard]] virtual const std::string& id() const = 0;

    /**
     * @brief 获取此连接所属的连接池键。
     * 池键由 "scheme//host:port" 构成，用于标识连接的目标。(scheme以Ada库的为标准是带":"的)
     * @return 对连接池键字符串的 const 引用。
     */
    [[nodiscard]] virtual const std::string& get_pool_key() const = 0;

    /**
     * @brief 执行一次应用层心跳检测 (PING) 以验证连接的端到端健康状况。
     * @return 一个协程句柄，其结果为 `true` 表示连接健康，`false` 表示不健康。
     */
    virtual boost::asio::awaitable<bool> ping() = 0;

    /**
     * @brief 更新连接的最后使用时间戳为当前时间。
     * 连接池的维护任务会使用此时间戳来判断连接是否空闲。
     */
    virtual void update_last_used_time() = 0;

    /**
     * @brief 更新连接的最后一次ping时间戳为当前时间。
     * 连接池的维护任务会使用此时间戳来判断连接是否空闲。
     */
    virtual void update_ping_used_time() = 0;

    /**
     * @brief 获取连接的最后一次活动时间戳。
     * @return 自某个固定时间点以来的毫秒数
     */
    [[nodiscard]] virtual std::chrono::steady_clock::time_point get_last_used_timestamp_ms() const = 0;



    /// @brief 上次PING的时间戳（ms）。
    /// 由 ConnectionManager 的后台维护任务使用，以判断连接多久没ping过了
    [[nodiscard]] virtual std::chrono::steady_clock::time_point get_ping_used_timestamp_ms() const =0;

    /**
     * @brief 返回当前连接上正在并发处理的请求（流）的数量。
     * @return 对于 HTTP/1.1，这个值通常是 0 或 1。
     *         对于 HTTP/2，它可以是 0 到服务器允许的最大并发流数。
     */
    [[nodiscard]] virtual size_t get_active_streams() const = 0;

    /**
     * @brief 获取服务器通过协议协商告知的最大并发流数。
     * @return 对于不支持多路复用的协议 (如 HTTP/1.1)，应返回 1。
     *         对于 HTTP/2，返回从服务器 SETTINGS 帧中获取的值。
     */
    [[nodiscard]] virtual size_t get_max_concurrent_streams() const = 0;

    /**
     * @brief 查询该连接是否支持在单个TCP连接上进行应用层多路复用。
     * @return HTTP/2 和 HTTP/3 连接应返回 `true`。
     *         HTTP/1.1 连接应返回 `false`。
     */
    [[nodiscard]] virtual bool supports_multiplexing() const { return false; }
};

#endif //ICONNECTION_HPP
