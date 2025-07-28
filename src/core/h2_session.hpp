#ifndef HTTP2SESSION_HPP
#define HTTP2SESSION_HPP

#include <boost/asio.hpp>          // Boost.Asio 核心库
#include <boost/beast/core.hpp>    // Boost.Beast 核心，我们主要使用其 HTTP 组件
#include <nghttp2/nghttp2.h>       // C 语言实现的 HTTP/2 协议核心库
#include <unordered_map>           // 用于存储 stream 上下文等
#include <string>
#include <memory>                  // 用于 std::shared_ptr 和 std::enable_shared_from_this
#include <boost/asio/ssl/stream.hpp> // Asio 提供的 TLS/SSL 流
#include "http/handler.hpp"
#include "http/router.hpp"
#include <boost/asio/experimental/channel.hpp> // <-- 引入 channel

/**
 * @class Http2Session
 * @brief 管理一个完整的 HTTP/2 客户端连接会话。
 *
 * HTTP/2 协议的处理器，是网络层的具体实现，负责管理单个 HTTP/2 连接的生命周期
 *
 *
 * 这个类是服务器处理 HTTP/2 协议的核心。它的一个实例对应一个已建立的 TLS 连接，
 * 负责该连接上所有的 HTTP/2 帧的接收、解析、处理和发送。
 *
 * 它利用 nghttp2 库来处理底层的 HTTP/2 协议逻辑（如帧的编码解码、流量控制等），
 * 并通过 Boost.Asio 的协程来实现高效的、非阻塞的异步 I/O 操作。
 *
 * 它的生命周期由 std::shared_ptr 管理，以确保在所有相关的异步操作完成前，
 * session 对象不会被销毁。
 */
class Http2Session : public std::enable_shared_from_this<Http2Session> {
public:
    // 定义 SSL/TLS 流的类型别名，方便使用
    using SSLStream = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;
    // 定义指向 SSLStream 的共享指针的类型别名
    using StreamPtr = std::shared_ptr<SSLStream>;

    /**
     * @brief 构造函数。
     * @param stream 一个已经完成 TLS 握手的 SSLStream 的共享指针。
     * @param router 对服务器主路由器的引用，用于分发请求。
     */
    Http2Session(StreamPtr stream, Router &router);

    /**
     * @brief 析构函数。
     *        负责释放 nghttp2_session 等资源。
     */
    ~Http2Session();

    // 禁止拷贝构造和拷贝赋值，因为每个 session 都是唯一的
    Http2Session(const Http2Session &) = delete;

    Http2Session &operator=(const Http2Session &) = delete;

    /**
     * @brief 静态工厂方法，用于创建 Http2Session 的共享指针实例。
     *
     * 这是创建 session 对象的推荐方式，它能正确地处理 `std::enable_shared_from_this`。
     * @param stream 传入的 SSLStream 共享指针。
     * @param r 对路由器的引用。
     * @return 一个新的 Http2Session 的 std::shared_ptr。
     */
    static std::shared_ptr<Http2Session> create(StreamPtr stream, Router &r) {
        return std::make_shared<Http2Session>(std::move(stream), r);
    }

    /**
     * @brief 启动会话处理。
     *
     * 这是会话的入口点。它会启动一个常驻的协程 (`session_loop`) 来处理这个连接上的所有事件。
     */
    boost::asio::awaitable<void> start();

    /**
     * @brief 启动优雅关闭流程的协程。
     *
     * 这个方法会向客户端发送一个 GOAWAY 帧，通知对方不要再发起新请求。
     * 调用者应该 co_await 这个函数来确保 GOAWAY 帧被提交。
     * @param error_code 发送给客户端的错误码，默认为 NGHTTP2_NO_ERROR。
     */
    boost::asio::awaitable<void> graceful_shutdown(uint32_t error_code = NGHTTP2_NO_ERROR);

private:
    /**
     * @brief 初始化 nghttp2 会话和相关的回调函数。
     */
    void init_session();

    /**
     * @brief 会话的主循环协程。
     *
     * 这是“融合模型”的核心。这个单一的协程负责处理此连接上的所有读写操作，
     * 包括 HTTP/2 握手和后续的请求/响应流。这种设计消除了读写协程之间的
     * 同步问题，提供了最健壮的实现。
     * @return boost::asio::awaitable<void>
     */
    boost::asio::awaitable<void> session_loop();

    boost::asio::awaitable<void> dispatcher_loop(); // 负责处理请求
    /**
     * @brief 执行写操作的协程。
     *
     * 从 nghttp2 的发送缓冲区中取出数据，并通过 `async_write` 写入套接字。
     * `co_await` 的使用为我们提供了天然的、隐式的背压控制。
     * @return boost::asio::awaitable<void>
     */
    boost::asio::awaitable<void> do_write();

    /**
     * @brief 请求分发和处理协程。
     *
     * 当一个完整的请求被接收后，此协程被调用。它会通过路由器找到对应的
     * 业务逻辑处理函数 (Handler)，`co_await` 该处理函数，然后将生成的响应
     * 提交给 nghttp2。
     * @param stream_id 需要处理的 HTTP/2 流 ID。
     * @return boost::asio::awaitable<void>
     */
    boost::asio::awaitable<void> dispatch(int32_t stream_id);

    // 新增一个专门的 writer_loop
    boost::asio::awaitable<void> writer_loop();

    // 新增一个触发函数
    void schedule_write();

    boost::asio::awaitable<void> do_graceful_shutdown(uint32_t error_code);

    // --- nghttp2 C-style 回调函数 ---
    // 这些静态函数作为 C 库 nghttp2 和 C++ 类 Http2Session 之间的桥梁。
    // 它们都接收一个 `void* user_data` 参数，我们用它来传递 `this` 指针。
    static int on_header_callback(nghttp2_session *, const nghttp2_frame *, const uint8_t *, size_t, const uint8_t *, size_t, uint8_t, void *);

    static int on_data_chunk_recv_callback(nghttp2_session *, uint8_t, int32_t, const uint8_t *, size_t, void *);

    static int on_stream_close_callback(nghttp2_session *, int32_t, uint32_t, void *);

    static int on_frame_recv_callback(nghttp2_session *, const nghttp2_frame *, void *);

    /**
     * @struct StreamContext
     * @brief 存储单个 HTTP/2 流（即一个请求）的上下文信息。
     */
    struct StreamContext {
        std::vector<std::pair<std::string, std::string> > headers; // 原始请求头

        std::string body; // 请求体
    };

    /**
     * @struct ProviderPack
     * @brief 用于向 nghttp2 提供响应体数据的辅助结构。
     */
    struct ProviderPack {
        std::shared_ptr<std::string> content; // 指向响应体字符串的共享指针，以管理其生命周期
        size_t offset = 0; // 用于跟踪已发送数据量的偏移量，以实现高效的零拷贝发送
    };

    StreamPtr stream_; // 指向 SSL/TLS 流的共享指针
    Router &router_; // 对服务器主路由器的引用
    boost::asio::strand<boost::asio::any_io_executor> strand_; // strand 保证所有对 session 状态的操作都在同一个逻辑线程上，避免数据竞争
    nghttp2_session *session_; // 指向 nghttp2 会话实例的裸指针
    std::unordered_map<int32_t, StreamContext> streams_; // 存储活跃的流上下文，键是 stream_id
    std::unordered_map<int32_t, std::shared_ptr<ProviderPack> > provider_pack_; // 存储响应数据提供者
    boost::asio::steady_timer idle_timer_; // 用于实现连接空闲超时的计时器

    boost::asio::steady_timer write_trigger_;
    bool write_in_progress_ = false;

    // --- **[新增]** 用于解耦的 channel ---
    using StreamIdChannel = boost::asio::experimental::channel<void(boost::system::error_code, int32_t)>;
    StreamIdChannel dispatch_channel_;
};

#endif // HTTP2SESSION_HPP
