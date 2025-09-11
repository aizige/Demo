//
// Created by Aiziboy on 2025/7/16.
//

#include "h2c_session.hpp"
#include <spdlog/spdlog.h>          // 日志库
#include <cstring>                  // 用于 C 风格字符串操作，如 memcpy
#include <boost/asio/experimental/awaitable_operators.hpp> // Asio 实验性功能，提供了协程操作符，如 ||
#include <chrono>                   // C++ 时间库
#include "http/request_context.hpp"
#include "utils/finally.hpp"
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>

// 将 C++14 的时间字面量（如 30s）引入当前作用域
using namespace std::literals::chrono_literals;
namespace http = boost::beast::http;

// 辅助函数: Base64URL 解码 (HTTP2-Settings 使用的是 base64url)
// nghttp2 需要原始的二进制设置数据
std::string base64url_decode(std::string_view encoded) {
    std::string b64_str{encoded};
    boost::replace_all(b64_str, "-", "+");
    boost::replace_all(b64_str, "_", "/");

    // 补齐 '=' 以符合标准 Base64 解码要求
    switch (b64_str.length() % 4) {
        case 2: b64_str += "==";
            break;
        case 3: b64_str += "=";
            break;
        default: break;
    }

    try {
        using namespace boost::archive::iterators;
        using It = transform_width<binary_from_base64<std::string::const_iterator>, 8, 6>;
        return std::string{It(b64_str.begin()), It(b64_str.end())};
    } catch (const std::exception &e) {
        SPDLOG_WARN("Base64URL decoding failed: {}", e.what());
        return "";
    }
}


H2cSession::H2cSession(SocketPtr socket, Router &router)
    : socket_(std::move(socket)),
      router_(router),
      strand_(socket_->get_executor()),
      session_(nullptr),
      idle_timer_(strand_),
      write_trigger_(strand_),
      dispatch_channel_(strand_) {
    idle_timer_.expires_at(std::chrono::steady_clock::time_point::max());
    write_trigger_.expires_at(std::chrono::steady_clock::time_point::max());
}

H2cSession::~H2cSession() {
    if (session_) {
        nghttp2_session_del(session_);
    }
}

void H2cSession::init_session() {
    nghttp2_session_callbacks *callbacks;
    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    nghttp2_session_server_new(&session_, callbacks, this);
    nghttp2_session_callbacks_del(callbacks);
}

boost::asio::awaitable<void> H2cSession::start(
    boost::optional<HttpRequest> initial_request,
    boost::beast::flat_buffer &initial_data
) {
    using namespace boost::asio::experimental::awaitable_operators;
    try {
        // 并行运行所有三个核心循环
        co_await (session_loop(std::move(initial_request), initial_data) && dispatcher_loop() && writer_loop());
    } catch (const std::exception &e) {
        SPDLOG_DEBUG("H2C session ended: {}", e.what());
    }
}

boost::asio::awaitable<void> H2cSession::do_write() {
    try {
        if (session_ && nghttp2_session_want_write(session_)) {
            const uint8_t *data_ptr = nullptr;
            ssize_t len = nghttp2_session_mem_send(session_, &data_ptr);

            if (len < 0) {
                SPDLOG_ERROR("H2C Server: nghttp2_session_mem_send() failed: {}", nghttp2_strerror(len));
                if (socket_->is_open()) socket_->close();
                co_return;
            }

            if (len > 0) {
                co_await boost::asio::async_write(*socket_, boost::asio::buffer(data_ptr, len), boost::asio::use_awaitable);
            }
        }
    } catch (const std::exception &e) {
        SPDLOG_WARN("H2C Server do_write failed, closing socket: {}", e.what());
        if (socket_->is_open()) {
            boost::system::error_code ignored_ec;
            socket_->close(ignored_ec);
        }
    }
}

void H2cSession::schedule_write() {
    if (write_in_progress_ || !session_ || !socket_->is_open()) {
        return;
    }
    if (nghttp2_session_want_write(session_)) {
        write_trigger_.cancel_one();
    }
}

boost::asio::awaitable<void> H2cSession::writer_loop() {
    try {
        while (socket_->is_open()) {
            boost::system::error_code ec;
            co_await write_trigger_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

            if (ec && ec != boost::asio::error::operation_aborted) {
                break;
            }
            if (!socket_->is_open()) {
                break;
            }

            if (write_in_progress_) continue;
            write_in_progress_ = true;
            auto guard = Finally([this] { write_in_progress_ = false; });

            // **关键的 "清空队列" 循环**
            while (session_ && nghttp2_session_want_write(session_)) {
                co_await do_write();
                // 检查 do_write 是否因为错误关闭了套接字
                if (!socket_->is_open()) {
                    co_return;
                }
            }
        }
    } catch (const std::exception &e) {
        SPDLOG_WARN("H2C writer_loop ended with exception: {}", e.what());
        if (socket_->is_open()) {
            boost::system::error_code ignored_ec;
            socket_->close(ignored_ec);
        }
    }
}

boost::asio::awaitable<void> H2cSession::session_loop(boost::optional<HttpRequest> initial_request, boost::beast::flat_buffer &initial_data) {
    co_await boost::asio::dispatch(strand_, boost::asio::use_awaitable);
    init_session();

    if (initial_request) {
        // --- Upgrade 模式 ---
        SPDLOG_INFO("H2C session starting in Upgrade mode.");
        auto settings_header_it = initial_request->find("HTTP2-Settings");
        if (settings_header_it == initial_request->end()) {
            SPDLOG_WARN("H2C Upgrade request missing HTTP2-Settings header. Closing.");
            co_return;
        }

        std::string decoded_settings = base64url_decode(settings_header_it->value());
        if (decoded_settings.empty()) {
            SPDLOG_WARN("Failed to decode HTTP2-Settings. Closing.");
            co_return;
        }

        // 使用 nghttp2_session_upgrade 初始化会话，这会将初始请求内部转换为 stream 1
        int rv = nghttp2_session_upgrade(session_, (const uint8_t *) decoded_settings.data(), decoded_settings.size(), nullptr);
        if (rv != 0) {
            SPDLOG_ERROR("nghttp2_session_upgrade() failed: {}", nghttp2_strerror(rv));
            co_return;
        }
    } else {
        // --- Direct H2C 模式 ---
        SPDLOG_INFO("H2C session starting in Direct mode.");
    }

    // 提交服务器设置
    nghttp2_settings_entry iv[1] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}};
    nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv, 1);

    // 处理任何已预读的数据 (对于 Upgrade 模式，这是连接前言；对于 Direct 模式，也是)
    if (initial_data.size() > 0) {
        ssize_t rv = nghttp2_session_mem_recv(session_, (const uint8_t *) initial_data.data().data(), initial_data.size());
        if (rv < 0) {
            SPDLOG_ERROR("H2C initial nghttp2_session_mem_recv() failed: {}", nghttp2_strerror(rv));
            co_return;
        }
        initial_data.consume(initial_data.size());
    }

    // 触发写入，发送我们的 SETTINGS 帧
    schedule_write();

    constexpr auto timeout = 60s;
    idle_timer_.expires_after(timeout);

    std::array<char, 8192> buf{};
    using namespace boost::asio::experimental::awaitable_operators;

    while (socket_->is_open()) {
        auto read_op = socket_->async_read_some(
            boost::asio::buffer(buf),
            boost::asio::as_tuple(boost::asio::use_awaitable)
        );
        auto timer_op = idle_timer_.async_wait(
            boost::asio::as_tuple(boost::asio::use_awaitable)
        );

        auto result = co_await(std::move(read_op) || std::move(timer_op));

        if (result.index() == 1) {
            // 超时
            auto [ec_timer] = std::get<1>(result);
            if (!ec_timer) {
                SPDLOG_INFO("H2C connection idle timeout. Sending GOAWAY.");
                co_await graceful_shutdown(NGHTTP2_NO_ERROR);
            }
            dispatch_channel_.close();
            break;
        }

        auto [ec, n] = std::get<0>(result);
        if (ec) {
            if (ec != boost::asio::error::eof) {
                SPDLOG_WARN("H2C session_loop read error: {}", ec.message());
            }
            dispatch_channel_.close();
            break;
        }

        idle_timer_.expires_after(timeout);
        ssize_t rv = nghttp2_session_mem_recv(session_, (const uint8_t *) buf.data(), n);
        if (rv < 0) {
            SPDLOG_ERROR("H2C nghttp2_session_mem_recv() failed: {}", nghttp2_strerror(rv));
            dispatch_channel_.close();
            break;
        }
        schedule_write();
    }
}

boost::asio::awaitable<void> H2cSession::dispatcher_loop() {
    for (;;) {
        auto [ec, stream_id] = co_await dispatch_channel_.async_receive(boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec) break;
        boost::asio::co_spawn(strand_, dispatch(stream_id), boost::asio::detached);
    }
}

boost::asio::awaitable<void> H2cSession::dispatch(int32_t stream_id) {
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) co_return;

    StreamContext stream_ctx = std::move(it->second);
    streams_.erase(it);

    HttpRequest req;
    req.version(20);
    for (const auto &pair: stream_ctx.headers) {
        if (pair.first.empty()) continue;
        if (pair.first[0] == ':') {
            if (pair.first == ":method") req.method(http::string_to_verb(pair.second));
            else if (pair.first == ":path") req.target(pair.second);
            else if (pair.first == ":authority") req.set(http::field::host, pair.second);
        } else {
            req.set(pair.first, pair.second);
        }
    }
    req.body() = std::move(stream_ctx.body);
    req.prepare_payload();

    RouteMatch match = router_.dispatch(req.method(), req.target());
    RequestContext ctx(std::move(req), std::move(match.path_params));

    try {
        co_await match.handler(ctx);
    } catch (const std::exception &e) {
        SPDLOG_ERROR("Exception in H2C handler for [{}]: {}", ctx.request().target(), e.what());
        ctx.json(http::status::internal_server_error, "error Internal Server Error");
    }

    auto &resp = ctx.response();

    std::vector<nghttp2_nv> headers;
    std::string status_code = std::to_string(resp.result_int());
    headers.push_back({(uint8_t *) ":status", (uint8_t *) status_code.c_str(), 7, status_code.size(), NGHTTP2_NV_FLAG_NONE});
    for (const auto &field: resp) {
        if (field.name() == http::field::connection) continue;
        headers.push_back({(uint8_t *) field.name_string().data(), (uint8_t *) field.value().data(), field.name_string().length(), field.value().length(), NGHTTP2_NV_FLAG_NONE});
    }

    auto content = std::make_shared<std::string>(std::move(resp.body()));
    nghttp2_data_provider provider{};
    std::shared_ptr<ProviderPack> pack = nullptr;
    if (content && !content->empty()) {
        pack = std::make_shared<ProviderPack>();
        pack->content = content;
        provider.source.ptr = pack.get();
        provider.read_callback = [](nghttp2_session *, int32_t, uint8_t *buf, size_t len, uint32_t *flags, nghttp2_data_source *src, void *) -> ssize_t {
            auto *p = static_cast<ProviderPack *>(src->ptr);
            if (!p || !p->content) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
            size_t remaining = p->content->size() - p->offset;
            size_t n = std::min(len, remaining);
            if (n > 0) {
                memcpy(buf, p->content->data() + p->offset, n);
                p->offset += n;
            }
            if (p->offset == p->content->size()) { *flags |= NGHTTP2_DATA_FLAG_EOF; }
            return n;
        };
        provider_pack_[stream_id] = pack;
    }

    nghttp2_submit_response(session_, stream_id, headers.data(), headers.size(), (pack ? &provider : nullptr));
    schedule_write();
}

int H2cSession::on_frame_recv_callback(nghttp2_session *, const nghttp2_frame *frame, void *user_data) {
    auto *self = static_cast<H2cSession *>(user_data);
    int32_t sid = frame->hd.stream_id;
    if (sid != 0 && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        if (self->streams_.count(sid) > 0) {
            self->dispatch_channel_.try_send(boost::system::error_code{}, sid);
        }
    }
    return 0;
}

int H2cSession::on_header_callback(nghttp2_session *, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t, void *user_data) {
    auto *self = static_cast<H2cSession *>(user_data);
    int32_t sid = frame->hd.stream_id;
    if (sid == 0) return 0;
    if (self->streams_.find(sid) == self->streams_.end()) {
        self->streams_[sid] = {};
    }
    self->streams_.at(sid).headers.emplace_back(
        std::string((const char *) name, namelen),
        std::string((const char *) value, valuelen)
    );
    return 0;
}

int H2cSession::on_data_chunk_recv_callback(nghttp2_session *, uint8_t, int32_t stream_id, const uint8_t *data, size_t len, void *user_data) {
    auto *self = static_cast<H2cSession *>(user_data);
    if (auto it = self->streams_.find(stream_id); it != self->streams_.end()) {
        it->second.body.append((char *) data, len);
    }
    return 0;
}

int H2cSession::on_stream_close_callback(nghttp2_session *, int32_t stream_id, uint32_t, void *user_data) {
    auto *self = static_cast<H2cSession *>(user_data);
    boost::asio::post(self->strand_, [self, stream_id]() {
        self->provider_pack_.erase(stream_id);
    });
    return 0;
}

boost::asio::awaitable<void> H2cSession::graceful_shutdown(uint32_t error_code) {
    co_await boost::asio::dispatch(strand_, boost::asio::use_awaitable);
    co_await do_graceful_shutdown(error_code);
}

boost::asio::awaitable<void> H2cSession::do_graceful_shutdown(uint32_t error_code) {
    if (!session_ || !socket_->is_open()) {
        co_return;
    }

    SPDLOG_INFO("Initiating graceful shutdown for H2cSession with error_code={}", error_code);
    int32_t last_stream_id = nghttp2_session_get_last_proc_stream_id(session_);
    int rv = nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE, last_stream_id, error_code, nullptr, 0);
    if (rv != 0) {
        SPDLOG_ERROR("nghttp2_submit_goaway() failed: {}", nghttp2_strerror(rv));
    }
    schedule_write();
}
