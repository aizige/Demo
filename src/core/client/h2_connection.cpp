//
// Created by ubuntu on 2025/7/21.
//

#include "h2_connection.hpp"
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/impl/co_spawn.hpp>
#include <spdlog/spdlog.h>
#include <cstring>                  // 用于 C 风格字符串操作，如 memcpy
#include <chrono>                   // C++ 时间库
#include <boost/asio/detached.hpp>

#include <boost/asio/experimental/awaitable_operators.hpp>
#include <nghttp2/nghttp2.h>
#include <boost/system/system_error.hpp>
#include <vector>
#include <string>
#include <boost/beast/ssl.hpp>
#include <atomic>
#include <deque>
#include <boost/asio/co_spawn.hpp>
#include <ranges>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/asio/experimental/channel.hpp>
#include "error/aizix_error.hpp"
#include "utils/finally.hpp"
#include <functional>

#include "utils/pocess_info.hpp"
#include "utils/toml.hpp"

namespace asio = boost::asio;
// 将 C++14 的时间字面量（如 30s）引入当前作用域
using namespace std::literals::chrono_literals;
// 引入 asio 协程操作符，如 || (race)
using namespace boost::asio::experimental::awaitable_operators;

// 内部常量
namespace {
    // 使用 string_view literal (sv) 来避免构造 string
    using namespace std::literals::string_view_literals;

    constexpr auto H2_METHOD_SV = ":method"sv;
    constexpr auto H2_SCHEME_SV = ":scheme"sv;
    constexpr auto H2_AUTHORITY_SV = ":authority"sv;
    constexpr auto H2_PATH_SV = ":path"sv;
}

Http2Connection::Http2Connection(StreamPtr stream, std::string pool_key, size_t max_concurrent_streams,  std::chrono::milliseconds max_idle_ms)
    : stream_(std::move(stream)),
      pool_key_(std::move(pool_key)),
      id_(generate_connection_id()),
      // 初始化 Actor 的邮箱，设置一个缓冲区大小
      actor_channel_(stream_->get_executor(), 256),
      // 初始化握手信号 channel，缓冲区为1即可
      handshake_signal_(stream_->get_executor(), 1),
      max_concurrent_streams_(max_concurrent_streams),
      last_used_timestamp_ms_(std::chrono::steady_clock::now()),
      last_ping_timestamp_ms_(std::chrono::steady_clock::now()),
      // 所有异步对象都使用同一个 executor，确保它们在同一个上下文中执行
      idle_timer_(stream_->get_executor()),
      max_idle_time_(max_idle_ms) {
    // 初始化定时器为永不超时，避免在没有活动时立即触发
    idle_timer_.expires_at(std::chrono::steady_clock::time_point::max());
    SPDLOG_DEBUG("Create a connection [{}]-[{}] ", id_, pool_key);
    //SPDLOG_DEBUG("初始化上次使用时间点:{} ", Http2Connection::get_last_used_timestamp_ms().time_since_epoch().count());
    //SPDLOG_DEBUG("初始化ping用时间点:{} ", Http2Connection::get_ping_used_timestamp_ms().time_since_epoch().count());
}


Http2Connection::~Http2Connection() {
    // 确保 nghttp2 会话资源被释放
    if (session_) {
        nghttp2_session_del(session_);
        session_ = nullptr;
    }
    SPDLOG_DEBUG("销毁连接 [{}]-[{}]", id_, pool_key_);
}

/// 生成一个易于识别的唯一ID，包含进程前缀和递增计数器
std::string Http2Connection::generate_connection_id() {
    static std::atomic<uint64_t> counter = 0;
    if (counter >= 1000000) counter = 0;
    // 最终ID: "conn-h2-12345-1"...
    return "conn-h2-" + ProcessInfo::get_prefix() + std::to_string(++counter);
}


/**
 * @brief 启动 Actor 协程并等待握手完成
 */
asio::awaitable<void> Http2Connection::run() {
    // 使用 co_spawn 在后台启动 actor_loop()。这是 Actor 模式的核心。
    // actor_loop 将独立运行，直到连接关闭或出错。
    asio::co_spawn(
        stream_->get_executor(),
        // 使用 shared_from_this() 捕获 self，确保 Http2Connection 对象在协程运行时保持存活
        [self = shared_from_this()]() -> asio::awaitable<void> {
            try {
                // 执行主循环
                co_await self->actor_loop();
            } catch (const std::exception& e) {
                // 捕获 actor_loop 抛出的任何异常
                if (std::string_view(e.what()) != "The coro was cancelled") {
                    SPDLOG_WARN("H2 连接 [{}] 的主循环因异常退出: {}", self->id_, e.what());
                }
                // 确保握手信号被关闭，以防调用者永久等待
                self->handshake_signal_.close();
            }
            // 主循环结束后，执行清理和关闭
            co_await self->close();
        },
        // detached 表示我们不关心这个后台协程的结果，它自我管理生命周期
        asio::detached
    );

    // post 一个空操作，确保 co_spawn 有机会开始执行
    co_await asio::post(asio::use_awaitable);

    // 等待 actor_loop() 完成握手后发出的信号。
    // 这使得 run() 方法在返回时，连接已经准备好接收请求。
    co_await handshake_signal_.async_receive(asio::as_tuple(asio::use_awaitable));
    SPDLOG_DEBUG("{}: 握手已完成.", pool_key_);
}


/**
 * @brief 外部接口：发送一个 HTTP 请求
 */
asio::awaitable<HttpResponse> Http2Connection::execute(const HttpRequest& request) {
    // 每次使用连接时都更新时间戳
    update_last_used_time();

    // 获取当前协程的执行器
    auto ex = co_await asio::this_coro::executor;

    // 如果连接已关闭或收到服务器的 GOAWAY，则拒绝新请求
    if (is_closing_ || remote_goaway_received_) {
        SPDLOG_WARN("[{}] execute: 连接已关闭或收到 GOAWAY，拒绝请求", id_);
        throw boost::system::system_error(asio::error::connection_aborted, "连接已关闭或收到 GOAWAY，拒绝请求");
    }

    // 为这个特定的请求创建一个响应通道，作为“返回地址”
    auto response_channel = std::make_shared<asio::experimental::channel<void(boost::system::error_code, HttpResponse)>>(ex, 1);

    // 将请求和响应通道打包成一个消息
    H2RequestMessage msg{request, response_channel};

    // 异步地将消息发送到 actor_loop 的邮箱
    co_await actor_channel_.async_send({}, H2ActorMessage{std::move(msg)}, asio::as_tuple(asio::use_awaitable));

    try {
        // 等待 actor_loop 通过响应通道发回结果
        auto [ec, response] = co_await response_channel->async_receive(asio::as_tuple(asio::use_awaitable));

        if (ec) {
            SPDLOG_WARN("H2 [{}] execute: Response channel error: {}", id_, ec.message());
            throw boost::system::system_error(ec, "Response failed or was cancelled");
        }

        SPDLOG_DEBUG("[{}] execute: 收到响应，状态码 {}, 当前活动的steam = {}", id_, response.result_int(), active_streams_.load());
        co_return response;
    } catch (const boost::system::system_error& e) {
        // 特殊处理超时错误，提供更明确的错误信息
        if (e.code() == asio::error::operation_aborted) {
            throw boost::system::system_error(aizix_error::h2::receive_timeout, "接收响应超时（连接空闲）");
        }
        throw;
    }
}


bool Http2Connection::is_usable() const {
    // 综合多个条件判断连接是否健康可用
    if (is_closing_ || remote_goaway_received_ || !handshake_completed_ || !stream_ || session_ == nullptr) return false;
    if (!stream_->lowest_layer().is_open()) return false;
    return true;
}


/**
 * @brief 优雅地关闭连接
 */
boost::asio::awaitable<void> Http2Connection::close() {
    // 使用原子 exchange 确保关闭消息只发送一次
    if (close_called_.exchange(true)) {
        co_return;
    }
    SPDLOG_DEBUG("Http2Connection [{}]-[{}] 发送关闭消息给 Actor.", id_, pool_key_);
    // 异步地、非阻塞地尝试向 actor_channel_ 发送一个关闭消息。
    // 使用 try_send 是因为：
    // 1. 我们不希望 close() 方法阻塞或挂起。它应该是一个快速的、发起关闭的信号。
    // 2. 如果 channel 已满或已关闭，try_send 会立即返回 false，这没关系，
    //    因为这意味着 actor_loop 正在忙或已经关闭，关闭流程无论如何都会被触发。
    boost::system::error_code ec;
    actor_channel_.try_send(ec, H2CloseMessage{});
    co_return;
}


asio::awaitable<bool> Http2Connection::ping() {
    // 如果连接不可用或正忙，则认为 ping "成功" (因为连接池不应移除它)
    update_ping_used_time();

    if (!is_usable() || get_active_streams() > 0) {
        co_return true;
    }

    try {
        auto ex = co_await asio::this_coro::executor;
        const auto result_channel = std::make_shared<asio::experimental::channel<void(boost::system::error_code, bool)>>(ex, 1);

        // 发送 PingMessage
        co_await actor_channel_.async_send({}, H2PingMessage{result_channel}, asio::use_awaitable);

        // 等待 ping 的结果
        auto [ec, success] = co_await result_channel->async_receive(asio::as_tuple(asio::use_awaitable));
        if (ec) {
            co_return false;
        }
        co_return success;
    } catch (const std::exception& e) {
        SPDLOG_DEBUG("[{}]-[{}] 发送ping失败: {}", id_, pool_key_, e.what());
        is_closing_ = true;
        co_return false;
    }
}

void Http2Connection::update_last_used_time() {
    last_used_timestamp_ms_.store(std::chrono::steady_clock::now());
//    SPDLOG_DEBUG("更新上次使用时间点{}", get_last_used_timestamp_ms().time_since_epoch().count());

}

/**
 * @brief 更新连接的最后一次ping动作的时间戳为当前时间。
 */
void Http2Connection::update_ping_used_time() {
    last_ping_timestamp_ms_.store(std::chrono::steady_clock::now());
   // SPDLOG_DEBUG("更新上次ping时间点{}", get_ping_used_timestamp_ms().time_since_epoch().count());
}

size_t Http2Connection::get_active_streams() const {
    return active_streams_.load();
}

tcp::socket& Http2Connection::lowest_layer_socket() const {
    return stream_->next_layer();
}


/**
 * @brief [Actor] 执行所有待发送数据的写操作。
 */
boost::asio::awaitable<void> Http2Connection::do_write() {
    try {
        SPDLOG_TRACE("H2 [{}] do_write: session_want_write = {}, session_want_read = {}", id_,
                     nghttp2_session_want_write(session_), nghttp2_session_want_read(session_));

        // 只要 nghttp2 引擎有数据待发送，就循环写入。
        // 这是一个关键的循环，确保所有缓冲的数据都被清空。
        while (session_ && nghttp2_session_want_write(session_)) {
            const uint8_t* data = nullptr;
            // 从 nghttp2 获取待发送数据块的指针和长度（零拷贝）。
            const ssize_t len = nghttp2_session_mem_send(session_, &data);
            if (len <= 0) break;

            // 异步写入网络。co_await 提供了天然的背压机制：如果网络拥塞，
            // 协程会在这里暂停，直到数据发送完成，从而不会过度缓冲数据。
            auto [ec, _] = co_await asio::async_write(*stream_, asio::buffer(data, len), asio::as_tuple(asio::use_awaitable));
            if (ec) {
                if (!is_closing_) is_closing_ = true; // 标记连接为关闭状态
                break;
            }
        }
    } catch (...) {
        if (!is_closing_) {
            is_closing_ = true;
            // 重新抛出异常，让 actor_loop 来捕获并进行统一的清理
            throw;
        }
    }
}


/**
 * @brief [Actor] 核心 Actor 协程，管理此连接的整个生命周期。
 *
 * 这是一个单一的、常驻的协程，它在一个循环中融合了多种异步事件的处理：
 * 1. 阶段一：执行 HTTP/2 握手（SETTINGS 帧交换），并内置超时。
 * 2. 阶段二：使用 `operator||` 并发地等待网络数据 (`async_read_some`) 和
 *    来自 `execute()` 的新请求 (`request_channel_.async_receive`)。
 *
 * 这种单一协程模型从根本上消除了对 `strand` 或 `mutex` 的需求，
 * 因为所有对共享状态（`session_`, `streams_` map 等）的访问都在这个协程中串行执行。
 */
/**
 * @brief [Actor] 核心 Actor 协程，管理此连接的整个生命周期。
 */
asio::awaitable<void> Http2Connection::actor_loop() {
    // 使用 Finally guard 确保无论如何（即使异常退出），握手信号 channel 都会被关闭，
    // 这样 run() 方法就不会永久阻塞。
    auto guard = Finally([&] {
        handshake_signal_.close();
    });

    // --- 阶段一: 握手 ---
    SPDLOG_DEBUG("[{}]-[{}]: 开始握手", id_, pool_key_);
    nghttp2_session_callbacks* callbacks;
    nghttp2_session_callbacks_new(&callbacks);
    auto cb_guard = Finally([&] { nghttp2_session_callbacks_del(callbacks); }); // 使用 RAII guard 确保 callbacks 结构被释放
    // 设置所有必要的 nghttp2 回调函数
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, &on_begin_headers_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, &on_header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, &on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, &on_stream_close_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, &on_frame_recv_callback);
    nghttp2_session_client_new(&session_, callbacks, this); // 创建客户端会话

    // 准备并提交客户端的 SETTINGS 帧
    constexpr nghttp2_settings_entry iv[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 65535}
    };
    nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv, std::size(iv));

    // 立即发送连接前言 (Magic string) 和 SETTINGS 帧
    co_await do_write();

    // 循环等待，直到收到服务器的 SETTINGS 帧，这标志着握手成功
    asio::steady_timer handshake_timer(co_await asio::this_coro::executor);
    bool ok = false;
    while (!is_closing_ && !ok) {
        handshake_timer.expires_after(std::chrono::seconds(3s));
        // 同时等待网络数据或握手超时
        auto result = co_await (
            stream_->async_read_some(asio::buffer(read_buffer_), asio::as_tuple(asio::use_awaitable)) ||
            handshake_timer.async_wait(asio::as_tuple(asio::use_awaitable))
        );

        if (result.index() == 1) {
            // 定时器先完成
            throw std::runtime_error("HTTP/2 握手超时");
        }

        handshake_timer.cancel_one(); // 读取成功，取消定时器

        auto [ec, n] = std::get<0>(result);
        if (ec) throw boost::system::system_error(ec, "握手期间读取失败");


        // 将收到的数据喂给 nghttp2 引擎
        if (nghttp2_session_mem_recv(session_, reinterpret_cast<const uint8_t*>(read_buffer_.data()), n) < 0) {
            throw std::runtime_error("处理接收到的握手数据失败");
        }

        // 处理数据后，nghttp2 可能需要发送 SETTINGS ACK，立即写出
        co_await do_write();

        // 检查是否已收到远程的 SETTINGS，以此作为握手成功的标志
        if (nghttp2_session_get_remote_settings(session_, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS)) {
            ok = true;
        }
    }

    handshake_completed_ = true;
    SPDLOG_TRACE("[{}]-[{}] 握手完成. Actor 循环开始.", id_, pool_key_);

    // 握手成功，解除 Finally guard，因为我们将手动关闭信号通道
    guard.disarm();
    // 手动关闭 channel，向等待的 run() 方法发送成功信号
    handshake_signal_.close();

    // --- 阶段二: 主事件循环 (融合模型) ---
    idle_timer_.expires_after(max_idle_time_); // 启动空闲超时
    while (!is_closing_ && stream_->lowest_layer().is_open()) {
        // 这是 Actor 模型的核心：使用 `operator||` 并发地等待三个事件源中的任何一个：
        // 1. 网络套接字可读。
        // 2. 有新的请求通过 channel 发来。
        // 3. 空闲定时器超时。
        auto result = co_await (
            stream_->async_read_some(asio::buffer(read_buffer_), asio::as_tuple(asio::use_awaitable)) ||
            actor_channel_.async_receive(asio::as_tuple(asio::use_awaitable)) ||
            idle_timer_.async_wait(asio::as_tuple(asio::use_awaitable))
        );



        if (result.index() == 0) {
            // 网络可读
            auto [ec, n] = std::get<0>(result);

            if (ec) {
                SPDLOG_TRACE("[{}]-[{}] 网络读取失败: {}", id_, pool_key_, ec.message());
                is_closing_ = true;
                break;
            }

            idle_timer_.expires_after(max_idle_time_); // 处理新请求也是活动，重置定时器

            SPDLOG_DEBUG("[{}]-[{}] 读取数据 {} byte", id_, pool_key_, n);

            // 将数据喂给 nghttp2
            if (nghttp2_session_mem_recv(session_, reinterpret_cast<const uint8_t*>(read_buffer_.data()), n) < 0) {
                SPDLOG_TRACE("[{}]-[{}] 处理网络数据失败，准备关闭连接.", id_, pool_key_);
                is_closing_ = true;
                break;
            }
        } else if (result.index() == 1) {
            // 收到 Actor 消息

            auto [ec, msg_variant] = std::get<1>(result);
            if (ec) {
                // channel 关闭或出错
                SPDLOG_DEBUG("[{}]-[{}]  请求通道关闭或错误: {}", id_, pool_key_, ec.message());
                is_closing_ = true;
                break;
            }

            idle_timer_.expires_after(max_idle_time_); // 处理新请求也是活动，重置定时器

            // 使用 std::visit 处理 variant 消息
            std::visit([&]<typename T0>(T0&& msg) {
                using T = std::decay_t<T0>;

                // 判断消息类型
                if constexpr (std::is_same_v<T, H2RequestMessage>) {
                    if (is_closing_ || remote_goaway_received_) {
                        msg.response_channel->try_send(asio::error::operation_aborted, HttpResponse{});
                        return;
                    }

                    update_last_used_time();

                    auto stream_ctx = std::make_unique<StreamContext>();
                    stream_ctx->response_channel = msg.response_channel;

                    std::vector<nghttp2_nv> nva;
                    prepare_headers(nva, msg.request, *stream_ctx);

                    nghttp2_data_provider provider{};
                    if (!msg.request.body().empty()) {
                        stream_ctx->request_body = std::move(msg.request.body());
                        provider.source.ptr = stream_ctx.get();
                        provider.read_callback = &read_request_body_callback;
                    }

                    int32_t stream_id = nghttp2_submit_request(session_, nullptr, nva.data(), nva.size(), provider.source.ptr ? &provider : nullptr, nullptr);
                    if (stream_id > 0) {
                        nghttp2_session_set_stream_user_data(session_, stream_id, stream_ctx.get());
                        streams_.emplace(stream_id, std::move(stream_ctx));
                        ++active_streams_;
                        SPDLOG_DEBUG("[{}]-[{}] 成功提交请求: stream_id = {}, target = {}，当前活跃streams数量 = {}", id_, pool_key_, stream_id, msg.request.target(), active_streams_.load());
                    } else {
                        SPDLOG_ERROR("[{}]-[{}] 提交请求失败，stream_id: {}", id_, pool_key_, stream_id);
                        msg.response_channel->try_send(boost::system::error_code(stream_id, boost::system::generic_category()), HttpResponse{});
                    }
                } else if constexpr (std::is_same_v<T, H2PingMessage>) {
                    bool success = false;
                    if (is_usable() && session_) {
                        if (nghttp2_submit_ping(session_, NGHTTP2_FLAG_NONE, nullptr) == 0) {
                            success = true;
                        }
                    }

                    msg.result_channel->try_send(boost::system::error_code{}, success);

                } else if constexpr (std::is_same_v<T, H2CloseMessage>) {
                    is_closing_ = true;
                    if (session_ && !remote_goaway_received_) {
                        nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE, nghttp2_session_get_last_proc_stream_id(session_), NGHTTP2_NO_ERROR, nullptr, 0);
                    }
                }
            }, msg_variant);
        } else { // result.index() == 2, 空闲超时

            SPDLOG_DEBUG("[{}]-[{}]: 空闲超时 ({}ms).", id_, pool_key_, max_idle_time_.count());
            is_closing_ = true;
        }

        // 在每次循环的末尾，统一处理所有待发送的数据（新请求、响应ACK等）
        co_await do_write();
        if (is_closing_ && active_streams_ == 0) {
            break; // 退出循环，触发关闭流程
        }
    }

    // --- 阶段三: 最终清理 ---
    SPDLOG_TRACE("[{}]-[{}] actor_loop 循环结束，开始最终清理.: active_streams = {}, is_closing = {}, remote_goaway_received = {}",id_, pool_key_, active_streams_.load(), is_closing_.load(), remote_goaway_received_.load());
    is_closing_ = true;
    actor_channel_.close();

    for (const auto& stream_ctx_ptr : streams_ | std::views::values) {
        if (stream_ctx_ptr && stream_ctx_ptr->response_channel) {
            stream_ctx_ptr->response_channel->try_send(asio::error::operation_aborted, HttpResponse{});
        }
    }
    streams_.clear();

    if (stream_ && stream_->lowest_layer().is_open()) {
        boost::system::error_code ec;
        co_await stream_->async_shutdown(asio::redirect_error(asio::use_awaitable, ec));
        stream_->lowest_layer().close(ec);
    }
}


// --- 回调函数 & 私有辅助函数 ---

void Http2Connection::prepare_headers(std::vector<nghttp2_nv>& nva, const HttpRequest& req, StreamContext& stream_ctx) {
    // --- 阶段一: 计算总大小和 header 数量 ---
    size_t total_size = 0;
    size_t header_count = 0;

    // 伪头部
    total_size += H2_METHOD_SV.length() + req.method_string().length();
    total_size += H2_SCHEME_SV.length() + 5; // "https"
    total_size += H2_AUTHORITY_SV.length() + req.at(http::field::host).length();
    std::string_view path_sv = req.target();
    if (path_sv.empty()) path_sv = "/";
    total_size += H2_PATH_SV.length() + path_sv.length();
    header_count += 4;


    // 常规头部,同时加了 name 和 value 的长度
    for (const auto& field : req) {
        http::field name_enum = field.name();
        if (name_enum == http::field::host || name_enum == http::field::connection ||
            name_enum == http::field::upgrade || name_enum == http::field::proxy_connection ||
            name_enum == http::field::transfer_encoding || name_enum == http::field::keep_alive) {
            continue;
        }
        // 这里累加的是原始 name 的长度，因为 to_lower_copy 不改变长度。这是安全的。
        total_size += field.name_string().length();
        total_size += field.value().length();
        header_count++;
    }

    // --- 阶段二: 一次性分配内存 ---
    stream_ctx.header_arena.resize(total_size);
    char* current_ptr = stream_ctx.header_arena.data();
    nva.reserve(header_count); //  使用计算出的 header_count ***

    // --- 阶段三: 填充 Arena 并构造 nghttp2_nv ---
    auto fill_nv = [&](const std::string_view name_sv, const std::string_view value_sv) {
        char* name_start = current_ptr;
        memcpy(name_start, name_sv.data(), name_sv.length());
        current_ptr += name_sv.length();

        char* value_start = current_ptr;
        memcpy(value_start, value_sv.data(), value_sv.length());
        current_ptr += value_sv.length();

        nva.push_back({
            reinterpret_cast<uint8_t*>(name_start),
            reinterpret_cast<uint8_t*>(value_start),
            name_sv.length(),
            value_sv.length(),
            NGHTTP2_NV_FLAG_NONE
        });
    };


    // 填充伪头部
    fill_nv(H2_METHOD_SV, req.method_string());
    fill_nv(H2_SCHEME_SV, "https"sv);
    fill_nv(H2_AUTHORITY_SV, req.at(http::field::host));
    fill_nv(H2_PATH_SV, path_sv);

    // 填充常规头部
    for (const auto& field : req) {
        if (const http::field name_enum = field.name();
            name_enum == http::field::host || name_enum == http::field::connection ||
            name_enum == http::field::upgrade || name_enum == http::field::proxy_connection ||
            name_enum == http::field::transfer_encoding || name_enum == http::field::keep_alive) {
            continue;
        }
        std::string lower_name = boost::algorithm::to_lower_copy(std::string(field.name_string()));
        fill_nv(lower_name, field.value());
    }
}

void Http2Connection::handle_stream_close(int32_t stream_id, const uint32_t error_code) {
    const auto it = streams_.find(stream_id);
    if (it == streams_.end()) return;

    // 移动上下文的所有权，然后从 map 中删除
    const auto stream_ctx_ptr = std::move(it->second);
    streams_.erase(it);

    --active_streams_; // 减少活跃流计数

    if (!stream_ctx_ptr->response_channel) return;

    if (error_code == NGHTTP2_NO_ERROR) {
        // 流正常关闭，发送最终构建好的响应
        stream_ctx_ptr->response_in_progress.prepare_payload();
        stream_ctx_ptr->response_channel->try_send(boost::system::error_code{}, std::move(stream_ctx_ptr->response_in_progress));
    } else {
        // 流因错误关闭，发送一个错误码
        SPDLOG_WARN("stream {} 因错误码关闭: {}", stream_id, nghttp2_strerror(error_code));
        stream_ctx_ptr->response_channel->try_send(boost::system::error_code(static_cast<int>(error_code), boost::system::generic_category()), HttpResponse{});
    }
}

int Http2Connection::on_begin_headers_callback(nghttp2_session* session, const nghttp2_frame* frame,  void* user_data) {
    (void)user_data;
    if (frame->hd.type != NGHTTP2_HEADERS) return 0; // 只关心 HEADERS 帧
    // 获取此流的上下文
    const auto stream_ctx = static_cast<StreamContext*>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
    if (!stream_ctx) return 0;
    // 开始接收头部，初始化响应对象版本
    stream_ctx->response_in_progress.version(20);
    return 0;
}

int Http2Connection::on_header_callback(nghttp2_session* session, const nghttp2_frame* frame, const uint8_t* name, const size_t name_len, const uint8_t* value, const size_t value_len, uint8_t,  void* user_data) {
    (void)user_data;
    const auto stream_ctx = static_cast<StreamContext*>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));

    if (!stream_ctx) return 0;
    // 特殊处理 `:status` 伪头部
    if (const std::string_view key(reinterpret_cast<const char*>(name), name_len); key == ":status") {
        try {
            stream_ctx->response_in_progress.result(std::stoi(std::string(reinterpret_cast<const char*>(value), value_len)));
        } catch (...) {
            /* 忽略转换失败 */
        }
    } else if (!key.empty() && key[0] != ':') {
        // 忽略其他伪头部，只添加普通头部
        stream_ctx->response_in_progress.set(key, std::string_view(reinterpret_cast<const char*>(value), value_len));
    }
    return 0;
}

int Http2Connection::on_data_chunk_recv_callback(nghttp2_session* session, uint8_t flags, const int32_t stream_id, const uint8_t* data, const size_t len, void* user_data) {
    (void)user_data;
    const auto stream_ctx = static_cast<StreamContext*>(nghttp2_session_get_stream_user_data(session, stream_id));
    if (!stream_ctx) return 0;
    // 将接收到的数据块追加到响应体中
    stream_ctx->response_in_progress.body().append(reinterpret_cast<const char*>(data), len);
    return 0;
}

int Http2Connection::on_stream_close_callback(nghttp2_session*, const int32_t stream_id, const uint32_t error_code, void* user_data) {
    const auto self = static_cast<Http2Connection*>(user_data);
    // 在单协程 Actor 模型下，回调函数总是被 actor_loop 间接调用，所以可以直接调用成员函数，无需 post 到 strand。
    self->handle_stream_close(stream_id, error_code);
    return 0; // 返回0表示成功
}

int Http2Connection::on_frame_recv_callback(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
    const auto self = static_cast<Http2Connection*>(user_data);

    // 检查 PING 响应帧，用于连接活性检查
    if (frame->hd.type == NGHTTP2_PING && (frame->hd.flags & NGHTTP2_FLAG_ACK)) {
        SPDLOG_DEBUG("[{}]-[{}] 收到PING帧.", self->id_, self->pool_key_);
    }

    // 处理服务器主动关闭连接的 GOAWAY 帧
    if (frame->hd.type == NGHTTP2_GOAWAY) {
        SPDLOG_WARN("[{}]-[{}] 收到 GOAWAY 帧, 错误码: {}", self->id_, self->pool_key_, frame->goaway.error_code);
        self->remote_goaway_received_ = true;
        self->is_closing_ = true; // 标记连接为关闭状态
    }

    // 处理服务器发送的 SETTINGS 帧，更新本地的认知
    if (frame->hd.type == NGHTTP2_SETTINGS && (frame->hd.flags & NGHTTP2_FLAG_ACK) == 0) {
        uint32_t max_streams = nghttp2_session_get_remote_settings(
            self->session_, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS
        );
        // 如果服务器通告的最大并发流数小于我们的配置，则以服务器为准
        if (max_streams != NGHTTP2_INITIAL_MAX_CONCURRENT_STREAMS && self->max_concurrent_streams_ > max_streams) {
            self->max_concurrent_streams_ = max_streams;
            SPDLOG_DEBUG("[{}]-[{}]: 服务器更新 max concurrent streams 为 {}", self->id_, self->pool_key_, max_streams);
        }
    }
    return 0;
}

ssize_t Http2Connection::read_request_body_callback(nghttp2_session* session, const int32_t stream_id, uint8_t* buf, const size_t length, uint32_t* data_flags, nghttp2_data_source* source, void*) {
    // 当 nghttp2 准备发送请求体时，会调用此回调来“拉取”数据
    const auto stream_ctx = static_cast<StreamContext*>(nghttp2_session_get_stream_user_data(session, stream_id));
    if (!stream_ctx) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    const size_t remaining = stream_ctx->request_body.size() - stream_ctx->request_body_offset;
    const size_t n = std::min(length, remaining); // 计算本次可以复制的数据量
    if (n > 0) {
        memcpy(buf, stream_ctx->request_body.data() + stream_ctx->request_body_offset, n);
        stream_ctx->request_body_offset += n;
    }
    // 如果所有数据都已读取，设置 EOF 标志
    if (stream_ctx->request_body_offset == stream_ctx->request_body.size()) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return n;
}
