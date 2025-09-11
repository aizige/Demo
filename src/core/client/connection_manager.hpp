//
// Created by Aiziboy on 2025/7/18.
//

#ifndef UNTITLED1_CONNECTION_MANAGER_HPP
#define UNTITLED1_CONNECTION_MANAGER_HPP

#include <queue>
#include <unordered_set>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <boost/asio/ssl/context.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
// 向前声明
class IConnection;

struct PooledConnection {
    std::shared_ptr<IConnection> connection;
    bool is_reused = false; // true 表示是从池中复用的
};


/**
 * @brief 一个现代化的、基于 strand 和协程的 HTTP/1.1 & HTTP/2 连接池管理器。
 *
 * 该管理器线程安全，并提供可选的后台维护任务以实现主动保活和陈旧连接清理。
 */
class ConnectionManager  : public std::enable_shared_from_this<ConnectionManager> {
public:
    /**
     * @brief 构造函数。
     * @param ioc io_context 的引用。
     * @param enable_maintenance 是否启用后台维护任务（PING保活和清理）。
     */
    explicit ConnectionManager(boost::asio::io_context& ioc, bool enable_maintenance = true);

    ~ConnectionManager();

    // 禁止拷贝和移动，因为它管理着后台任务和状态。
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    /**
     * @brief 异步获取一个到指定目标的健康连接。
     *        此方法是线程安全的。
     * @param scheme "http" 或 "https".
     * @param host 主机名，例如 "www.google.com".
     * @param port 端口号。
     * @return 一个 awaitable，其结果是一个 IConnection 的 shared_ptr。
     */
    boost::asio::awaitable<PooledConnection>  get_connection(
        std::string_view scheme, std::string_view host, uint16_t port);

    /**
     * @brief 将一个连接释放回池中以供复用。
     *        此方法是线程安全的。
     * @param conn 要释放的连接。
     */
    void release_connection(const std::shared_ptr<IConnection>& conn);


    // 负责关闭所有连接
    boost::asio::awaitable<void> stop();
private:


    // 在 strand 上创建新连接的私有协程
    boost::asio::awaitable<std::shared_ptr<IConnection>> create_new_connection(
        const std::string& key, std::string_view scheme, std::string_view host, uint16_t port);

    // 启动后台维护任务
    void start_maintenance();

    // 后台维护任务的协程实现
    boost::asio::awaitable<void> run_maintenance();

    // 停止后台任务（在析构时调用）
    void stop_internal();

    // --- 核心数据成员 ---

    // I/O 上下文
    boost::asio::io_context& ioc_;

    // **单一同步原语**: 所有对 pool_ 的访问都必须通过此 strand
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;

    // SSL 上下文（整个管理器共享一个）
    boost::asio::ssl::context ssl_ctx_;

    // DNS 解析器（整个管理器共享一个）
    boost::asio::ip::tcp::resolver resolver_;

    // 连接池本体
    using H1ConnectionDeque = std::deque<std::shared_ptr<IConnection>>;
    std::unordered_map<std::string, H1ConnectionDeque> pool_;

    // 存储可共享的 H2 连接。每个主机可以有多个 H2 连接以提高吞吐量上限。
    using H2ConnectionVector = std::vector<std::shared_ptr<IConnection>>;
    std::unordered_map<std::string, H2ConnectionVector> h2_pool_;

    // --- 后台维护任务相关 ---
    boost::asio::steady_timer maintenance_timer_;
    bool stopped_ = false;

    std::unordered_set<std::string> creation_in_progress_;
};


#endif //UNTITLED1_CONNECTION_MANAGER_HPP