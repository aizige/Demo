//
// Created by Aiziboy on 2025/7/16.
//

#ifndef H2CSESSION_HPP
#define H2CSESSION_HPP


#include <boost/asio.hpp>          // Boost.Asio 核心库
#include <boost/beast/core.hpp>    // Boost.Beast 核心，我们主要使用其 HTTP 组件
#include <nghttp2/nghttp2.h>       // C 语言实现的 HTTP/2 协议核心库
#include <unordered_map>           // 用于存储 stream 上下文等
#include <string>
#include <memory>                  // 用于 std::shared_ptr 和 std::enable_shared_from_this
#include <boost/asio/experimental/channel.hpp>

#include "http/handler.hpp"
#include "http/router.hpp"

/**
 * @class H2cSession ,专为明文 TCP 套接字设计
 * @brief 管理一个完整的 HTTP/2 客户端连接会话。
 *
 * HTTP/2 协议的处理器，是网络层的具体实现，负责管理单个 HTTP/2 连接的生命周期
 *
 *
 * 这个类是服务器处理 HTTP/2 协议的核心。它的一个实例对应一个已建立的 socket 连接，
 * 负责该连接上所有的 HTTP/2 帧的接收、解析、处理和发送。
 *
 * 它利用 nghttp2 库来处理底层的 HTTP/2 协议逻辑（如帧的编码解码、流量控制等），
 * 并通过 Boost.Asio 的协程来实现高效的、非阻塞的异步 I/O 操作。
 *
 * 它的生命周期由 std::shared_ptr 管理，以确保在所有相关的异步操作完成前，
 * session 对象不会被销毁。
 */

class H2cSession : public std::enable_shared_from_this<H2cSession> {
public:
    using Socket = boost::asio::ip::tcp::socket;
    using SocketPtr = std::shared_ptr<Socket>;

    /**
   * @brief 构造函数。
   * @param socket 一个已经建立的 TCP socket 的共享指针。
   * @param router 对服务器主路由器的引用。
   */
    H2cSession(SocketPtr socket, Router &router);

    /**
     * @brief 析构函数。
     *        负责释放 nghttp2_session 等资源。
     */
    ~H2cSession();

    // 禁止拷贝构造和拷贝赋值，因为每个 session 都是唯一的
    H2cSession(const H2cSession &) = delete;

    H2cSession &operator=(const H2cSession &) = delete;

    /**
     * @brief 静态工厂方法，用于创建 H2cSession 的共享指针实例。
     *
     * 这是创建 session 对象的推荐方式，它能正确地处理 `std::enable_shared_from_this`。
     * @param socket 传入的 TCP 共享指针。
     * @param r 对路由器的引用。
     * @return 一个新的 H2cSession 的 std::shared_ptr。
     */
    static std::shared_ptr<H2cSession> create(SocketPtr socket, Router &r) {
        return std::make_shared<H2cSession>(std::move(socket), r);
    }


    /**
     * @brief 启动会话处理。
     * @param initial_request (可选) 触发升级的 HTTP/1.1 请求。如果非空，
     *        会话将以 "Upgrade" 模式启动。
     * @param initial_data (可选) 在协议嗅探或请求解析期间可能已经预读的数据。
     *        这通常是客户端在发送升级请求后立即发送的 HTTP/2 连接前言。
     */
    boost::asio::awaitable<void> start(boost::optional<HttpRequest> initial_request,boost::beast::flat_buffer &initial_data
    );

    /**
     * @brief 启动优雅关闭流程的协程。
     */
    boost::asio::awaitable<void> graceful_shutdown(uint32_t error_code = NGHTTP2_NO_ERROR);

private:
    void init_session();

    boost::asio::awaitable<void> session_loop(
        boost::optional<HttpRequest> initial_request,
        boost::beast::flat_buffer& initial_data
    );
    boost::asio::awaitable<void> dispatcher_loop();
    boost::asio::awaitable<void> do_write();
    boost::asio::awaitable<void> dispatch(int32_t stream_id);
    boost::asio::awaitable<void> writer_loop();
    void schedule_write();
    boost::asio::awaitable<void> do_graceful_shutdown(uint32_t error_code);

    // nghttp2 C-style 回调函数
    static int on_header_callback(nghttp2_session *, const nghttp2_frame *, const uint8_t *, size_t, const uint8_t *, size_t, uint8_t, void *);
    static int on_data_chunk_recv_callback(nghttp2_session *, uint8_t, int32_t, const uint8_t *, size_t, void *);
    static int on_stream_close_callback(nghttp2_session *, int32_t, uint32_t, void *);
    static int on_frame_recv_callback(nghttp2_session *, const nghttp2_frame *, void *);

    // StreamContext 和 ProviderPack 保持不变
    struct StreamContext {
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
    };
    struct ProviderPack {
        std::shared_ptr<std::string> content;
        size_t offset = 0;
    };

    SocketPtr socket_;
    Router &router_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    nghttp2_session *session_;
    std::unordered_map<int32_t, StreamContext> streams_;
    std::unordered_map<int32_t, std::shared_ptr<ProviderPack>> provider_pack_;
    boost::asio::steady_timer idle_timer_;
    boost::asio::steady_timer write_trigger_;
    bool write_in_progress_ = false;

    using StreamIdChannel = boost::asio::experimental::channel<void(boost::system::error_code, int32_t)>;
    StreamIdChannel dispatch_channel_;
};


#endif //H2CSESSION_HPP
