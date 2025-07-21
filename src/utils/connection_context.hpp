#ifndef CONNECTION_CONTEXT_HPP
#define CONNECTION_CONTEXT_HPP

#include <boost/asio.hpp>          // Boost.Asio 核心库
#include <boost/asio/steady_timer.hpp> // Asio 提供的稳定计时器，不受系统时间改变的影响
#include <chrono>                  // C++ 时间库
#include <string>                  // C++ 字符串库
#include <atomic>                  // C++ 原子操作库，用于线程安全的计数

// 引入 C++14 的时间字面量，如 30s
using namespace std::literals::chrono_literals;

/**
 * @class ConnectionContext
 * @brief 封装与单个客户端连接相关的上下文信息。
 *
 * 这个类的实例代表一个独立的客户端连接，从建立到断开的整个生命周期。
 * 它用于存储和管理该连接的状态信息，例如：
 * - 连接的唯一追踪 ID，用于日志和调试。
 * - 客户端的 IP 地址。
 * - 该连接上已处理的请求数量。
 * - 用于实现连接空闲超时的计时器。
 *
 * 通过将这些信息聚合在一个对象中，可以方便地在不同的处理阶段（如连接建立、
 * 请求处理、日志记录）之间传递和共享状态，而无需通过大量的函数参数。
 */
class ConnectionContext {
public:
    /**
     * @brief 构造一个新的 ConnectionContext 对象。
     * @param executor 一个有效的 Boost.Asio 执行器 (executor)。
     *                 所有与此上下文关联的异步操作（如计时器）都将在此执行器上调度。
     *                 通常这个执行器来自 io_context 或一个 strand。
     */
    ConnectionContext(boost::asio::any_io_executor executor)
    : executor_(executor),                   // 保存执行器
      timeout_timer_(executor),                 // 使用该执行器构造一个计时器
      trace_id_(generate_trace_id()),           // 生成一个唯一的追踪 ID
      remote_ip_("unknown"),                  // 远程 IP 地址默认为 "unknown"
      request_count_(0)                       // 初始化请求计数为 0
    {}

    /**
     * @brief 重置空闲超时计时器。
     *
     * 每当连接上有活动（如收到数据或发送数据）时，都应调用此函数，
     * 以防止连接因空闲而被服务器主动关闭。
     *
     * @param timeout 超时时长，默认为 30 秒。
     */
    void reset_timeout(std::chrono::seconds timeout = 30s) {
        // 设置计时器在指定的`timeout`时长后到期。
        timeout_timer_.expires_after(timeout);
    }

    /**
     * @brief 获取客户端的 IP 地址。
     * @return 客户端的 IP 地址字符串。
     */
    std::string ip() const { return remote_ip_; }

    /**
     * @brief 获取内部的超时计时器对象的引用。
     *
     * 允许外部代码直接操作这个计时器，例如在其上调用 `async_wait`。
     * @return 对 boost::asio::steady_timer 的可变引用。
     */
    boost::asio::steady_timer& timer() { return timeout_timer_; }

    /**
     * @brief 获取此连接的唯一追踪 ID。
     * @return 追踪 ID 字符串，例如 "conn-1627384920"。
     */
    std::string trace_id() const { return trace_id_; }

    /**
     * @brief 线程安全地增加已处理的请求数量。
     */
    void increment_request() {
        // 使用原子操作 ++，确保在多线程环境下计数的正确性。
        ++request_count_;
    }

    /**
     * @brief 获取此连接上已处理的请求总数。
     * @return 请求数量。
     */
    size_t request_count() const { return request_count_; }

    /**
     * @brief 设置客户端的 IP 地址。
     * @param ip 从套接字中获取到的远程端点 IP 地址。
     */
    void set_remote_ip(const std::string& ip) { remote_ip_ = ip; }

private:
    /**
     * @brief 生成一个基于当前时间的、近似唯一的追踪 ID。
     *
     * 使用稳态时钟 (steady_clock) 从某个固定时间点开始到现在的纳秒数作为 ID 的一部分，
     * 这在单次程序运行中足以保证唯一性，非常适合用于日志追踪。
     * @return 生成的追踪 ID 字符串。
     */
    std::string generate_trace_id() {
        // 获取稳态时钟的纪元时间计数
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        return "conn-" + std::to_string(now);
    }

    // Boost.Asio 执行器，是所有异步操作的调度核心。
    boost::asio::any_io_executor executor_;

    // 用于实现空闲超时的计时器。
    boost::asio::steady_timer timeout_timer_;

    // 此连接的唯一标识符，主要用于日志。
    std::string trace_id_;

    // 在此连接上处理的请求计数。使用 std::atomic 保证多线程安全。
    std::atomic<size_t> request_count_;

    // 客户端的 IP 地址。
    std::string remote_ip_;
};

#endif // CONNECTION_CONTEXT_HPP