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
#include <boost/asio/experimental/parallel_group.hpp> // <-- 引入 parallel_group
#include "http/request_context.hpp"
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

#include "error/my_error.hpp"
#include "utils/finally.hpp"
#include "utils/utils.hpp"

namespace asio = boost::asio;
using namespace boost::asio::experimental::awaitable_operators; // For operator||


Http2Connection::Http2Connection(StreamPtr stream, std::string pool_key)
    : stream_(std::move(stream)),
      pool_key_(std::move(pool_key)),
      id_(generate_simple_uuid()),
      request_channel_(stream_->get_executor(), 256), // 邮箱，带有 256 个消息的缓冲区
      last_used_timestamp_seconds_(steady_clock_seconds_since_epoch()) {
    SPDLOG_DEBUG("H2连接 [{}] 已创建.", id_);
}

Http2Connection::~Http2Connection() {
    if (session_) {
        nghttp2_session_del(session_);
        session_ = nullptr;
    }
    SPDLOG_DEBUG("H2连接 [{}] 已销毁.", id_);
}



std::string Http2Connection::generate_simple_uuid() {
    static std::atomic<uint64_t> counter = 0;
    return "h2-client-conn-" + std::to_string(++counter);
}


// --- IConnection 接口 & 公共方法 ---

void Http2Connection::run() {
    asio::co_spawn(
        stream_->get_executor(),
        [self = shared_from_this()]() -> asio::awaitable<void> {
            try {
                // 直接运行并等待唯一的 actor_loop 协程。
                co_await self->actor_loop();
            } catch (const std::exception& e) {
                // 捕获所有从 actor_loop 中冒泡出来的异常。
                if (std::string_view(e.what()) != "The coro was cancelled") {
                     SPDLOG_WARN("H2 连接 [{}] 的主循环因异常退出: {}", self->id_, e.what());
                }
            }
            // 无论 actor_loop 是正常结束还是因异常退出，都执行最终的清理工作。
            co_await self->close();
        },
        asio::detached
    );
}

// 发起一个 HTTP/2 请求，并等待响应（带超时保护）
boost::asio::awaitable<HttpResponse> Http2Connection::execute(HttpRequest request) {

    // 更新连接的最后使用时间（用于连接池管理）
    update_last_used_time();

    // 获取当前协程的执行器（通常是 ioc_）
    auto ex = co_await asio::this_coro::executor;

    // 如果连接已关闭或已收到 GOAWAY，拒绝请求
    if (is_closing_ || remote_goaway_received_) {
        SPDLOG_WARN("[{}] execute: 连接已关闭或收到 GOAWAY，拒绝请求", id_);
        throw boost::system::system_error(asio::error::connection_aborted, "连接已关闭或收到 GOAWAY，拒绝请求");
    }

    // 为本次请求创建一个响应通道（用于异步接收响应）
    auto response_channel = std::make_shared<asio::experimental::channel<void(boost::system::error_code, HttpResponse)>>(ex, 1);

    // 构造请求消息，包含请求内容和响应通道
    H2RequestMessage msg{std::move(request), response_channel};

    // 将当前协程切换到 actor 的执行器（确保 async_send 在 actor 的上下文中执行）
    co_await asio::dispatch(stream_->get_executor(), asio::use_awaitable);

    // 设置发送请求的超时定时器（300 毫秒）TODO:时间在配置文件中配置
    asio::steady_timer send_timer(ex);
    send_timer.expires_after(std::chrono::milliseconds(300ms));

    // 并发等待：请求发送 或 超时
    auto send_result = co_await (
        request_channel_.async_send({}, std::move(msg), asio::as_tuple(asio::use_awaitable)) ||
        send_timer.async_wait(asio::as_tuple(asio::use_awaitable))
    );

    // 如果是定时器触发（即超时），则抛出异常
    if (send_result.index() == 1) {
        SPDLOG_ERROR("[{}] execute: async_send 超时", id_);
        throw boost::system::system_error(asio::error::connection_reset, "发送请求超时，服务端可能已经关闭或挂起");
    }

    // 获取 async_send 的结果
    auto [ec_send] = std::get<0>(send_result);
    if (ec_send) {
        SPDLOG_ERROR("[{}] execute: async_send 错误: {}", id_, ec_send.message());
        throw boost::system::system_error(ec_send, "发送请求失败");
    }


    // 设置接收响应的超时定时器（3000 毫秒）TODO:时间在配置文件中配置
    asio::steady_timer recv_timer(ex);
    recv_timer.expires_after(std::chrono::milliseconds(3000ms));

    // 并发等待：响应到达 或 超时
    auto recv_result = co_await (
        response_channel->async_receive(asio::as_tuple(asio::use_awaitable)) ||
        recv_timer.async_wait(asio::as_tuple(asio::use_awaitable))
    );

    // 如果是定时器触发（即响应超时），则标记连接为关闭并抛出异常
    if (recv_result.index() == 1) {
        SPDLOG_ERROR(" [{}] : 响应超时", id_);

        // 如果当前没有其他活跃 stream，说明连接可能挂起
        if (active_streams_ <= 0) {
            is_closing_ = true;
            SPDLOG_WARN("[{}], active_streams_ = {} : 无活跃 stream 且响应超时，标记连接关闭", id_,active_streams_.load());
        }
        throw boost::system::system_error(my_error::h2::receive_timeout, "接收响应超时");
    }

    // 获取响应结果
    auto [ec, response] = std::get<0>(recv_result);
    if (ec) {
        SPDLOG_WARN("H2 [{}] execute: 响应错误: {}", id_, ec.message());
        throw boost::system::system_error(ec, "响应失败");
    }

    // 正常收到响应，记录日志并返回
    SPDLOG_INFO("H2 [{}] execute: 收到响应，状态码 {}, 当前活动的steam = {}", id_, response.result_int(),active_streams_.load());
    co_return response;
}

bool Http2Connection::is_usable() const {
    // 一个连接只有在握手成功后才真正可用。

    if (is_closing_ || remote_goaway_received_  || !handshake_completed_ || !stream_ || session_ == nullptr) return false;
    // socket 仍“开放”不代表 TLS/HTTP2 状态可复用
    if (!stream_->lowest_layer().is_open()) return false;

    return true;
}

boost::asio::awaitable<void> Http2Connection::close() {
    // 这个函数现在可以从任何协程安全调用，因为它是最后的清理步骤。
    if (close_called_.exchange(true)) co_return;
    SPDLOG_INFO("Http2Connection (Actor) [{}] 正在关闭连接.", id_);
    is_closing_ = true;
    request_channel_.close();

    // 取消所有仍在等待响应的流
    auto streams_to_cancel = std::move(streams_);
    for (auto& [id, stream_ctx] : streams_to_cancel) {
        if (stream_ctx && stream_ctx->response_channel) {
            stream_ctx->response_channel->try_send(asio::error::operation_aborted, HttpResponse{});
        }
    }

    if (stream_ && stream_->lowest_layer().is_open()) {
        boost::system::error_code ec;
        co_await stream_->async_shutdown(asio::redirect_error(asio::use_awaitable, ec));
        boost::system::error_code error_code = stream_->lowest_layer().shutdown(tcp::socket::shutdown_both, ec);
        boost::system::error_code close = stream_->lowest_layer().close(ec);
    }
    SPDLOG_INFO("Http2Connection (Actor) [{}] 连接关闭完成.", id_);
}

boost::asio::awaitable<bool> Http2Connection::ping() {
    if (!is_usable() || get_active_streams() > 0) {
        co_return true;
    }
    nghttp2_submit_ping(session_, NGHTTP2_FLAG_NONE, nullptr);
    co_await do_write(); // 直接调用 do_write 发送 PING 帧
    update_last_used_time();
    co_return true;
}

void Http2Connection::update_last_used_time() {
    last_used_timestamp_seconds_ = steady_clock_seconds_since_epoch();
}

size_t Http2Connection::get_active_streams() const {
    return active_streams_.load();
}

tcp::socket& Http2Connection::lowest_layer_socket() {
    return stream_->next_layer();
}


// --- Actor 核心 (融合模型) ---

boost::asio::awaitable<void> Http2Connection::do_write() {
    try {
        SPDLOG_TRACE("H2 [{}] do_write: session_want_write = {}, session_want_read = {}", id_,
             nghttp2_session_want_write(session_), nghttp2_session_want_read(session_));

        // 只要 nghttp2 有数据想发送，就循环写入
        while (session_ && nghttp2_session_want_write(session_)) {
            const uint8_t *data = nullptr;
            ssize_t len = nghttp2_session_mem_send(session_, &data);
            if (len <= 0) break;

            // co_await 提供了天然的背压
            auto [ec, _] = co_await asio::async_write(*stream_, asio::buffer(data, len), asio::as_tuple(asio::use_awaitable));
            if (ec) {
                if (!is_closing_) is_closing_ = true;
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

asio::awaitable<void> Http2Connection::actor_loop() {
    // --- 阶段一: 握手 ---
    SPDLOG_INFO("H2 连接 [{}]: 开始握手.", id_);

    nghttp2_session_callbacks *callbacks;
    nghttp2_session_callbacks_new(&callbacks);
    auto cb_guard = Finally([&] { nghttp2_session_callbacks_del(callbacks); });
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, &on_begin_headers_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, &on_header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, &on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, &on_stream_close_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, &on_frame_recv_callback);
    nghttp2_session_client_new(&session_, callbacks, this);

    nghttp2_settings_entry iv[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 65535} // 使用一个标准的、保守的值
    };
    nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv, std::size(iv));

    // 立即发送连接前言和 SETTINGS 帧
    co_await do_write();

    asio::steady_timer handshake_timer(co_await asio::this_coro::executor);
    handshake_timer.expires_after(std::chrono::seconds(3s));

    bool handshake_ok = false;
    while (!is_closing_ && !handshake_ok) {
        // 并发地等待网络数据或超时
        auto result = co_await (
            stream_->async_read_some(asio::buffer(read_buffer_), asio::as_tuple(asio::use_awaitable)) ||
            handshake_timer.async_wait(asio::as_tuple(asio::use_awaitable))
        );

        if (result.index() == 1) {
            SPDLOG_ERROR("H2 连接 [{}] 握手阶段超时.", id_);
            throw std::runtime_error("HTTP/2 握手超时.");
        }

        auto [ec, n] = std::get<0>(result);
        if (ec) {
            SPDLOG_ERROR("H2 连接 [{}] 握手阶段读取失败: {}", id_, ec.message());
            throw boost::system::system_error(ec, "握手期间读取失败");
        }

        SPDLOG_DEBUG("H2 连接 [{}] 握手阶段收到 {} 字节数据.", id_, n);

        if (nghttp2_session_mem_recv(session_, reinterpret_cast<const uint8_t *>(read_buffer_.data()), n) < 0) {
            SPDLOG_ERROR("H2 连接 [{}] 握手数据处理失败.", id_);
            throw std::runtime_error("处理接收到的握手数据失败.");
        }

        // 收到数据后，可能会有 ACK 需要发送，立即处理写操作
        co_await do_write();

        // 检查是否收到了服务器的 SETTINGS 帧作为握手成功的标志
        int32_t remote_val = nghttp2_session_get_remote_settings(session_, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
        if (remote_val >= 0) {
            handshake_ok = true;
            SPDLOG_INFO("H2 连接 [{}] 握手成功，远端 max_concurrent_streams = {}", id_, remote_val);
        }
    }

    handshake_completed_ = true; // 标记握手已完成
    SPDLOG_INFO("H2 连接 [{}] 握手完成. Actor 循环开始.", id_);

    // --- 阶段二: 主事件循环 (融合模型) ---
    while (!is_closing_ && session_) {
        // 并发地等待网络数据 或 应用程序发来的新请求
        auto result = co_await (
            stream_->async_read_some(asio::buffer(read_buffer_), asio::as_tuple(asio::use_awaitable)) ||
            request_channel_.async_receive(asio::as_tuple(asio::use_awaitable))
        );

        update_last_used_time();

        SPDLOG_TRACE("Actor 循环 [{}]: result.index() = {}", id_, result.index());

        if (result.index() == 0) { // 网络可读
            auto [ec, n] = std::get<0>(result);

            if (ec) {
                SPDLOG_WARN("H2 连接 [{}] 网络读取失败: {}", id_, ec.message());
                is_closing_ = true;
                break;
            }

            SPDLOG_TRACE("H2 连接 [{}] 网络读取 {} 字节.", id_, n);

            if (nghttp2_session_mem_recv(session_, reinterpret_cast<const uint8_t *>(read_buffer_.data()), n) < 0) {
                SPDLOG_ERROR("H2 连接 [{}] 处理网络数据失败，准备关闭连接.", id_);
                is_closing_ = true;
                break;
            }
        } else { // 收到新请求
            auto [ec, msg] = std::get<1>(result);
            if (ec) {
                SPDLOG_WARN("H2 连接 [{}] 请求通道关闭或错误: {}", id_, ec.message());
                is_closing_ = true;
                break;
            }

            SPDLOG_DEBUG("H2 连接 [{}] 收到新请求: {}", id_, msg.request.target());

            if (is_closing_ || remote_goaway_received_) {
                SPDLOG_WARN("H2 连接 [{}] 已收到 GOAWAY 或正在关闭，拒绝新请求.", id_);
                msg.response_channel->try_send(asio::error::operation_aborted, HttpResponse{});
                continue;
            }

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

            int32_t stream_id = nghttp2_submit_request(session_, nullptr, nva.data(), nva.size(), provider.source.ptr ? &provider : nullptr, stream_ctx.get());
            if (stream_id < 0) {
                SPDLOG_ERROR("H2 连接 [{}] 提交请求失败: stream_id = {}", id_, stream_id);
                stream_ctx->response_channel->try_send(boost::system::error_code(NGHTTP2_ERR_INVALID_ARGUMENT, boost::system::generic_category()), HttpResponse{});
            } else {
                SPDLOG_INFO("H2 连接 [{}] 成功提交请求: stream_id = {}, target = {}", id_, stream_id, msg.request.target());
                streams_.emplace(stream_id, std::move(stream_ctx));
                ++active_streams_;
                SPDLOG_DEBUG("H2 连接 [{}] 当前活跃流数量 = {}", id_, active_streams_.load());
            }
        }

        // 在每次循环的末尾，统一处理所有待发送的数据
        SPDLOG_DEBUG("H2 [{}] actor_loop end: active_streams = {}, is_closing = {}, remote_goaway_received = {}",
             id_, active_streams_.load(), is_closing_.load(), remote_goaway_received_.load());

        co_await do_write();
    }
}


// --- 回调函数 & 私有辅助函数 ---

void Http2Connection::handle_stream_close(int32_t stream_id, uint32_t error_code) {
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return;

    auto stream_ctx_ptr = std::move(it->second);
    streams_.erase(it);

    --active_streams_;

    if (!stream_ctx_ptr->response_channel) return;

    if (error_code == NGHTTP2_NO_ERROR) {
        stream_ctx_ptr->response_in_progress.prepare_payload();
        stream_ctx_ptr->response_channel->try_send(boost::system::error_code{}, std::move(stream_ctx_ptr->response_in_progress));
    } else {
        SPDLOG_WARN("流 {} 因错误码关闭: {}", stream_id, nghttp2_strerror(error_code));
        stream_ctx_ptr->response_channel->try_send(boost::system::error_code(error_code, boost::system::generic_category()), HttpResponse{});
    }
}

int Http2Connection::on_stream_close_callback(nghttp2_session *, int32_t stream_id, uint32_t error_code, void *user_data) {
    auto self = static_cast<Http2Connection *>(user_data);
    // 在单协程模型下，可以直接调用，因为不会有数据竞争。
    self->handle_stream_close(stream_id, error_code);
    return 0;
}

int Http2Connection::on_frame_recv_callback(nghttp2_session *, const nghttp2_frame *frame, void *user_data) {
    auto self = static_cast<Http2Connection *>(user_data);

    if (frame->hd.type == NGHTTP2_GOAWAY) {
        SPDLOG_WARN("在连接 [{}] 上收到 GOAWAY 帧, 错误码: {}", self->id_, frame->goaway.error_code);
        self->remote_goaway_received_ = true;
        self->is_closing_ = true;

        // 立即拒绝新流，取消未完成流
        for (auto& [id, ctx] : self->streams_) {
            if (ctx && ctx->response_channel) {
                ctx->response_channel->try_send(asio::error::operation_aborted, HttpResponse{});
            }
        }
        self->streams_.clear();
        self->active_streams_ = 0;

        // 主动向对端发送本地 GOAWAY，并驱动写出
        nghttp2_session_terminate_session(self->session_, NGHTTP2_NO_ERROR);
    }

    if (frame->hd.type == NGHTTP2_SETTINGS && (frame->hd.flags & NGHTTP2_FLAG_ACK) == 0) {
        uint32_t max_streams = nghttp2_session_get_remote_settings(
            self->session_, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS
        );
        // nghttp2 v1.1.0 之后，初始值不再是无限，而是 100
        if (max_streams != NGHTTP2_INITIAL_MAX_CONCURRENT_STREAMS) {
            self->max_concurrent_streams_ = max_streams;
            SPDLOG_INFO("H2 [{}]: 服务器更新 max_concurrent_streams 为 {}", self->id_, max_streams);
        }
    }
    return 0;
}

int Http2Connection::on_begin_headers_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data) {
    (void) user_data;
    if (frame->hd.type != NGHTTP2_HEADERS) return 0;
    auto stream_ctx = static_cast<StreamContext *>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
    if (!stream_ctx) return 0;
    stream_ctx->response_in_progress.version(20);
    return 0;
}

int Http2Connection::on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t, void *user_data) {
    (void) user_data;
    auto stream_ctx = static_cast<StreamContext *>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
    if (!stream_ctx) return 0;
    std::string_view key((const char *) name, namelen);
    if (key == ":status") {
        try {
            stream_ctx->response_in_progress.result(std::stoi(std::string((const char *) value, valuelen)));
        } catch (...) { /* 忽略转换失败 */ }
    } else if (!key.empty() && key[0] != ':') {
        stream_ctx->response_in_progress.set(key, std::string_view((const char *) value, valuelen));
    }
    return 0;
}

int Http2Connection::on_data_chunk_recv_callback(nghttp2_session *session, uint8_t, int32_t stream_id, const uint8_t *data, size_t len, void *user_data) {
    (void) user_data;
    auto stream_ctx = static_cast<StreamContext *>(nghttp2_session_get_stream_user_data(session, stream_id));
    if (!stream_ctx) return 0;
    stream_ctx->response_in_progress.body().append((const char *) data, len);
    return 0;
}

ssize_t Http2Connection::read_request_body_callback(nghttp2_session *, int32_t, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *) {
    auto stream_ctx = static_cast<StreamContext *>(source->ptr);
    if (!stream_ctx) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    size_t remaining = stream_ctx->request_body.size() - stream_ctx->request_body_offset;
    size_t n = std::min(length, remaining);
    if (n > 0) {
        memcpy(buf, stream_ctx->request_body.data() + stream_ctx->request_body_offset, n);
        stream_ctx->request_body_offset += n;
    }
    if (stream_ctx->request_body_offset == stream_ctx->request_body.size()) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return n;
}

void Http2Connection::prepare_headers(std::vector<nghttp2_nv> &nva, const HttpRequest &req, StreamContext &stream_ctx) {
    size_t header_count = 0;
    for (const auto &field: req) {
        if (field.name() != http::field::host && field.name() != http::field::connection) {
            header_count++;
        } else {
            SPDLOG_DEBUG("Filtered header: {}", field.name_string());
        }
    }

    auto &storage = stream_ctx.header_storage;
    // 预估大小，减少 vector 重分配的可能。每个头需要2个string(k,v)，伪头部需要1个string。
    storage.reserve((header_count * 2) + 4);

    // --- 伪头部 ---
    // 对于伪头部，它们的名称是常量字符串，可以直接使用。值则需要存储。
    storage.emplace_back(req.method_string());
    nva.push_back({(uint8_t *) ":method", (uint8_t *) storage.back().data(), 7, storage.back().length(), NGHTTP2_NV_FLAG_NONE});

    storage.emplace_back("https");
    nva.push_back({(uint8_t *) ":scheme", (uint8_t *) storage.back().data(), 7, 5, NGHTTP2_NV_FLAG_NONE});

    storage.emplace_back(req.at(http::field::host));
    nva.push_back({(uint8_t *) ":authority", (uint8_t *) storage.back().data(), 10, storage.back().length(), NGHTTP2_NV_FLAG_NONE});

    std::string_view path_sv = req.target();
    if (path_sv.empty()) path_sv = "/";
    storage.emplace_back(path_sv);
    nva.push_back({(uint8_t *) ":path", (uint8_t *) storage.back().data(), 5, storage.back().length(), NGHTTP2_NV_FLAG_NONE});


    // --- 常规头部 ---
    for (const auto &field: req) {
        http::field name_enum = field.name();
        // 过滤头部
        if (name_enum == http::field::host || name_enum == http::field::connection ||
            name_enum == http::field::upgrade || name_enum == http::field::proxy_connection ||
            name_enum == http::field::transfer_encoding || name_enum == http::field::keep_alive) {
            continue;
        }


        // 1. 存储小写的 header name
        storage.emplace_back(boost::algorithm::to_lower_copy(std::string(field.name_string())));

        // 2. 存储 header value
        storage.emplace_back(field.value());

        // 3. 使用指向 storage 末尾两个元素的指针
        const auto &name_str = *(storage.end() - 2);
        const auto &value_str = *(storage.end() - 1);

        nva.push_back({
            (uint8_t *) name_str.data(),
            (uint8_t *) value_str.data(),
            name_str.length(),
            value_str.length(),
            NGHTTP2_NV_FLAG_NONE // **移除 NO_COPY 标志**
        });
    }
}





