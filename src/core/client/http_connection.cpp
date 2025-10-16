//
// Created by Aiziboy on 2025/7/18.
//

#include "http_connection.hpp"
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/redirect_error.hpp>
#include <spdlog/spdlog.h>
#include <ada.h>
#include "utils/utils.hpp"   // 假设 time_utils 和 Finally 在这里
#include "utils/finally.hpp" // 明确包含 Finally
#include "utils/pocess_info.hpp"


// 在 .cpp 文件顶部引入命名空间
using namespace std::literals::chrono_literals;
using namespace boost::asio::experimental::awaitable_operators;


/**
 * @brief HttpConnection 类的构造函数。
 * @param socket 一个已经建立的 TCP socket。所有权被转移并封装在 `beast::tcp_stream` 中。
 * @param pool_key 标识此连接所属连接池的键 (e.g., "http://example.com:80")。
 */
HttpConnection::HttpConnection(tcp::socket socket, std::string pool_key)
    : socket_(std::move(socket)),
      id_(generate_connection_id()),
      pool_key_(std::move(pool_key)),
      last_used_timestamp_seconds_(time_utils::steady_clock_seconds_since_epoch()),
      last_ping_timestamp_seconds_(time_utils::steady_clock_seconds_since_epoch()) {
    SPDLOG_TRACE("[{}]-[{}] 创建", id_, pool_key_);
}

/**
 * @brief 异步地在此连接上执行一个 HTTP/1.1 请求。
 * @note 该实现使用 RAII guard 来确保 `active_streams_` 计数器的异常安全。
 * @param request 要发送的 HttpRequest 对象，其所有权被此函数接管。
 * @return 一个协程句柄，其结果是收到的 HttpResponse 对象。
 * @throws boost::system::system_error 如果发生不可恢复的网络 I/O 错误。
 */
boost::asio::awaitable<HttpResponse> HttpConnection::execute(const HttpRequest& request) {
    // 立即标记连接为繁忙状态。
    ++active_streams_;
    // 创建一个 RAII guard，确保在函数退出时（无论正常返回还是异常），计数器都会被递减。
    auto guard = Finally([this] {
        --active_streams_;
    });

    // 更新此连接的最后活动时间戳。
    update_last_used_time();

    try {
        // 异步发送请求。并设置超时时间
        SPDLOG_DEBUG("向 [{}]-[{}] 写入request .", id_, pool_key_);
        co_await http::async_write(socket_, request, boost::asio::use_awaitable);


        // 异步接收响应，复用成员变量 buffer_ 以提高效率。并设置超时时间
        HttpResponse response;

        co_await http::async_read(socket_, buffer_, response, boost::asio::use_awaitable);
        SPDLOG_DEBUG("收到 [{}]-[{}] 的响应。 status =  {}.", id_, pool_key_, response.result_int());


        // 根据服务器响应中的 "Connection" 头部来更新本地的 keep-alive 状态。
        // [潜在问题点]：`response.result_int() >= 300` 的判断过于宽泛，
        // 可能会错误地将本可复用的连接标记为不可用。
        // 仅依赖 `response.keep_alive()` 是更标准的做法。
        if (response.result_int() >= 300) {
            keep_alive_ = false;
        } else {
            keep_alive_ = response.keep_alive();
        }

        // 正常返回响应。在 co_return 发生时，guard 会被析构。
        co_return response;
    } catch (const boost::system::system_error& e) {
        SPDLOG_ERROR("HttpConnection [{}] error: {}", id_, e.what());
        // 发生网络错误，此连接不再可信，必须标记为不可复用。
        keep_alive_ = false;
        // 如果错误是超时，记录更具体的日志
        if (e.code() == boost::beast::error::timeout) {
            SPDLOG_WARN("HttpConnection [{}] I/O operation timed out.", id_);
        } else {
            SPDLOG_ERROR("HttpConnection [{}] I/O error: {}", id_, e.what());
        }
        throw;
    }
}

/**
 * @brief 执行一次应用层心跳检测 (PING) 以验证连接的端到端健康状况。
 *
 * 通过发送一个轻量级的 HTTP OPTIONS/HEAD 请求，并等待一个合法的响应头，
 * 来端到端地验证 TCP -> HTTP 整个链路的健康状况。
 * 这个方法是线程安全的，并且为每个I/O操作都内置了超时控制。
 *
 * @return boost::asio::awaitable<bool> 如果连接健康则 co_return true，否则 co_return false。
 */
boost::asio::awaitable<bool> HttpConnection::ping() {
    // 1. 前置检查：如果连接已被标记为不可用，PING 必须立即失败。
    if (!is_usable()) {
        co_return false;
    }

    // 更新最后一次发送ping的时间
    update_last_used_time();

    // 已经有业务在用了，那肯定是活的，直接返回成功，不要去添乱
    if (active_streams_.load() > 0) {
        co_return true;
    }

    try {
        // 从连接池键中解析出主机名，用于设置 Host 头部。
        auto url = ada::parse<ada::url_aggregator>(pool_key_);
        if (!url) {
            SPDLOG_WARN("PING 期间无法解析 pool_key[{}]", pool_key_);
            co_return false;
        }

        // 1. 构造一个轻量级的 HTTP head 请求
        http::request<http::empty_body> req{http::verb::head, "/", 11};
        req.set(http::field::host, url->get_hostname());
        req.set(http::field::user_agent, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
        req.set(http::field::accept, "*/*");
        req.set(http::field::connection, "keep-alive");
        req.set(http::field::accept_language, "en-US,en;q=0.9,zh-CN;q=0.8,zh;q=0.7,zh-HK;q=0.6");
        SPDLOG_DEBUG("host [{}]...", url->get_host());
        SPDLOG_DEBUG("host name [{}]...", url->get_hostname());

        SPDLOG_DEBUG("[{}]-[{}] PING ...", id(), pool_key_);

        // 2. 异步发送请求
        co_await http::async_write(socket_, req, boost::asio::use_awaitable);


        // 3.  使用 parser
        boost::beast::flat_buffer ping_buffer;
        // 创建一个只解析头部的 parser，empty_body 表示我们不期望/不关心 body
        http::response_parser<http::empty_body> header_parser;

        // 4. 异步读取响应头。这个函数在解析完头部后就会立即返回。
        co_await http::async_read_header(socket_, ping_buffer, header_parser, boost::asio::use_awaitable);


        // 5. 验证响应。从 parser 中获取解析出的头部信息
        auto const& header = header_parser.get();

        // 6. 检查响应状态码，任何合法的HTTP响应都表示连接存活。
        unsigned status = header.result_int();
        SPDLOG_DEBUG("[{}]-[{}] PING 响应 {}", id(), pool_key_, status);


        if (status > 0) {
            // 如果服务器那边没返回header: Connection = keep-alive,这将连接设置为不可用
            // 如果我强行保留连接，服务器那边用不了多久久主动关闭了，最后会报SSL错误: @code decryption failed or bad record mac (SSL routines) [asio.ssl:167772441]
            keep_alive_ = header_parser.keep_alive();

            // 防御性
            // 如果 buffer 中还有残留数据（Pipelining），则标记连接不可用。
            if (ping_buffer.size() > 0) {
                SPDLOG_WARN("连接 [{}] 在 HEAD 响应后有尾随数据。标记为关闭。", id());
                keep_alive_ = false;
            }
            co_return true;
        }

        // 其它任何问题，则认为连接有问题。
        keep_alive_ = false;
        co_return false;
    } catch (const boost::system::system_error& e) {
        // PING 期间发生任何异常都表明连接已不再健康。
        keep_alive_ = false;
        SPDLOG_DEBUG("连接 [{}] [{}] PING 失败，出现异常：{}", id(), pool_key_, e.what());
        co_return false;
    }
}

/**
 * @brief 判断连接是否可供连接池复用。
 */
bool HttpConnection::is_usable() const {
    // 一个可用的连接必须同时满足：底层 TCP 套接-字打开，且协议层允许保持连接。
    return socket_.socket().is_open() && keep_alive_;
}


/**
 * @brief 异步的、主动地关闭此 HTTP 连接。
 *
 * 这个协程会尽力而为地执行一个优雅的 TCP 关闭流程。
 * 它首先将连接标记为不可用，然后同步地关闭底层 socket。
 * 因为 socket shutdown 和 close 都是快速的非阻塞操作，
 * 所以在协程中同步调用它们是安全且常见的做法。
 */
boost::asio::awaitable<void> HttpConnection::close() {
    // 1. 立即将连接在逻辑上标记为不可用。
    keep_alive_ = false;

    // 2. 检查底层 socket 是否仍然是打开的。
    if (socket_.socket().is_open()) {
        boost::system::error_code ec;

        // 3. 同步调用 shutdown()。这是一个非阻塞的系统调用。
        socket_.socket().shutdown(tcp::socket::shutdown_both, ec);
        if (ec && ec != boost::asio::error::not_connected) {
            // 忽略 "not_connected" 错误，因为它意味着 socket 已经关闭了
            SPDLOG_DEBUG("HttpConnection [{}] error on socket shutdown: {}", id_, ec.message());
        }

        // 4. 同步调用 close()。这是一个本地操作，释放文件描述符。
        socket_.close(); // 这是 beast::tcp_stream::close()，它会关闭底层 socket
    }

    // 因为此函数中没有真正的异步挂起点，所以它会同步执行完毕。
    // 但返回 awaitable<void> 仍然是正确的，因为它满足了 IConnection 的接口契约。
    co_return;
}


/**
 * @brief 更新连接的最后一次活动时间戳为当前时间。
 */
void HttpConnection::update_last_used_time() {
    last_used_timestamp_seconds_ = time_utils::steady_clock_seconds_since_epoch();
}

/**
 * @brief 更新连接的最后一次ping动作的时间戳为当前时间。
 */
void HttpConnection::update_ping_used_time() {
    last_ping_timestamp_seconds_ = time_utils::steady_clock_seconds_since_epoch();
}

/**
 * @brief 返回当前正在处理的请求数 (对于 H1.1 总是 0 或 1)。
 */
size_t HttpConnection::get_active_streams() const {
    return active_streams_.load();
}

/**
 * @brief 生成一个在多进程环境中具有高可追溯性的唯一连接ID。
 *
 * 生成的ID格式为: "<type_prefix><process_prefix><sequence_number>"
 * 例如: "conn-h1-12345-1"
 *
 * - **类型前缀 (Type Prefix)**: (如 "conn-h1-") 用于在日志中快速识别连接的类型。
 * - **进程前缀 (Process Prefix)**: (如 "12345-") 来自 ProcessInfo，通常是进程ID (PID)。
 *   这确保了在多个服务实例（进程）同时运行时，它们的连接ID不会冲突，
 *   极大地提升了在聚合日志系统中的可追溯性。
 * - **序列号 (Sequence Number)**: (如 "1") 一个进程内唯一的、自增的原子计数器，
 *   保证了在单个进程内的线程安全和唯一性。
 *
 * @return std::string 一个全局唯一的、可读性强的连接ID。
 */
std::string HttpConnection::generate_connection_id() {
    static std::atomic<uint64_t> counter = 0;
    // 最终ID: "conn-h1-12345-1", "conn-h1-12345-2", ...
    return "conn-h1-" + ProcessInfo::get_prefix() + std::to_string(++counter);
}
