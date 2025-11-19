#ifndef HTTP2SESSION_HPP
#define HTTP2SESSION_HPP
#include <boost/asio.hpp>          // Boost.Asio 核心库，提供异步 I/O、协程、网络等功能
#include <boost/beast/core.hpp>    // Boost.Beast 核心，我们主要使用其 HTTP 组件（如 HTTP 字段定义）
#include <nghttp2/nghttp2.h>       // C 语言实现的 HTTP/2 协议核心库，负责协议帧的解析和生成
#include <unordered_map>           // 用于高效地存储以 stream_id 为键的流上下文
#include <string>
#include <memory>                  // 用于 std::shared_ptr 和 std::enable_shared_from_this，管理 session 的生命周期
#include <boost/asio/ssl/stream.hpp> // Asio 提供的 TLS/SSL 流，用于实现 HTTPS (h2)
#include "http/router.hpp"         // 引入应用层的路由定义
#include <boost/asio/experimental/channel.hpp> // 引入 channel，一个协程安全的消息队列，用于在协程间传递数据

/// 为 Asio channel 定义一个类型别名，方便使用。
/// 这个 channel 用于从 session_loop 的回调中向 dispatcher_loop 发送已准备好的 stream_id。
/// 它可以传递一个 error_code 和一个 int32_t (stream_id)。
using StreamIdChannel = boost::asio::experimental::channel<void(boost::system::error_code, int32_t)>;
/**
@class Http2Session
@brief 管理一个完整的服务器端 HTTP/2 会话。
这个类实现了“三协程 Actor”模型，将读、写和请求分发逻辑分离到
三个并行的、常驻的协程 (session_loop, writer_loop, dispatcher_loop) 中，
以实现最高的并发性能和最清晰的逻辑分离。
session_loop: 负责从 socket 读取数据，并将其喂给 nghttp2 引擎进行解析。
writer_loop: 负责检查 nghttp2 引擎是否有待发送的数据，并将其写入 socket。
dispatcher_loop: 负责从 channel 中接收已完成的请求流 ID，并派发给具体的处理逻辑。
所有对共享状态（如 nghttp2_session、streams_ 等）的访问都通过 strand_ 来串行化，
从而从根本上避免了数据竞争，使得代码逻辑更简单，无需手动加锁。
*/
class Http2Session : public std::enable_shared_from_this<Http2Session> {
public:
    // 定义 SSL/TLS 流的类型别名，方便使用。HTTP/2 标准强制要求使用 TLS。
    using SSLStream = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

    // 定义指向 SSLStream 的共享指针的类型别名，因为 session 的生命周期是与连接绑定的，使用 shared_ptr 可以方便地管理。
    using StreamPtr = std::shared_ptr<SSLStream>;
    /**
    @brief 构造函数。
    @param stream 已经完成 TLS 握手的 SSL/TLS 流的共享指针。
    @param work_executor work executor
    @param router 对服务器主路由器的引用，用于分发请求。
    @param max_request_body_size_bytes 最大允许的 HTTP 请求体大小 （bytes）
    @param keep_alive_timeout keep alive超时时间.
    */
    Http2Session(StreamPtr stream,boost::asio::any_io_executor work_executor, Router& router, size_t max_request_body_size_bytes, std::chrono::milliseconds keep_alive_timeout);
    /**
    @brief 析构函数。
    负责释放 nghttp2 会话资源。
    */
    ~Http2Session();
    // 禁止拷贝构造和拷贝赋值，因为每个 session 都与一个唯一的网络连接相关联，拷贝没有意义。
    Http2Session(const Http2Session&) = delete;
    Http2Session& operator=(const Http2Session&) = delete;

    /**
    *@brief 静态工厂方法，用于创建 Http2Session 的共享指针。

    *这是创建 std::enable_shared_from_this 派生类对象的标准做法。
    *@param stream 移动过来的 SSL 流指针。
   * @param work_executor work executor
    *@param r 对路由器的引用。
    *@param max_request_body_size_bytes 最大允许的 HTTP 请求体大小 （bytes）
   * @param keep_alive_timeout keep alive超时时间.
    *@return Http2Session 的 std::shared_ptr 实例。
    */
    static std::shared_ptr<Http2Session> create(StreamPtr stream, boost::asio::any_io_executor work_executor,Router& r, size_t max_request_body_size_bytes, std::chrono::milliseconds keep_alive_timeout) {
        return std::make_shared<Http2Session>(std::move(stream),  std::move(work_executor),r, max_request_body_size_bytes, keep_alive_timeout);
    }

    /**
 *@brief 启动会话处理。
 *这是 session 的主入口点，它会并行启动三个核心协程。
 *@return 一个可等待的 Asio awaitable。
*/
    boost::asio::awaitable<void> start();

    /**
    * @brief 优雅地关闭 HTTP/2 会话。
    *        它会向客户端发送一个 GOAWAY 帧，告知客户端停止发送新的请求。
    * @param error_code 关闭原因的错误码，默认为 NGHTTP2_NO_ERROR（正常关闭）。
    * @return 一个可等待的 Asio awaitable。
    */
    boost::asio::awaitable<void> graceful_shutdown(uint32_t error_code = NGHTTP2_NO_ERROR);
    /**
    @brief 获取客户端的远程网络端点（IP 和端口）。
    @return tcp::endpoint 对象。
    */
    tcp::endpoint remote_endpoint() const;

private:
    /**
 * @brief 初始化 nghttp2 会话，设置所有必需的回调函数。
 */
    void init_session();
    /**
 * @brief 核心协程之一：会话循环（读取循环）。
 *        - 负责从 TCP/TLS 套接字异步读取数据。
 *        - 将读取到的数据送入 nghttp2 引擎进行解析。
 *        - 管理连接的空闲超时。
 */
    boost::asio::awaitable<void> session_loop();

    /**
     * @brief 核心协程之一：分发循环。
     *        - 从 `dispatch_channel_` 异步等待已完成的请求流 ID。
     *        - 为每个请求 `co_spawn` 一个新的协程 (`dispatch`) 进行处理，实现请求的并行处理。
     */
    boost::asio::awaitable<void> dispatcher_loop();

    /**
     * @brief 执行一次写操作。
     *        - 从 nghttp2 引擎获取待发送的数据。
     *        - 将数据异步写入 TCP/TLS 套接字。
     */
    boost::asio::awaitable<void> do_write() ;

    /**
     * @brief 处理单个 HTTP/2 请求流。
     *        - 从 `streams_` 中获取请求头和请求体。
     *        - 构造一个 HttpRequest 对象。
     *        - 通过 router 找到对应的处理函数并执行。
     *        - 将处理函数生成的 HttpResponse 构造成 HTTP/2 响应帧并提交给 nghttp2 引擎。
     * @param stream_id 要处理的流的 ID。
     */
    boost::asio::awaitable<void> dispatch(int32_t stream_id);;

    /**
    * @brief 核心协程之一：写入循环。
    *        - 永久阻塞，等待 `write_trigger_` 定时器被触发。
    *        - 被唤醒后，循环调用 `do_write()` 将 nghttp2 引擎中所有待发送的数据全部写入套接字，直到队列清空。
    */
    boost::asio::awaitable<void> writer_loop();

    /**
     * @brief 调度一次写操作。
     *        这是一个非阻塞函数，通过取消 `write_trigger_` 定时器来立即唤醒 `writer_loop`。
     *        必须在 strand 上下文中调用。
     */
    void schedule_write();

    /// 空闲超时定时器的主循环协程
    boost::asio::awaitable<void> idle_timer_loop();




    // --- nghttp2 C-style 回调函数 ---
    // 这些是静态函数，因为 C 库不知道如何调用 C++ 成员函数。
    // `void* user_data` 参数被用来传递 `this` 指针，从而在回调内部可以访问 Http2Session 实例。
    static int on_header_callback(nghttp2_session*, const nghttp2_frame*, const uint8_t*, size_t, const uint8_t*, size_t, uint8_t, void*);
    static int on_data_chunk_recv_callback(nghttp2_session*, uint8_t, int32_t, const uint8_t*, size_t, void*);
    static int on_stream_close_callback(nghttp2_session*, int32_t, uint32_t, void*);
    static int on_frame_recv_callback(nghttp2_session*, const nghttp2_frame*, void*);

    /**
     * @struct StreamContext
     * @brief 存储一个正在进行中的 HTTP/2 流（请求）的上下文信息。
     */
    struct StreamContext {
        std::vector<std::pair<std::string, std::string>> headers; // 存储解析出的原始请求头
        std::string body; // 存储接收到的请求体数据
        size_t body_size = 0; // 跟踪 body 大小
        bool is_rejected = false; // 标记此流是否已因超限被拒绝
    };


    /**
     * @struct ProviderPack
     * @brief 用于向 nghttp2 提供响应体数据的封装结构。
     *        nghttp2 会通过回调函数从这个结构中拉取数据进行发送。
     */
    struct ProviderPack {
        std::shared_ptr<std::string> content; // 指向响应体字符串的共享指针，以管理其生命周期，防止在异步发送完成前被析构。
        size_t offset = 0; // 用于跟踪已发送数据量的偏移量，以实现高效的发送。
    };

    // --- 成员变量 ---

    StreamPtr stream_; // 指向 SSL/TLS 流的共享指针，代表了与客户端的连接。
    boost::asio::any_io_executor work_executor_; // 注入进来的thread_pool executor
    Router& router_; // 对服务器主路由器的引用。
    boost::asio::strand<boost::asio::any_io_executor> strand_; // strand 保证所有对 session 共享状态的操作都在同一个逻辑线程上，避免数据竞争。
    nghttp2_session* session_; // 指向 nghttp2 会话实例的裸指针，由 nghttp2 库管理。
    std::unordered_map<int32_t, StreamContext> streams_; // 存储活跃的请求流上下文，键是 stream_id。
    std::unordered_map<int32_t, std::shared_ptr<ProviderPack>> provider_pack_; // 存储响应数据提供者，确保响应体在发送完成前一直有效。

    boost::asio::steady_timer idle_timer_; // 用于实现连接空闲超时的计时器。
    size_t active_streams_ = 0; // 当前活跃的流数量。

    size_t max_request_body_size_bytes_;
    std::chrono::milliseconds keep_alive_ms_;

    // 用于唤醒 writer_loop 的机制
    boost::asio::steady_timer write_trigger_; // 一个定时器，通过取消它来立即触发一次写操作，比使用 channel 更轻量。
    bool write_in_progress_ = false; // 一个标志位，防止 writer_loop 的重入。

    // 用于在 reader 和 dispatcher 之间传递 stream_id 的协程安全通道。
    StreamIdChannel dispatch_channel_;

};
#endif // HTTP2SESSION_HPP
