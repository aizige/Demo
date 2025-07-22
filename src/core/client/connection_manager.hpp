//
// Created by Aiziboy on 2025/7/18.
//

#ifndef UNTITLED1_CONNECTION_MANAGER_HPP
#define UNTITLED1_CONNECTION_MANAGER_HPP
#include <map>
#include <queue>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/experimental/promise.hpp>
#include <boost/asio/ssl/context.hpp>
// 向前声明
class IConnection;

struct PooledConnection {
    std::shared_ptr<IConnection> connection;
    bool is_reused; // true 表示是从池中复用的
};


class ConnectionManager {
public:
    // 构造函数，需要一个 io_context 的引用
    explicit ConnectionManager(boost::asio::io_context& ioc);

    void replace_connection(std::shared_ptr<IConnection> old_conn, std::shared_ptr<IConnection> new_conn);

    // 析构和移动操作（如果需要的话，但通常默认的就行）
    ~ConnectionManager() = default;
    ConnectionManager(ConnectionManager&&) noexcept = default;
    ConnectionManager& operator=(ConnectionManager&&) noexcept = default;

    // 核心功能：异步获取一个连接
    // scheme: "http" or "https"
    // host: "example.com"
    // port: 80 or 443
    boost::asio::awaitable<PooledConnection> get_connection(
        std::string_view scheme,
        std::string_view host,
        uint16_t port
    );

    // 将使用完毕的连接归还给池子
    void release_connection(std::shared_ptr<IConnection> conn);

private:
    // 异步创建新连接的私有方法
    boost::asio::awaitable<std::shared_ptr<IConnection>> create_new_connection(
        std::string_view scheme,
        std::string_view host,
        uint16_t port
    );

    boost::asio::io_context& ioc_;
    // **直接持有 ssl::context 对象**
    boost::asio::ssl::context ssl_ctx_;
    boost::asio::ip::tcp::resolver resolver_;



    // 连接池的核心数据结构
    // Key: "http://example.com:80"
    // Value: 一个该目标可用的空闲连接队列
    std::map<std::string, std::queue<std::shared_ptr<IConnection>>> pool_;

    std::map<std::string, boost::asio::strand<boost::asio::any_io_executor>> creation_strands_;

    std::mutex pool_mutex_; // 用于保护连接池的互斥锁

};


#endif //UNTITLED1_CONNECTION_MANAGER_HPP