//
// Created by ubuntu on 2025/7/21.
//
#include <aizix/core/client/https_connection.hpp>
#include <aizix/utils/finally.hpp>
#include <aizix/utils/pocess_info.hpp>
#include <aizix/utils/time_util.hpp> // 假设 time_utils 在这里

#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/redirect_error.hpp>
#include <spdlog/spdlog.h>
#include <aizix/lib/ada.h>




// 将 C++14 的时间字面量（如 30s）引入当前作用域
using namespace std::literals::chrono_literals;
using namespace boost::asio::experimental::awaitable_operators;

/**
 * @brief HttpsConnection 类的构造函数。
 * @param stream 一个已经完成 TLS 握手的 boost::beast::ssl_stream 的共享指针。
 * @param pool_key 标识此连接所属连接池的键（通常是 "scheme://host:port"）。
 */
HttpsConnection::HttpsConnection(StreamPtr stream, std::string pool_key)
    : stream_(std::move(stream)), // 直接移动传入的 stream
      id_(generate_connection_id()),
      pool_key_(std::move(pool_key)),
      last_used_timestamp_(std::chrono::steady_clock::now()), last_ping_timestamp_(std::chrono::steady_clock::now()) {
    SPDLOG_DEBUG("HttpsConnection [{}] for pool [{}] created.", id_, pool_key_);
}


/**
 * @brief 执行一次应用层心跳检测 (PING)。
 *
 * 通过发送一个轻量级的 HTTP HEAD 请求，并等待一个合法的响应头，
 * 来端到端-地验证 TCP -> TLS -> HTTP 整个链路的健康状况。
 *
 * @return boost::asio::awaitable<bool> 如果 PING 成功则 co_return true，否则 co_return false。
 */
boost::asio::awaitable<bool> HttpsConnection::ping() {
    co_await boost::asio::dispatch(stream_->get_executor(),boost::asio::use_awaitable);

    // 1. 前置检查：如果连接已标记为不可用，或正在处理业务请求，则无需 PING。
    if (!is_usable()) {
        co_return false;
    }

    // 更新最后一次发送ping的时间
    update_ping_used_time();

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

        auto verb = http::verb::head;
        if (pool_key_ == "https://www.okx.com:443") {
            verb = http::verb::options;
        }
        // 1. 构造一个轻量级的 HTTP head 请求
        http::request<http::empty_body> req{verb, "/", 11};
        req.set(http::field::host, url->get_hostname());
        req.set(http::field::user_agent, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
        req.set(http::field::accept, "*/*");
        req.set(http::field::connection, "keep-alive");
        req.set(http::field::accept_language, "en-US,en;q=0.9,zh-CN;q=0.8,zh;q=0.7,zh-HK;q=0.6");


        // 2. 异步发送请求
        co_await http::async_write(*stream_, req, boost::asio::use_awaitable);

        // 3.  使用 parser
        boost::beast::flat_buffer ping_buffer;
        // 创建一个只解析头部的 parser，empty_body 表示我们不期望/不关心 body
        http::response_parser<http::empty_body> header_parser;

        // 4. 异步读取响应头。这个函数在解析完头部后就会立即返回。
        co_await http::async_read_header(*stream_, ping_buffer, header_parser, boost::asio::use_awaitable);



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
    } catch (const std::exception& e) {
        // 捕获所有其他异常，如内存分配失败等。
        keep_alive_ = false;
        SPDLOG_DEBUG("连接 [{}] [{}] PING 失败，出现异常：{}", id(), pool_key_, e.what());
        co_return false;
    }
}


/**
 * @brief 异步地在此连接上执行一个 HTTP/1.1 请求。
 *
 * 这个协程负责将请求写入 TLS 流，然后读取完整的响应。
 * 它使用 RAII (Finally guard) 来安全地管理 active_streams_ 计数器，
 * 保证在任何退出路径（正常返回或异常）下，计数器都能被正确递减。
 *
 * @param request 要发送的 HttpRequest 对象，其所有权被此函数接管。
 * @return 一个协程句柄 (awaitable)，其最终结果是收到的 HttpResponse 对象。
 * @throws boost::system::system_error 如果发生不可恢复的网络 I/O 错误。
 */
boost::asio::awaitable<HttpResponse> HttpsConnection::execute(const HttpRequest& request) {
    // 立即增加并发流计数器。
    ++active_streams_;

    // 创建一个 RAII guard，确保在函数退出时（无论如何退出）计数器都会被递减。
    auto guard = Finally([this] {
        --active_streams_;
    });

    // 更新此连接的最后活动时间戳。
    update_last_used_time();

    try {
        // 异步发送请求。
        // 使用 std::move(request) 来转移所有权，这既能清晰地表达“消耗”意图，
        // 也能在 body 较大时提高性能。
        co_await http::async_write(*stream_, request, boost::asio::use_awaitable);
        SPDLOG_DEBUG("HttpsConnection [{}] request sent.", id_);

        // 异步接收响应。
        HttpResponse response;
        co_await http::async_read(*stream_, buffer_, response, boost::asio::use_awaitable);
        SPDLOG_DEBUG("HttpsConnection [{}] response received with status {}.", id_, response.result_int());

        // 根据服务器的响应，更新此连接的 keep-alive 状态。
        keep_alive_ = response.keep_alive();
        SPDLOG_DEBUG("HttpsConnection [{}] keep-alive state set to {}.", id_, keep_alive_);

        // 正常返回响应。在 co_return 发生时，guard 会被析构。
        co_return response;
    } catch (const boost::system::system_error& e) {
        SPDLOG_ERROR("HttpsConnection [{}] I/O error during execute: {}", id_, e.what());
        // 发生网络错误，此连接不再可信，必须标记为不可复用。
        keep_alive_ = false;
        // 重新抛出异常，以便上层（如 HttpClient）的重试逻辑能够捕获。
        // 在 throw 之前，guard 会被析构，保证计数器被正确递减。
        throw;
    }
}

/**
 * @brief 判断此 HTTP/1.1 连接当前是否健康且可供连接池复用。
 */
bool HttpsConnection::is_usable() const {
    // 一个可用的连接必须同时满足：SSL流有效，底层TCP套接字打开，且协议层允许保持连接。
    //if (stream_) {SPDLOG_DEBUG("stream_ = true");} else { SPDLOG_DEBUG("stream_ = true"); }
    //SPDLOG_DEBUG("stream_->.is_open() = {}",stream_->next_layer().is_open());
    //SPDLOG_DEBUG("keep_alive = {}",keep_alive_);

    return stream_ && stream_->next_layer().is_open() && keep_alive_;
}

/**
 * @brief 异步地、主动地关闭此 HTTPS 连接。
 */
boost::asio::awaitable<void> HttpsConnection::close() {
    co_await boost::asio::dispatch(stream_->get_executor(),boost::asio::use_awaitable);

    // 立即将连接在逻辑上标记为不可用。
    keep_alive_ = false;

    try {
        // 先检查指针，然后使用
        if (stream_ && stream_->next_layer().is_open()) {
            boost::system::error_code ec;

            // 1. 尝试进行优雅的异步执行 TLS shutdown (发送 close_notify)。
            //    使用 redirect_error 捕获错误，而不是抛出异常。
            co_await stream_->async_shutdown(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            // 即使 shutdown 失败，我们也要继续关闭底层 socket。

            // 2. 最终，无论如何都确保底层 TCP socket 被物理关闭
            //    我们只关心尽力关闭，所以忽略这里的错误码。
            if (stream_->next_layer().is_open()) {
                const auto error_code = stream_->next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
                const auto code = stream_->next_layer().close(ec);
                if (error_code) {
                    throw boost::system::system_error(error_code);
                }
                if (code) {
                    throw boost::system::system_error(code);
                }
            }
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("HttpsConnection [{}] exception during async_shutdown: {}", id_, e.what());
    }
    co_return;
}

/**
 * @brief 更新连接的最后一次活动时间戳为当前时间。
 */
void HttpsConnection::update_last_used_time() {
    last_used_timestamp_  = std::chrono::steady_clock::now();
}

/**
 * @brief 更新连接的最后一次ping动作的时间戳为当前时间。
 */
void HttpsConnection::update_ping_used_time() {
    last_ping_timestamp_  = std::chrono::steady_clock::now();
}

/**
 * @brief 获取此连接实例的唯一标识符。
 */
const std::string& HttpsConnection::id() const { return id_; }

/**
 * @brief 获取此连接所属的连接池键。
 */
const std::string& HttpsConnection::get_pool_key() const { return pool_key_; }

/**
 * @brief 生成一个在多进程环境中具有高可追溯性的唯一连接ID。
 *
 * 生成的ID格式为: "<type_prefix><process_prefix><sequence_number>"
 * 例如: "conn-h1s-12345-1"
 *
 * - **类型前缀 (Type Prefix)**: (如 "conn-h2-") 用于在日志中快速识别连接的类型。
 * - **进程前缀 (Process Prefix)**: (如 "12345-") 来自 ProcessInfo，通常是进程ID (PID)。
 *   这确保了在多个服务实例（进程）同时运行时，它们的连接ID不会冲突，
 *   极大地提升了在聚合日志系统中的可追溯性。
 * - **序列号 (Sequence Number)**: (如 "1") 一个进程内唯一的、自增的原子计数器，
 *   保证了在单个进程内的线程安全和唯一性。
 *
 * @return std::string 一个全局唯一的、可读性强的连接ID。
 */
std::string HttpsConnection::generate_connection_id() {
    static std::atomic<uint64_t> counter = 0;
    if (counter >= 1000000) counter = 0;
    // 最终ID: "conn-h1s-12345-1"
    return "conn-h1s-" + ProcessInfo::get_prefix() + std::to_string(++counter);
}
