//
// Created by Aiziboy on 2025/7/18.
//

#ifndef AIZIX_CONNECTION_MANAGER_HPP
#define AIZIX_CONNECTION_MANAGER_HPP

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
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/beast/http/basic_parser.hpp>
#include <spdlog/spdlog.h>

#include <aizix/utils/config/AizixConfig.hpp>


// 向前声明，以减少头文件依赖
class IConnection;

namespace aizix {
    class App;
}

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


/// @brief 主机协议能力的枚举状态。
/// 用于指导 get_connection 在面对空闲连接池时的并发创建策略。
enum class ProtocolKnowledge {
    /// @brief [初始态] 未知。
    /// 策略：通常进行“乐观尝试”。如果是 HTTPS，假设可能支持 H2 并进入等待队列；
    ///       如果是 HTTP，直接视为 H1 进行并发创建。
    Unknown,

    /// @brief [复用态] 已知该主机支持 HTTP/2。
    /// 策略：<b>开启惊群保护。</b>
    ///       当多个协程并发请求该主机时，只允许第一个协程发起 TCP/TLS 握手，
    ///       其余协程进入 `creation_in_progress_` 队列等待。
    ///       一旦连接建立，所有等待者将共享这同一个 H2 连接（多路复用）。
    SupportsH2,

    /// @brief [独占态] 已知该主机仅支持 HTTP/1.1 (或被配置强制降级)。
    /// 策略：<b>绕过等待队列 (Bypass Queue)。</b>
    ///       H1 连接无法复用，让 N 个请求等待 1 个连接建立会导致严重的队头阻塞 (Head-of-Line Blocking)。
    ///       因此，所有请求都将立即并发发起独立的 TCP 握手，以最大化吞吐量。
    RequiresH1
};

/// @brief 协议缓存条目，包含状态和最后更新时间（用于 TTL 过期淘汰）。
struct ProtocolInfo {
    ProtocolKnowledge knowledge = ProtocolKnowledge::Unknown;
    /// @brief 上次握手成功的时间点。
    /// 如果超过 `protocol_cache_ttl_`，该记录将被清除，以便重新探测服务器配置变化。
    std::chrono::steady_clock::time_point last_updated;
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
        std::monostate,               // 初始状态：仍在进行中
        std::shared_ptr<IConnection>, // 成功状态：存储创建的连接
        std::exception_ptr            // 失败状态：存储捕获到的异常
    > result;

    // 用于“一次性认领”，防止 H1.1 连接被多个协程获取。
    bool has_been_claimed_ = false; // 不需要 atomic，因为是在单线程内访问

    /**
     * @brief 构造函数。
     * @param ex 用于构造内部 channel 的执行器。
     */
    explicit CreationInProgress(const boost::asio::any_io_executor& ex)
        : signal(std::make_shared<SignalChannel>(ex, 1)) {
    }
};


/**
 * @class HttpClientPool
 * @brief [高性能核心] 线程本地 (Thread-Local) HTTP 客户端连接池。
 *
 * - 架构变更说明 (v2.0 Per-Thread Architecture)
 * 为了最大化多核性能并消除锁竞争，本类已从“全局单例”重构为“线程分片实例”。
 *
 * <h3>核心特性：</h3>
 * 1. <b>Shared-Nothing 设计：</b> 每个 IO 线程拥有自己独立的 `HttpClientPool` 实例。
 * 2. <b>完全无锁 (Lock-Free)：</b>
 *    - 移除了所有的 `mutex` 和 `boost::asio::strand`。
 *    - 所有的连接获取 (`get_connection`)、归还 (`release_connection`)、创建和销毁操作
 *      都严格限制在当前 IO 线程内部执行。
 *    - 依赖单线程的顺序执行来保证线程安全，性能极高 (L1 Cache 友好)。
 * 3. <b>本地化资源：</b>
 *    - DNS 解析器 (`resolver_`) 和 定时器 (`timer`) 均绑定到当前线程的 `io_context`。
 *    - 连接对象 (`socket`, `ssl_stream`) 直接绑定到当前线程，杜绝跨线程 I/O。
 *
 * <h3>使用约束：</h3>
 * - <b>严禁跨线程调用：</b> 你不能在一个线程中获取 Pool 指针，然后传给另一个线程使用。
 * - <b>必须在 IO 线程运行：</b> 只能在 `App` 启动的 Worker IO 线程中访问此类的实例。
 */
class HttpClientPool : public std::enable_shared_from_this<HttpClientPool> {
public:
    /**
     * @brief 构造函数。
     * @param ctx 当前 Worker 线程绑定的 IO Context
     *            整个 Pool 的生命周期将绑定到这个 Context 上。
     * @param config 全局配置对象。
     */
    explicit HttpClientPool(boost::asio::io_context& ctx, const AizixConfig& config);

    ~HttpClientPool();


    /**
     * @brief 异步的获取一个可用连接
     *
     * 流程：
     * 1. <b>本地查找：</b> 直接在 `unordered_map` 中查找可用连接 (O(1))。
     * 2. <b>协议决策：</b> 根据历史记录 (`hosts_protocol_`) 判断是否需要等待复用 (H2) 或直接创建 (H1)。
     * 3. <b>本地创建：</b> 如果需要，直接在当前线程发起异步连接建立流程。
     *
     * @note 此函数不会发生任何线程切换 (Context Switch)。
     * @param scheme "http:" 或 "https:"。以Ada的标准为准
     * @param host 主机名或IP地址。(例如:www.google.com:443)
     * @param port 端口号。
     * @return 一个协程句柄 (awaitable)，其结果是一个 `PooledConnection` 对象。
     * @throws boost::system::system_error 如果连接创建失败。
     */
    boost::asio::awaitable<PooledConnection> get_connection(
        std::string scheme, std::string host, uint16_t port); // 传值更方便 lambda 捕获

    /**
     * @brief 归还连接。
     *
     * 将连接放回当前线程的本地池中。
     * 如果是 H2 连接，它其实一直没离开过池（只是引用计数变化）。
     * 如果是 H1 连接，放回 deque 队尾。
     */
    void release_connection(const std::shared_ptr<IConnection>& conn);

    /**
     * @brief [本地] 停止当前线程的连接池。
     *
     * 1. 取消本地维护定时器。
     * 2. 关闭当前线程持有的所有连接。
     * 3. 等待所有关闭操作完成。
     */
    boost::asio::awaitable<void> stop();


    uint8_t max_redirects;

private:
    /**
     * @brief 创建一个新连接
     * 包含 DNS 解析、TCP 连接、TLS 握手和 ALPN 协商的完整流程。
     */
    boost::asio::awaitable<std::shared_ptr<IConnection>> create_new_connection(const std::string& key, std::string_view scheme, std::string_view host, uint16_t port);


    /**
     * @brief 将一个新创建的连接添加到相应的池中。
     *
     * 只针对HTTP2连接，H1连接会在用完后调用release_connection()函数将连接释放回池中
     * @param key 连接池的键。
     * @param conn 要添加的连接。
     */
    void add_connection_to_pool_h2(const std::string& key, const std::shared_ptr<IConnection>& conn);


    /**
    * @brief 启动后台维护任务。
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
     */
    boost::asio::awaitable<void> run_maintenance();


    // --- 核心数据成员 ---

    /// 当前线程的 IO Context，用于 Timer, Resolver
    boost::asio::io_context& ioc_;

    /// @brief 全局共享的 SSL/TLS 上下文，用于创建所有出站的 HTTPS 连接。
    boost::asio::ssl::context ssl_ctx_;

    /// @brief 全局共享的 DNS 解析器。
    ///
    boost::asio::ip::tcp::resolver resolver_;


    // --- 本地连接池 ---

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


    /**
     * @brief [智能调度核心] 主机协议协商历史缓存表。
     *
     *
     * 这是一个 Thread-Local 的哈希表，记录了当前 IO 线程与各个 Host 的历史“交情”。
     *
     * 它的存在是为了解决高性能 HTTP 客户端的一个经典难题：
     *
     * <b>"我应该等待别人建好连接蹭车 (H2)，还是自己赶紧建一个 (H1)？"</b>
     *
     * <h3>工作机制：</h3>
     * 1. <b>学习 (Learn):</b> 每次 `create_new_connection` 完成握手 (ALPN) 后，
     *    会将结果 (H2/H1) 写入此表。
     * 2. <b>决策 (Decide):</b> `get_connection` 在查不到可用连接时，会查阅此表：
     *    - 若为 `SupportsH2`: 启动 `creation_in_progress` 机制，1 人创建，万人等待。
     *    - 若为 `RequiresH1`: 直接发起创建，100 人请求，100 个并发连接。
     * 3. <b>遗忘 (Expire):</b> 后台维护协程会定期清理过期的条目，允许客户端适应服务器的升级或降级。
     *
     * @note 仅在当前 IO 线程内访问，无锁且高效。
     */
    std::unordered_map<std::string, ProtocolInfo> hosts_protocol_;


    // --- 配置参数 ---
    /// 连接建立超时TCP + TLS 握手阶段的最大等待时间，
    std::chrono::milliseconds connect_timeout_;

    /// 客户端是否启用Http2
    bool http2_enabled_ = false;

    /// HTTP2初始最大并发流数   控制每个连接允许的最大并发请求数
    size_t http2_max_concurrent_streams_;

    // --- 后台维护任务相关 ---
    /// 用于触发周期性维护任务的定时器。
    boost::asio::steady_timer maintenance_timer_;
    /// 连接池维护间隔
    std::chrono::milliseconds maintenance_interval_;
    /// 连接闲置关闭时间
    std::chrono::milliseconds idle_timeout_for_close_;
    /// 主动发送PING帧或者head请求间隔
    std::chrono::milliseconds idle_timeout_for_ping_;

    /// @brief 标志位，用于在关闭时安全地停止后台循环。
    bool stopped_ = false;


    /// 单个个HTTP1.1 Host的最大连接池大小
    size_t max_h1_connections_per_host_;
    /// 单个个HTTP2 Host的最大连接池大小
    size_t max_h2_connections_per_host_;


    /// @brief 主机的协议缓存有效时间 (Time-To-Live)（毫秒）
    /// 缓存的协议信息（如主机支持H2或H1.1）超过此时间后将被视为过期，
    /// 以便客户端能够适应服务器配置的变化。
    std::chrono::milliseconds protocol_cache_ttl_;
};


#endif //AIZIX_CONNECTION_MANAGER_HPP
