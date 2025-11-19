//
// Created by Aiziboy on 2025/11/16.
//

#ifndef AIZIX_SERVER_HPP
#define AIZIX_SERVER_HPP

#include <fstream>
#include <set>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>     // Asio 的 SSL/TLS 功能
#include <boost/asio/experimental/parallel_group.hpp> // 引入 parallel_group
#include <boost/asio/experimental/awaitable_operators.hpp>
#include "http/http_common_types.hpp"
#include "utils/config/AizixConfig.hpp"
#include "http/router.hpp"
#include "h2_session.hpp"

/// @class Server
/// @brief 主服务器类，负责网络监听和连接分发。
/// 服务器的“大脑”，负责监听、接受连接、进行协议分发。
/// 这个类是整个应用程序的入口点和事件循环的驱动者。它的职责包括：
/// 监听指定的 TCP 端口，等待客户端连接。
/// 接受新的 TCP 连接。
/// 如果启用了 SSL/TLS，则执行 TLS 握手。
/// 通过 ALPN (Application-Layer Protocol Negotiation) 判断客户端期望使用的协议。
/// 如果是 HTTP/2 (h2)，则将连接交给 Http2Session 处理。
/// 如果是 HTTP/1.1 或未知协议，则回退到基于 Boost.Beast 的 HTTPS 或 HTTP 处理器。
/// 管理服务器范围内的资源，如路由器 (Router)*/
class Server {
public:
    /**
    @brief 构造函数 (带 IP 和端口)。
    @param ioc 对主 io_context 的引用。
    @param work_executor 工作线程的executor
    @param config 配置文件
    */
    Server(boost::asio::io_context& ioc, boost::asio::any_io_executor work_executor, const AizixConfig& config);


    /**
     *  @brief 获取对内部路由器的引用。
     *  允许外部代码（如 `main.cpp`）向服务器注册路由。
     *  @return Router&
     */
    Router& router();

    /**
    @brief 配置服务器以启用 TLS (HTTPS, H2)。
    @param cert_file PEM 格式的证书链文件路径。
    @param key_file PEM 格式的私钥文件路径。
    @note [逻辑问题]：此函数在加载失败时不会抛出异常，而是静默地将服务器
    降级为 HTTP 模式，这在生产环境中可能导致严重的安全风险。

    */
    void set_tls(const std::string& cert_file, const std::string& key_file);

    /**
    * @brief 启动服务器的监听循环。
    *这是一个非阻塞操作，它会启动一个后台协程来处理连接接受。
    */
    void run();

    /**
     *
    * @brief [修复后] 异步地、优雅地关闭服务器上的所有活跃会话。
    * 这个协程是服务器优雅停机流程的关键部分。它会执行以下步骤：
    * 立即关闭 acceptor，停止接受任何新的客户端连接。
    * 安全地从会话管理列表中收集所有当前活跃的会话。
    * 并发地为每个活跃会话启动一个独立的 graceful_shutdown 协程。
    * 异步地等待所有这些关闭任务都完成后，此协程才会返回。
    * @note 调用者应该 co_await 这个函数，以确保在继续执行后续的清理
    * 操作（如停止 `io_context`）之前，所有网络会话都已完全关闭。
    * @note 目前此方法只处理 Http2Session。如果未来添加了其他需要
    * 优雅关闭的会话类型 (如 H2cSession)，应考虑引入一个
    * 通用的 IStoppable 接口来统一管理和关闭所有会话，
    * 以避免此函数中的代码重复。
    * @return 一个协程句柄，表示整个关闭流程。
    */
    boost::asio::awaitable<void> stop();

private
:
    /**
    * @brief 主监听协程，一个无限循环，负责接受新连接。
    */
    boost::asio::awaitable<void> listener();

    /**
     * @brief [私有] 处理纯文本 HTTP/1.1 连接的协程。
     *        包含 Keep-Alive 逻辑和超时处理。
     */
    boost::asio::awaitable<void> handle_plain(tcp::socket sock) const;

    /**
     *  @brief 处理加密的 HTTPS (HTTP/1.1) 连接的协程。
     *        逻辑与 handle_plain 基本相同，只是 I/O 操作对象变为了 tls_stream。
     */

    boost::asio::awaitable<void> handle_https(std::shared_ptr<boost::asio::ssl::stream<tcp::socket>> tls_stream) const;

    /**
     * @brief [私有] 配置并启动 TCP acceptor。
     */
    void setup_acceptor(uint16_t port, const std::string& ip);


    /**
    * @brief 回调函数，用于在 TLS 握手期间选择一个应用层协议。
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


    /// @brief 对应用程序主 io_context 的引用。
    boost::asio::io_context& io_context_;
    /// @brief 工作线程的 executor
    boost::asio::any_io_executor work_executor_;
    /// @brief 用于创建和配置所有 TLS 连接的 SSL 上下文。
    boost::asio::ssl::context ssl_context_;
    /// @brief 负责在指定端口上监听和接受传入的 TCP 连接。
    tcp::acceptor acceptor_;
    /// @brief 标志位，指示服务器当前是否应在 SSL/TLS 模式下运行。
    bool use_ssl_;
    /// @brief 存储所有已注册路由的路由器实例。
    Router router_;

    /// @brief 用于保护会话列表 (h2_sessions_) 的互斥锁，确保多线程访问安全。
    std::mutex session_mutex_;
    /// @brief 存储所有活跃的 Http2Session 的弱指针。
    /// 使用 weak_ptr 可以避免循环引用，并允许在不影响 session 自身生命周期的情况下对其进行跟踪。
    std::set<std::weak_ptr<Http2Session>, std::owner_less<std::weak_ptr<Http2Session>>> h2_sessions_;

    // 设置最大允许的 HTTP 请求体大小 （bytes）
    size_t max_request_body_size_bytes_; // 1 MB
    bool http2_enabled_;
    std::vector<std::string> tls_versions_;
    std::chrono::milliseconds initial_timeout_ms_;
    std::chrono::milliseconds keep_alive_timeout;
};


#endif //AIZIX_SERVER_HPP
