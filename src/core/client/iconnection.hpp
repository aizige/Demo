//
// Created by Aiziboy on 2025/7/18.
//

#ifndef ICONNECTION_HPP
#define ICONNECTION_HPP

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "http/http_common_types.hpp"

class IConnection {
public:
    virtual ~IConnection() = default;

    // 核心功能：异步执行一个请求并返回一个响应
    // 这是所有连接子类必须实现的纯虚函数
    virtual boost::asio::awaitable<HttpResponse> execute(HttpRequest request) = 0;

    // 检查连接是否仍然可用且可以被复用
    virtual bool is_usable() const = 0;

    // 主动关闭连接
    virtual boost::asio::awaitable<void> close() = 0;

    // 获取连接的唯一ID，用于连接池管理
    virtual const std::string& id() const = 0;

    virtual const std::string& get_pool_key() const = 0;

    //  健康检查
    /**
     * @brief 返回当前连接上正在处理的并发请求（流）的数量。
     * @return 对于 H1.1，这个值是 0 或 1。对于 H2，可以是 0 到 max_concurrent_streams。
     */
    virtual size_t get_active_streams() const = 0;
    virtual boost::asio::awaitable<bool> ping() = 0; // 新增 ping 接口
    virtual int64_t get_last_used_timestamp_seconds() const = 0; // 新增时间戳接口

    /**
 * @brief 释放并返回底层 TCP socket 的所有权。
 * 这个方法主要用于 H2C Upgrade 流程，当一个 HTTP/1.1 连接需要
 * 转换成一个 H2C 连接时，需要转移底层的 socket。
 * 调用后，此 IConnection 对象将变得不可用。
 * @return 一个 tcp::socket 对象。
 */
    virtual boost::asio::awaitable<std::optional<boost::asio::ip::tcp::socket>>  release_socket() {
        // 提供一个默认实现，因为只有 Http1Connection 需要它。
        // 其他连接类型（如 H2）不支持升级，调用此方法会返回一个关闭的 socket。
        // 默认实现返回一个空的 optional
        co_return std::nullopt;
    }
};


#endif //ICONNECTION_HPP