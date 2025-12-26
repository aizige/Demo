//
// Created by Aiziboy on 2025/11/16.
//

#ifndef AIZIX_SERVER_HPP
#define AIZIX_SERVER_HPP

#include <fstream>
#include <set>
#include <unordered_set>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>     // Asio 的 SSL/TLS 功能
#include <boost/asio/experimental/parallel_group.hpp> // 引入 parallel_group
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <aizix/http/http_common_types.hpp>
#include <aizix/utils/config/AizixConfig.hpp>
#include <aizix/http/router.hpp>


class Http2Session;
class HttpsSession;
class HttpSession;

namespace aizix {
    class App;
}

/// @class Server
/// @brief 高性能 HTTP/HTTPS/HTTP2 服务器核心类。
///
/// <h2>架构设计核心：</h2>
/// 1. <b>One Loop Per Thread (Reactor):</b> 每个 IO 线程拥有独立的 `io_context` 和事件循环。
/// 2. <b>SO_REUSEPORT:</b> 利用内核级负载均衡。每个线程拥有独立的 `acceptor` 监听同一端口，
///    消除 `accept` 锁竞争，实现连接的完美分片（Sharding）。
/// 3. <b>Thread-Local Context:</b> 所有 Session 管理均在线程本地进行（ThreadContext），
///    <b>完全无锁</b>，最大化 CPU 缓存亲和性（Cache Locality）。
/// 4. <b>Safety Shutdown:</b> 使用引用计数栅栏（Active Counter Barrier）防止停机时的 Use-After-Free 崩溃。
/// 管理服务器范围内的资源，如路由器 (Router)
class Server {
public:
    /**
     * @brief 构造函数
     * - 初始化 SSL 上下文，并为每个 IO 线程预创建 Acceptor。
     * @param app 传入 App 引用以获取 io_context  池
     * @param config 配置文件
     */
    Server(aizix::App& app, const AizixConfig& config);


    /**
     *  @brief 获取对内部路由器的引用。
     *  允许外部代码（如 `main.cpp`）向服务器注册路由。
     *  @return Router&
     */
    Router& router();

    /**
     * @brief 配置服务器以启用 TLS (HTTPS, H2)。
     * @param cert_file PEM 格式的证书链文件路径。
     * @param key_file PEM 格式的私钥文件路径。
     * @note [逻辑问题]：此函数在加载失败时不会抛出异常，而是静默地将服务器
     * 降级为 HTTP 模式，这在生产环境中可能导致严重的安全风险。
     */
    void set_tls(const std::string& cert_file, const std::string& key_file);

    /**
     * @brief 启动服务器的监听循环。
     * 这是一个非阻塞操作，它会启动一个后台协程来处理连接接受。
     */
    void run();

    /**
     * @brief 分布式优雅停机。
     *
     * 流程：
     * 1. 关闭所有 Acceptor
     * 2. 向所有 IO 线程广播停机任务
     * 3. 每个 IO 线程并行执行：关闭 H1/Https -> 优雅关闭 H2 (GOAWAY) -> 等待协程栈彻底销毁
     * 4. 主线程等待所有 IO 线程任务完成
     *
     * @return awaitable<void>
     */
    boost::asio::awaitable<void> stop();

private:
    // ========================================================================
    //  Thread-Local Data (无锁设计的核心)
    // ========================================================================
    // 每个 IO 线程拥有一个独立的 ThreadContext 实例。
    // 因为 listener 和 handle_connection 都绑定在特定的 IO 线程上，
    // 所以访问此结构体永远不需要互斥锁 (Mutex)。
    struct ThreadContext {
        // 使用 unordered_set 实现 O(1) 的快速查找和移除
        std::unordered_set<std::shared_ptr<HttpSession>> h1_sessions;
        std::unordered_set<std::shared_ptr<HttpsSession>> https_sessions;

        // H2 使用 weak_ptr 防止循环引用 (Session 持有 Connection, Connection 持有 Session)
        // 使用 std::set 是因为 weak_ptr 需要 owner_less 进行排序，unordered_set 哈希 weak_ptr 较麻烦且容易出错
        std::set<std::weak_ptr<Http2Session>, std::owner_less<std::weak_ptr<Http2Session>>> h2_sessions;

        // 活跃协程计数器 (生命周期保护伞)
        // 作用：追踪当前线程上所有未销毁的协程（包括正在握手的连接、正在执行关闭逻辑的后台任务）。
        // 目的：在 Server::stop 返回前，必须等待此计数归零。
        //       防止 io_context 析构时，还有残留的协程栈帧试图访问已释放的内存 (导致 SEGV)。
        size_t active_coroutine_count = 0;
    };


    /**
     * @brief 监听协程 (Per Thread)。
     * 运行在特定的 IO 线程上，利用 SO_REUSEPORT 负责接受内核分发的新连接。
     */
    boost::asio::awaitable<void> listener(boost::asio::ip::tcp::acceptor& acceptor, ThreadContext* ctx);

    /**
     * @brief 初始化 Acceptors。
     * 为 App 中的每个 IO 线程创建一个 Acceptor 并绑定端口。
     */
    void setup_acceptor(uint16_t port, const std::string& ip);


    /**
     * @brief ALPN 回调函数：用于在 TLS 握手期间协商选择一个应用层协议 H2 或 HTTP/1.1
     * @param ssl OpenSSL 的 SSL 对象指针。
     * @param out 用于存放服务器选择的协议的指针。
     * @param out_len 用于存放服务器选择的协议的长度。
     * @param in 客户端提供的协议列表。
     * @param in_len 客户端协议列表的总长度。
     * @param arg 用户自定义参数（在此未使用）。
     * @return `SSL_TLSEXT_ERR_OK` 表示成功选择了一个协议，
     *         `SSL_TLSEXT_ERR_NOACK` 表示没有找到共同支持的协议。
     */
    static int alpn_select_callback(SSL* ssl, const unsigned char** out, unsigned char* out_len, const unsigned char* in, unsigned int in_len, void* arg);


    /**
     * @brief 连接处理主逻辑。
     * 包含：引用计数增加 -> TLS 握手 -> ALPN -> Session 创建 -> 引用计数减少。
     */
    boost::asio::awaitable<void> handle_connection(boost::asio::ip::tcp::socket socket, ThreadContext* ctx);

    /// 引用 App 而不是 io_context
    aizix::App& app_;

    /// 工作线程的 executor
    boost::asio::any_io_executor work_executor_;
    /// 用于创建和配置所有 TLS 连接的 SSL 上下文。
    boost::asio::ssl::context ssl_context_;

    ///  Acceptor列表，负责在指定端口上监听和接受传入的 TCP 连接。 列表长度等于 IO 线程数
    std::vector<boost::asio::ip::tcp::acceptor> acceptors_;

    /// 线程上下文，索引与 acceptors_ 及 IO 线程 ID 一一对应
    /// 使用 unique_ptr 保证地址稳定性，防止 vector 扩容导致指针失效
    std::vector<std::unique_ptr<ThreadContext>> thread_contexts_;

    /// 标志位，指示服务器当前是否应在 SSL/TLS 模式下运行。
    bool use_ssl_;
    /// 存储所有已注册路由的路由器实例。
    Router router_;

    /// listener循环停止原子标志位 为true则不要再处理新连接了
    /// 通知所有 listener 停止 accept
    std::atomic<bool> is_stopping_{false};


    // 设置最大允许的 HTTP 请求体大小 （bytes）
    size_t max_request_body_size_bytes_;
    bool http2_enabled_;
    std::vector<std::string> tls_versions_;
    std::chrono::milliseconds initial_timeout_ms_;
    std::chrono::milliseconds keep_alive_timeout;
};


#endif //AIZIX_SERVER_HPP
