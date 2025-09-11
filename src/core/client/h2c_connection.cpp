//
// Created by ubuntu on 2025/7/21.
//

#include "h2c_connection.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/impl/co_spawn.hpp>
#include <spdlog/spdlog.h>
#include <cstring>                  // 用于 C 风格字符串操作，如 memcpy
#include <boost/asio/experimental/awaitable_operators.hpp> // Asio 实验性功能，提供了协程操作符，如 ||
#include <chrono>                   // C++ 时间库
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/parallel_group.hpp> // <-- 引入 parallel_group
#include "http/request_context.hpp"
#include <nghttp2/nghttp2.h>
#include <boost/asio/experimental/promise.hpp>
#include <boost/beast/ssl.hpp>
#include <atomic>
#include <deque>
#include <boost/asio/co_spawn.hpp>
#include <ranges>
#include <boost/algorithm/string/case_conv.hpp>

#include "utils/finally.hpp"
#include "utils/utils.hpp"


namespace asio = boost::asio;
using namespace boost::asio::experimental::awaitable_operators;


// --- Constructor, Destructor, and Helpers ---
Http2cConnection::Http2cConnection(StreamPtr stream, std::string pool_key)
    : stream_(std::move(stream)),
      pool_key_(std::move(pool_key)),
      id_(generate_simple_uuid()),
      request_channel_(stream_->get_executor(), 256),
      last_used_timestamp_seconds_(steady_clock_seconds_since_epoch()) {
    SPDLOG_DEBUG("Http2cConnection (Actor) [{}] 已创建.", id_);
}

Http2cConnection::~Http2cConnection() {
    if (session_) {
        nghttp2_session_del(session_);
        session_ = nullptr;
    }
    SPDLOG_DEBUG("Http2cConnection (Actor) [{}] 已销毁.", id_);
}

std::string Http2cConnection::generate_simple_uuid() {
    static std::atomic<uint64_t> counter = 0;
    return "h2c-client-conn-" + std::to_string(++counter);
}


// --- IConnection 接口 & 公共方法 ---

void Http2cConnection::run() {
    asio::co_spawn(
        stream_->get_executor(),
        [self = shared_from_this()]() -> asio::awaitable<void> {
            try {
                co_await self->actor_loop();
            } catch (const std::exception& e) {
                if (std::string_view(e.what()) != "The coro was cancelled") {
                     SPDLOG_WARN("H2C 连接 [{}] 的主循环因异常退出: {}", self->id_, e.what());
                }
            }
            co_await self->close();
        },
        asio::detached
    );
}

boost::asio::awaitable<HttpResponse> Http2cConnection::execute(HttpRequest request) {
    update_last_used_time();
    auto ex = co_await asio::this_coro::executor;

    if (is_closing_) {
        throw boost::system::system_error(asio::error::connection_aborted, "连接正在关闭中");
    }

    auto response_channel = std::make_shared<asio::experimental::channel<void(boost::system::error_code, HttpResponse)>>(ex, 1);
    H2RequestMessage msg{std::move(request), response_channel};

    auto [ec_send] = co_await request_channel_.async_send({}, std::move(msg), asio::as_tuple(asio::use_awaitable));
    if (ec_send) {
        throw boost::system::system_error(ec_send, "发送请求到 H2C 连接 actor 失败");
    }

    auto [ec, response] = co_await response_channel->async_receive(asio::as_tuple(asio::use_awaitable));
    if (ec) {
        throw boost::system::system_error(ec, "H2C 流执行出错");
    }

    SPDLOG_DEBUG("Http2cConnection [{}] 收到响应，状态码 {}.", id_, response.result_int());
    co_return response;
}

bool Http2cConnection::is_usable() const {
    return !is_closing_ && handshake_completed_ && stream_ && stream_->socket().is_open() && session_ != nullptr;
}

boost::asio::awaitable<void> Http2cConnection::close() {
    if (close_called_.exchange(true)) co_return;

    is_closing_ = true;
    request_channel_.close();

    auto streams_to_cancel = std::move(streams_);
    for (auto& [id, stream_ctx] : streams_to_cancel) {
        if (stream_ctx && stream_ctx->response_channel) {
            stream_ctx->response_channel->try_send(asio::error::operation_aborted, HttpResponse{});
        }
    }

    if (stream_ && stream_->socket().is_open()) {
        boost::system::error_code ec;
        stream_->socket().shutdown(tcp::socket::shutdown_both, ec);
        stream_->socket().close(ec);
    }
    co_return; // co_awaitable<void> 需要一个返回
}

boost::asio::awaitable<bool> Http2cConnection::ping() {
    if (!is_usable() || get_active_streams() > 0) {
        co_return true;
    }
    nghttp2_submit_ping(session_, NGHTTP2_FLAG_NONE, nullptr);
    co_await do_write();
    update_last_used_time();
    co_return true;
}

void Http2cConnection::update_last_used_time() {
    last_used_timestamp_seconds_ = steady_clock_seconds_since_epoch();
}

size_t Http2cConnection::get_active_streams() const {
    return active_streams_.load();
}

tcp::socket& Http2cConnection::lowest_layer_socket() {
    return stream_->socket();
}


// --- Actor 核心 (融合模型) ---

boost::asio::awaitable<void> Http2cConnection::do_write() {
    try {
        while (session_ && nghttp2_session_want_write(session_)) {
            const uint8_t *data = nullptr;
            ssize_t len = nghttp2_session_mem_send(session_, &data);
            if (len <= 0) break;

            auto [ec, _] = co_await asio::async_write(*stream_, asio::buffer(data, len), asio::as_tuple(asio::use_awaitable));
            if (ec) {
                if (!is_closing_) is_closing_ = true;
                break;
            }
        }
    } catch (...) {
        if (!is_closing_) {
            is_closing_ = true;
            throw;
        }
    }
}

asio::awaitable<void> Http2cConnection::actor_loop() {
    // --- 阶段一: 握手 (对于H2C，就是发送初始帧) ---
    SPDLOG_INFO("H2C 连接 [{}]: 启动会话.", id_);

    nghttp2_session_callbacks *callbacks;
    nghttp2_session_callbacks_new(&callbacks);
    auto cb_guard = Finally([&] { nghttp2_session_callbacks_del(callbacks); });
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, &on_begin_headers_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, &on_header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, &on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, &on_stream_close_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, &on_frame_recv_callback);
    nghttp2_session_client_new(&session_, callbacks, this);

    nghttp2_settings_entry iv[] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}};
    nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv, std::size(iv));

    // 立即发送连接前言和 SETTINGS
    co_await do_write();

    handshake_completed_ = true; // H2C 没有复杂的握手，发送完就可以认为好了
    SPDLOG_INFO("H2C 连接 [{}] 启动完成. Actor 循环开始.", id_);

    // --- 阶段二: 主事件循环 ---
    while (!is_closing_ && session_) {
        // 并发地等待网络数据 或 应用程序发来的新请求
        auto result = co_await (
            stream_->async_read_some(asio::buffer(read_buffer_), asio::as_tuple(asio::use_awaitable)) ||
            request_channel_.async_receive(asio::as_tuple(asio::use_awaitable))
        );

        update_last_used_time();

        if (result.index() == 0) { // 网络可读
            auto [ec, n] = std::get<0>(result);
            if (ec) { is_closing_ = true; break; }
            if (nghttp2_session_mem_recv(session_, reinterpret_cast<const uint8_t *>(read_buffer_.data()), n) < 0) {
                is_closing_ = true;
                break;
            }
        } else { // 收到新请求
            auto [ec, msg] = std::get<1>(result);
            if (ec) { is_closing_ = true; break; }

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
                stream_ctx->response_channel->try_send(boost::system::error_code(NGHTTP2_ERR_INVALID_ARGUMENT, boost::system::generic_category()), HttpResponse{});
            } else {
                streams_.emplace(stream_id, std::move(stream_ctx));
                ++active_streams_;
            }
        }

        // 在每次循环的末尾，统一处理所有待发送的数据
        co_await do_write();
    }
}


// --- 回调函数 & 私有辅助函数 ---

void Http2cConnection::handle_stream_close(int32_t stream_id, uint32_t error_code) {
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

int Http2cConnection::on_stream_close_callback(nghttp2_session *, int32_t stream_id, uint32_t error_code, void *user_data) {
    auto self = static_cast<Http2cConnection *>(user_data);
    self->handle_stream_close(stream_id, error_code);
    return 0;
}

// ... (prepare_headers 和其他回调函数与 H2 版本完全一致，只需将 Http2Connection 替换为 Http2cConnection) ...

int Http2cConnection::on_frame_recv_callback(nghttp2_session *, const nghttp2_frame *frame, void *user_data) {
    auto self = static_cast<Http2cConnection *>(user_data);
    if (frame->hd.type == NGHTTP2_SETTINGS && (frame->hd.flags & NGHTTP2_FLAG_ACK) == 0) {
        uint32_t max_streams = nghttp2_session_get_remote_settings(
            self->session_, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS
        );
        if (max_streams != NGHTTP2_INITIAL_MAX_CONCURRENT_STREAMS) {
            self->max_concurrent_streams_ = max_streams;
            SPDLOG_INFO("H2C [{}]: 服务器更新 max_concurrent_streams 为 {}", self->id_, max_streams);
        }
    }
    if (frame->hd.type == NGHTTP2_GOAWAY) {
        SPDLOG_WARN("在连接 [{}] 上收到 GOAWAY 帧, 错误码: {}", self->id_, frame->goaway.error_code);
        self->is_closing_ = true;
    }
    return 0;
}

int Http2cConnection::on_begin_headers_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data) {
    (void) user_data;
    if (frame->hd.type != NGHTTP2_HEADERS) return 0;
    auto stream_ctx = static_cast<StreamContext *>(nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
    if (!stream_ctx) return 0;
    stream_ctx->response_in_progress.version(20);
    return 0;
}

int Http2cConnection::on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t, void *user_data) {
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

int Http2cConnection::on_data_chunk_recv_callback(nghttp2_session *session, uint8_t, int32_t stream_id, const uint8_t *data, size_t len, void *user_data) {
    (void) user_data;
    auto stream_ctx = static_cast<StreamContext *>(nghttp2_session_get_stream_user_data(session, stream_id));
    if (!stream_ctx) return 0;
    stream_ctx->response_in_progress.body().append((const char *) data, len);
    return 0;
}

ssize_t Http2cConnection::read_request_body_callback(nghttp2_session *, int32_t, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *) {
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

void Http2cConnection::prepare_headers(std::vector<nghttp2_nv> &nva, const HttpRequest &req, StreamContext &stream_ctx) {
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

    storage.emplace_back(req.method_string());
    nva.push_back({(uint8_t *) ":method", (uint8_t *) storage.back().data(), 7, storage.back().length(), NGHTTP2_NV_FLAG_NONE});

    // **[关键区别]** H2C 使用 http scheme
    storage.emplace_back("http");
    nva.push_back({(uint8_t *) ":scheme", (uint8_t *) storage.back().data(), 7, 4, NGHTTP2_NV_FLAG_NONE});

    storage.emplace_back(req.at(http::field::host));
    nva.push_back({(uint8_t *) ":authority", (uint8_t *) storage.back().data(), 10, storage.back().length(), NGHTTP2_NV_FLAG_NONE});
    std::string_view path_sv = req.target();
    if (path_sv.empty()) path_sv = "/";
    storage.emplace_back(path_sv);
    nva.push_back({(uint8_t *) ":path", (uint8_t *) storage.back().data(), 5, storage.back().length(), NGHTTP2_NV_FLAG_NONE});

    for (const auto &field: req) {
        http::field name_enum = field.name();
        // H2C 同样需要过滤这些头部
        if (name_enum == http::field::host || name_enum == http::field::connection ||
            name_enum == http::field::upgrade || name_enum == http::field::proxy_connection ||
            name_enum == http::field::transfer_encoding || name_enum == http::field::keep_alive) {
            continue;
        }
        storage.emplace_back(boost::algorithm::to_lower_copy(std::string(field.name_string())));

        storage.emplace_back(field.value());
        const auto &name_str = *(storage.end() - 2);
        const auto &value_str = *(storage.end() - 1);
        nva.push_back({(uint8_t *) name_str.data(), (uint8_t *) value_str.data(), name_str.length(), value_str.length(), NGHTTP2_NV_FLAG_NONE});
    }
}













