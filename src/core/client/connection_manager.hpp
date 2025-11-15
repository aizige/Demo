//
// Created by Aiziboy on 2025/7/18.
//

#ifndef UNTITLED1_CONNECTION_MANAGER_HPP
#define UNTITLED1_CONNECTION_MANAGER_HPP

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/strand.hpp>
#include "utils/utils.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/beast/http/basic_parser.hpp>
#include <spdlog/spdlog.h>

#include "utils/config/AizixConfig.hpp"

// 向前声明，以减少头文件依赖
class IConnection;

/**
 * @struct PooledConnection
 * @brief 封装了从连接池获取的连接及其状态。
 *
 * 这个结构体不仅包含连接本身，还携带了一个 `is_reused` 标志，
 * 允许上层逻辑（如 HttpClient）根据连接是否为复用，来决定是否在遇到
 * 网络错误时进行自动重试（因为复用连接可能是 "stale" 的）。
 */
struct PooledConnection {
    /// @brief 指向获取到的连接对象的共享指针。
    std::shared_ptr<IConnection> connection;
    /// @brief 如果连接是从池中复用的，则为 true；如果是新创建的，则为 false。
    bool is_reused = false;
};

/**
 * @struct CreationInProgress
 * @brief 代表一个正在进行的、可被多个协程共享的连接创建任务。
 *
 * 用于解决并发请求同一目标时的“惊群效应” (thundering herd)。
 * 第一个请求者会创建一个此类型的实例，并启动连接创建；
 * 后续的请求者会发现此实例，并异步等待其完成信号，而不是重复创建。
 */
struct CreationInProgress {
    /// @brief 用于通知所有等待者“连接已创建完成（无论成功或失败）”的信号通道。
    using SignalChannel = boost::asio::experimental::channel<void(boost::system::error_code)>;
    std::shared_ptr<SignalChannel> signal;

    /// @brief 存储最终的创建结果（连接、异常），供所有等待者在收到信号后安全读取。
    std::variant<
        std::monostate, // 初始状态：仍在进行中
        std::shared_ptr<IConnection>, // 成功状态：存储创建的连接
        std::exception_ptr // 失败状态：存储捕获到的异常
    > result;

    // 原子标志位，用于“一次性认领”，防止 H1.1 连接被多个协程获取。
    std::atomic<bool> has_been_claimed_{false};

    /**
     * @brief 构造函数。
     * @param ex 用于构造内部 channel 的执行器。
     */
    explicit CreationInProgress(const boost::asio::any_io_executor& ex)
        : signal(std::make_shared<SignalChannel>(ex, 1)) {
    }
};

/**
 * @class ConnectionManager
 * @brief 一个现代化的、线程安全的 HTTP/1.1 和 HTTP/2 客户端连接池管理器。
 *
 * 该类是 HttpClient 的核心依赖，负责所有出站连接的生命周期管理、
 * 复用、保活和清理。它通过 `boost::asio::strand` 来保证所有对内部
 * 连接池状态的访问都是串行化的，从而实现了线程安全。
 *
 * @note 它的生命周期由 std::shared_ptr 管理，因为它需要在多个异步操作
 *       (如后台维护任务) 中安全地引用自身。
 */
class ConnectionManager : public std::enable_shared_from_this<ConnectionManager> {
public:
    /**
     * @brief 构造函数。
     * @param ioc 对主 `io_context` 的引用。
     * @param config 配置文件
     */
    explicit ConnectionManager(boost::asio::io_context& ioc, const AizixConfig& config);

    /**
     * @brief 析构函数。会触发后台任务的停止。
     */
    ~ConnectionManager();

    // 禁止拷贝和移动，因为 ConnectionManager 是一个唯一的、管理状态的系统级服务。
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    /**
     * @brief 异步地获取一个到指定目标的健康连接。
     *
     * 这是连接池的主要接口。它会首先尝试从池中复用一个现有连接。
     * 如果没有可用连接，它会启动一个创建新连接的任务。
     * 此方法通过内部机制处理并发创建的竞态条件。
     *
     * @param scheme "http:" 或 "https:"。以Ada的标准为准
     * @param host 主机名或IP地址。(例如:www.google.com:443)
     * @param port 端口号。
     * @return 一个协程句柄 (awaitable)，其结果是一个 `PooledConnection` 对象。
     * @throws boost::system::system_error 如果连接创建失败。
     */
    boost::asio::awaitable<PooledConnection> get_connection(
        std::string_view scheme, std::string_view host, uint16_t port);

    /**
     * @brief 将一个使用完毕的连接释放回池中，并可能唤醒一个等待者
     *
     * 如果连接仍然可用 (`is_usable()`)，它会被放回池中以供后续请求复用。
     * 如果连接已损坏或被标记为不可用，它将被安全地丢弃。
     * 此方法是线程安全的。
     *
     * @param conn 要释放的连接的共享指针。
     */
    void release_connection(const std::shared_ptr<IConnection>& conn);

    /**
     * @brief 异步地关闭所有连接池中的连接并停止后台任务。
     *
     * 这是优雅停机流程的一部分。它会并行地关闭所有连接，并等待它们完成。
     * @return 一个协程句柄，调用者可以 `co_await` 它来等待关闭完成。
     */
    boost::asio::awaitable<void> stop();

    /**
* @brief [私有] 创建一个新连接的协程。
* 包含 DNS 解析、TCP 连接、TLS 握手和 ALPN 协商的完整流程。
*/
    boost::asio::awaitable<std::shared_ptr<IConnection>> create_new_connection(const std::string& key, std::string_view scheme, std::string_view host, uint16_t port);

    uint8_t max_redirects;

private:
    /// @brief 缓存与每个主机的协议协商历史结果。
    enum class ProtocolKnowledge {
        Unknown, // 初始状态，尚未知晓
        SupportsH2, // 已知该主机支持并协商成功过 H2
        RequiresH1 // 已知该主机仅支持 H1.1（或协商降级）
    };


    /**
     * @brief [私有] 将一个新创建的连接添加到相应的池中。
     *
     * 只针对HTTP2连接，H1连接会在用完后调用release_connection()函数将连接释放回池中
     * @param key 连接池的键。
     * @param conn 要添加的连接。
     */
    void add_connection_to_pool_h2(const std::string& key, const std::shared_ptr<IConnection>& conn);


    /**
    * @brief [私有] 启动后台维护任务。
    */
    void start_maintenance();

    /**
     * @brief 连接客户端连接池维护协程。
     * 在一个循环中定期检查和维护所有连接池。
     * 它负责遍历容器中的所有连接，并执行以下维护操作：
     * 1. 移除已失效 (`is_usable() == false`) 的连接。
     * 2. 保留正在活动的连接。
     * 3. 关闭并移除空闲时间过长的连接。
     * 4. 对需要保活的空闲连接执行 PING 操作。
     *
     * @note 此协程必须在 `strand_` 的上下文中被调用，以保证线程安全。
     */
    boost::asio::awaitable<void> run_maintenance();

    /**
     * @brief [私有] 触发后台任务的停止（在析构时调用）。
     */
    void stop_internal();


    // --- 核心数据成员 ---

    // @brief I/O 上下文，对应用程序主 io_context 的引用。
    boost::asio::io_context& ioc_;

    /// @brief 核心同步原语。所有对内部状态（连接池(pool_)、`creation_in_progress_` map）
    ///        的读写操作都必须通过 `post` 到这个 strand 上来执行，以保证线程安全。
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;

    /// @brief 全局共享的 SSL/TLS 上下文，用于创建所有出站的 HTTPS 连接。
    boost::asio::ssl::context ssl_ctx_;

    /// @brief 全局共享的 DNS 解析器。
    boost::asio::ip::tcp::resolver resolver_;


    /// @brief HTTP/1.1 连接池。
    /// 使用 `std::deque` 是因为它能高效地在队头移除 (`pop_front`) 和队尾添加 (`push_back`)，
    /// 完美匹配 H1.1 连接“取用-归还”的模式。
    using HttpConnectionDeque = std::deque<std::shared_ptr<IConnection>>;
    std::unordered_map<std::string, HttpConnectionDeque> pool_;

    /// @brief HTTP/2 连接列表。
    /// 这不是一个传统的“池”，而是一个活动连接的列表。可共享的 H2 连接。每个主机可以有多个 H2 连接以提高吞吐量上限。
    /// 使用 `std::vector` 是因为它提供了最快的迭代性能，这对于遍历查找
    /// 负载最低的 H2 连接（多路复用）至关重要。
    using H2ConnectionVector = std::vector<std::shared_ptr<IConnection>>;
    std::unordered_map<std::string, H2ConnectionVector> h2_pool_;

    /// @brief 用于并发创建控制的映射表。
    /// 键是连接池键，值是代表创建任务状态的 `CreationInProgress` 对象。
    std::unordered_map<std::string, std::shared_ptr<CreationInProgress>> creation_in_progress_;


    /// 连接建立超时TCP + TLS 握手阶段的最大等待时间，
    size_t connect_timeout_ms_;

    /// 客户端是否启用Http2
    bool http2_enabled_ = false;

    /// HTTP2初始最大并发流数	控制每个连接允许的最大并发请求数
    size_t http2_max_concurrent_streams_;

    // --- 后台维护任务相关 ---
    /// 用于触发周期性维护任务的定时器。
    boost::asio::steady_timer maintenance_timer_;
    /// 连接池维护间隔
    size_t maintenance_interval_ms_;
    /// 连接闲置关闭时间
    size_t idle_timeout_for_close_ms_;
    /// 主动发送PING帧或者head请求间隔
    size_t idle_timeout_for_ping_ms_;

    /// @brief 标志位，用于在关闭时安全地停止后台循环。
    std::atomic<bool> stopped_ = false;


    /// 单个个HTTP1.1 Host的最大连接池大小
    size_t max_h1_connections_per_host_;
    /// 单个个HTTP2 Host的最大连接池大小
    size_t max_h2_connections_per_host_;

    std::unordered_map<std::string, ProtocolKnowledge> hosts_protocol_;
};


#endif //UNTITLED1_CONNECTION_MANAGER_HPP
