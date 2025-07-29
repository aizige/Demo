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
#include "utils/finally.hpp"
#include "utils/utils.hpp"

namespace asio = boost::asio;
using namespace boost::asio::experimental::awaitable_operators; // For operator||


Http2Connection::Http2Connection(StreamPtr stream, std::string pool_key)
    : stream_(std::move(stream)),
      pool_key_(std::move(pool_key)),
      id_(generate_simple_uuid()),
      strand_(stream_->get_executor()),
      request_channel_(stream_->get_executor(), 256),
      last_used_timestamp_seconds_(steady_clock_seconds_since_epoch()),
      write_trigger_(stream_->get_executor()) {
    write_trigger_.expires_at(asio::steady_timer::time_point::max()); // Initially, no writes scheduled
    SPDLOG_DEBUG("Http2Connection (Actor) [{}] created.", id_);
}

Http2Connection::~Http2Connection() {
    if (session_) {
        nghttp2_session_del(session_);
        session_ = nullptr;
    }
    SPDLOG_DEBUG("Http2Connection (Actor) [{}] destroyed.", id_);
}


std::string Http2Connection::generate_simple_uuid() {
    static std::atomic<uint64_t> counter = 0;
    return "h2-client-conn-" + std::to_string(++counter);
}


// --- IConnection Interface & Public Methods ---

void Http2Connection::run() {
    asio::co_spawn(
        strand_,
        [self = shared_from_this()]() {
            return self->run_internal();
        },
        asio::detached // 我们不直接等待这个任务，它在后台独立运行
    );
}
// 这是真正的后台主协程
boost::asio::awaitable<void> Http2Connection::run_internal() {
    using namespace boost::asio::experimental::awaitable_operators;
    try {
        co_await (actor_loop() && writer_loop());
    } catch (const std::exception &e) {
        // 只有当两个循环都因为异常或被取消而退出时，才会到这里
        if (e.what() != std::string_view("The coro was cancelled")) { // 避免打印正常取消的日志
            SPDLOG_WARN("H2 Connection [{}] main loops exited with exception: {}", id_, e.what());
        }
    }
    // 两个循环都结束后，统一在这里做最后的清理
    co_await close();
}
boost::asio::awaitable<HttpResponse> Http2Connection::execute(HttpRequest request) {
    update_last_used_time();
    auto ex = co_await asio::this_coro::executor;

    if (is_closing_) {
        throw boost::system::system_error(asio::error::connection_aborted, "Connection is closing");
    }

    auto response_channel = std::make_shared<asio::experimental::channel<void(boost::system::error_code, HttpResponse)> >(ex, 1);

    H2RequestMessage msg{std::move(request), response_channel};

    auto [ec_send] = co_await request_channel_.async_send({}, std::move(msg), asio::as_tuple(asio::use_awaitable));
    if (ec_send) {
        throw boost::system::system_error(ec_send, "Failed to send request to H2 connection actor");
    }

    auto [ec, response] = co_await response_channel->async_receive(asio::as_tuple(asio::use_awaitable));

    if (ec) {
        throw boost::system::system_error(ec, "H2 stream execution");
    }
    SPDLOG_DEBUG("Http2Connection [{}] response received with status {}.", id_, response.result_int());
    co_return response;
}

bool Http2Connection::is_usable() const {
    return !is_closing_ && stream_ && stream_->lowest_layer().is_open() && session_ != nullptr;
}

boost::asio::awaitable<void> Http2Connection::close() {
    co_await asio::post(strand_, asio::use_awaitable);
    if (close_called_.exchange(true)) co_return;

    is_closing_ = true;
    request_channel_.close();

    auto streams_to_cancel = std::move(streams_);
    for (auto &[id, stream_ctx]: streams_to_cancel) {
        if (stream_ctx && stream_ctx->response_channel) {
            stream_ctx->response_channel->try_send(asio::error::operation_aborted, HttpResponse{});
        }
    }

    if (session_) {
        nghttp2_session_del(session_);
        session_ = nullptr;
    }

    if (stream_ && stream_->lowest_layer().is_open()) {
        boost::system::error_code ec;
        co_await stream_->async_shutdown(asio::redirect_error(asio::use_awaitable, ec));
        stream_->lowest_layer().shutdown(tcp::socket::shutdown_both, ec);
        stream_->lowest_layer().close(ec);
    }
}

boost::asio::awaitable<bool> Http2Connection::ping() {
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);
    if (!is_usable() || get_active_streams() > 0) {
        co_return true;
    }

    nghttp2_submit_ping(session_, NGHTTP2_FLAG_NONE, nullptr);

    schedule_write();



    update_last_used_time();
    co_return true;
}

void Http2Connection::update_last_used_time() {
    last_used_timestamp_seconds_ = steady_clock_seconds_since_epoch();
}

size_t Http2Connection::get_active_streams() const {
    return active_streams_.load();
}

tcp::socket &Http2Connection::lowest_layer_socket() {
    return stream_->next_layer();
}


// --- Actor Core Implementation ---
// In h2_connection.cpp
asio::awaitable<void> Http2Connection::writer_loop() {
    try {
        while (true) { // 循环直到被 run_internal() 取消
            boost::system::error_code ec;
            co_await write_trigger_.async_wait(asio::redirect_error(asio::use_awaitable, ec));

            if (ec && ec != asio::error::operation_aborted) {
                throw boost::system::system_error(ec, "writer_loop wait failed");
            }
            if (is_closing_) co_return; // 如果在等待时被关闭，则退出

            if (write_in_progress_) continue;
            write_in_progress_ = true;
            auto guard = Finally([this] { write_in_progress_ = false; });


            // 在一次 writer_loop 的唤醒中，循环调用 do_write，直到 nghttp2 的缓冲区被清空
            while (session_ && nghttp2_session_want_write(session_)) {
                co_await do_write();

                // 检查 do_write 是否因为错误而将连接标记为关闭
                if (is_closing_) {
                    co_return;
                }
            }
        }
    } catch (const boost::system::system_error& e) {
        if (e.code() == asio::error::operation_aborted) {
            SPDLOG_DEBUG("Http2Connection [{}] writer_loop was canceled.", id_);
            co_return;
        }
        SPDLOG_WARN("Http2Connection [{}] writer_loop unhandled system_error: {}", id_, e.what());
        throw;
    } catch (const std::exception &e) {
        SPDLOG_WARN("H2 Connection [{}] writer_loop unhandled exception: {}", id_, e.what());
        throw;
    }
}

// In h2_connection.cpp
void Http2Connection::schedule_write() {
    // This function must be called from within the strand.
    if (write_in_progress_ || is_closing_) {
        return;
    }

    // If there's data to write, trigger the writer_loop immediately.
    if (session_ && nghttp2_session_want_write(session_)) {
        write_trigger_.cancel_one();
    }
}

asio::awaitable<void> Http2Connection::actor_loop() {
    try {
        // --- STAGE 1: HANDSHAKE ---
        SPDLOG_INFO("H2 Connection [{}]: Starting handshake.", id_);

        // 1.1. 初始化 nghttp2 session
        nghttp2_session_callbacks *callbacks;
        nghttp2_session_callbacks_new(&callbacks);
        auto cb_guard = Finally([&] { nghttp2_session_callbacks_del(callbacks); });
        // ... (设置所有回调函数) ...
        nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, &on_begin_headers_callback);
        nghttp2_session_callbacks_set_on_header_callback(callbacks, &on_header_callback);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, &on_data_chunk_recv_callback);
        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, &on_stream_close_callback);
        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, &on_frame_recv_callback);
        nghttp2_session_client_new(&session_, callbacks, this);

        // 1.2. 发送客户端的 SETTINGS 帧
        nghttp2_settings_entry iv[] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}, {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, (1 << 24)}};
        nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv, std::size(iv));

        SPDLOG_TRACE("H2 [{}]: After submit_settings, want_write={}", id_, nghttp2_session_want_write(session_));

        // 1.3. 触发 writer_loop 第一次发送数据（连接前言 + SETTINGS）
        schedule_write(); // 触发 writer_loop 发送数据

        // 1.3. 循环读，直到收到服务器的 SETTINGS 帧
        asio::steady_timer handshake_timer(co_await asio::this_coro::executor);
        handshake_timer.expires_after(std::chrono::seconds(10));

        bool handshake_ok = false;
        while (!is_closing_ && !handshake_ok) {
            // 我们只等待读操作或超时
            auto result = co_await (
                stream_->async_read_some(asio::buffer(read_buffer_), asio::as_tuple(asio::use_awaitable)) ||
                handshake_timer.async_wait(asio::as_tuple(asio::use_awaitable))
            );


            if (result.index() == 1) {
                // 超时
                auto [ec_timer] = std::get<1>(result);
                if (ec_timer == asio::error::operation_aborted) {
                    SPDLOG_TRACE("H2 [{}]: Handshake timer cancelled, continuing.", id_);
                    continue; // 定时器被取消是正常的
                }
                SPDLOG_ERROR("H2 [{}]: Handshake timer expired after 10s.", id_);
                throw std::runtime_error("HTTP/2 handshake timed out.");
            }

            // --- 收到了网络数据 ---
            auto [ec, n] = std::get<0>(result);
            if (ec) {
                SPDLOG_ERROR("H2 [{}]: Handshake read failed: {}", id_, ec.message());
                throw boost::system::system_error(ec, "Handshake read failed");
            }

            SPDLOG_TRACE("H2 [{}]: Read {} bytes from network during handshake.", id_, n);

            // 直接处理这次读到的 n 字节数据喂给 nghttp2
            ssize_t rv = nghttp2_session_mem_recv(session_, reinterpret_cast<const uint8_t *>(read_buffer_.data()), n);
            if (rv < 0) {
                SPDLOG_ERROR("H2 [{}]: nghttp2_session_mem_recv failed: {}", id_, nghttp2_strerror(rv));
                throw std::runtime_error("Failed to process received handshake data.");
            }

            SPDLOG_TRACE("H2 [{}]: Fed {} bytes to nghttp2.", id_, n);

            // --- [日志] 检查 nghttp2 在收到数据后的状态 ---
            // 它可能会想发送 SETTINGS ACK
            SPDLOG_TRACE("H2 [{}]: After mem_recv, want_read={}, want_write={}",
                         id_,
                         nghttp2_session_want_read(session_),
                         nghttp2_session_want_write(session_));


            // 1.7. 检查握手是否完成
            // 握手成功的标志是：我们收到了服务器的 SETTINGS 帧。
            // 我们可以通过检查远程设置是否被应用来判断。
            // nghttp2_session_get_remote_settings 在收到 SETTINGS 帧之前会返回-1。
            int32_t remote_max_streams = nghttp2_session_get_remote_settings(session_, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
            if (remote_max_streams >= 0) {
                SPDLOG_INFO("H2 [{}]: Received server SETTINGS frame. Max concurrent streams: {}", id_, remote_max_streams);
                handshake_ok = true;
                handshake_timer.cancel(); // 握手成功，取消超时
            } else {
                SPDLOG_TRACE("H2 [{}]: Server SETTINGS frame not yet received (remote_max_streams={}).", id_, remote_max_streams);
            }

            // 确保发送队列中的任何数据（比如 SETTINGS 的 ACK）被发送出去
           schedule_write();
        }

        handshake_completed_ = true;
        SPDLOG_INFO("H2 Connection [{}] handshake complete. Actor loop starting.", id_);

        // --- STAGE 2: MAIN EVENT LOOP ---

        schedule_write();   // 先调用一次 schedule_write()，以防握手后有数据待发
        while (!is_closing_ && session_) {
            auto result = co_await (
                stream_->async_read_some(asio::buffer(read_buffer_), asio::as_tuple(asio::use_awaitable)) ||
                request_channel_.async_receive(asio::as_tuple(asio::use_awaitable))
            );

            update_last_used_time();

            if (result.index() == 0) {
                // Network is readable
                auto [ec, n] = std::get<0>(result);
                if (ec) {
                    is_closing_ = true;
                    break;
                }

                if (nghttp2_session_mem_recv(session_, reinterpret_cast<const uint8_t *>(read_buffer_.data()), n) < 0) {
                    is_closing_ = true;

                }
            } else {
                // New request message received
                auto [ec, msg] = std::get<1>(result);
                if (ec) {
                    is_closing_ = true;
                    break;
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
                    stream_ctx->response_channel->try_send(boost::system::error_code(NGHTTP2_ERR_INVALID_ARGUMENT, boost::system::generic_category()), HttpResponse{});
                } else {
                    streams_.emplace(stream_id, std::move(stream_ctx));
                    ++active_streams_; // **计数增加**
                }
            }

            schedule_write();
        }
    } catch (const std::exception &e) {
        SPDLOG_WARN("H2 Connection [{}] actor_loop ended: {}", id_, e.what());
        // 同样，抛出异常让 run() 来处理
        throw;
    }

}


boost::asio::awaitable<void> Http2Connection::do_read() {
    if (is_closing_) co_return;
    auto [ec, n] = co_await stream_->async_read_some(asio::buffer(read_buffer_), asio::as_tuple(asio::use_awaitable));
    if (ec) {
        is_closing_ = true;
        co_return;
    }
    if (session_) {
        if (nghttp2_session_mem_recv(session_, reinterpret_cast<const uint8_t *>(read_buffer_.data()), n) < 0) {
            throw std::runtime_error("nghttp2 mem recv failed");
            // is_closing_ = true;
        }
    }
}

boost::asio::awaitable<void> Http2Connection::do_write() {
    try {
        if (session_ && nghttp2_session_want_write(session_)) {
            const uint8_t *data = nullptr;
            ssize_t len = nghttp2_session_mem_send(session_, &data);

            if (len < 0) {
                SPDLOG_ERROR("H2 Client [{}]: nghttp2_session_mem_send() failed: {}", id_, nghttp2_strerror(len));
                is_closing_ = true;
                co_return;
            }

            if (len > 0) {
                // co_await 发送这一小块数据。
                // 这提供了真正的背压。
                auto [ec, _] = co_await asio::async_write(*stream_, asio::buffer(data, len), asio::as_tuple(asio::use_awaitable));
                if (ec) {
                    is_closing_ = true;
                }
            }
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("H2 Client [{}] do_write exception: {}", id_, e.what());
        is_closing_ = true;
    }
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
    storage.emplace_back(req.method_string()); // e.g., "GET"
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


// --- Callbacks ---
void Http2Connection::handle_stream_close(int32_t stream_id, uint32_t error_code) {
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return;

    auto stream_ctx_ptr = std::move(it->second);
    streams_.erase(it);

    --active_streams_; // **计数减少**

    if (error_code == NGHTTP2_NO_ERROR) {
        stream_ctx_ptr->response_in_progress.prepare_payload();
        stream_ctx_ptr->response_channel->try_send(boost::system::error_code{}, std::move(stream_ctx_ptr->response_in_progress));
    } else {
        SPDLOG_WARN("Stream {} closed with error code: {}", stream_id, error_code);
        stream_ctx_ptr->response_channel->try_send(boost::system::error_code(error_code, boost::system::generic_category()), HttpResponse{});
    }
}

int Http2Connection::on_stream_close_callback(nghttp2_session *, int32_t stream_id, uint32_t error_code, void *user_data) {
    auto self = static_cast<Http2Connection *>(user_data);
    asio::post(self->strand_, [self, stream_id, error_code]() {
        self->handle_stream_close(stream_id, error_code);
    });
    return 0;
}

int Http2Connection::on_frame_recv_callback(nghttp2_session *, const nghttp2_frame *frame, void *user_data) {
    auto self = static_cast<Http2Connection *>(user_data);
    if (frame->hd.type == NGHTTP2_SETTINGS && (frame->hd.flags & NGHTTP2_FLAG_ACK) == 0) {
        // 从服务器的 SETTINGS 帧中获取正确的最大并发流数
        uint32_t max_streams = nghttp2_session_get_remote_settings(
            self->session_, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS
        );

        // nghttp2 规范说默认值是无限，但通常实现会给个值，我也设置了一个合理的默认上限
        if (max_streams > 0) {
            // 有些实现可能返回 0 或未指定
            self->max_concurrent_streams_ = max_streams;
            SPDLOG_INFO("H2 [{}]: Server updated max_concurrent_streams to {}", self->id_, max_streams);
        }
    }
    if (frame->hd.type == NGHTTP2_GOAWAY) {
        SPDLOG_WARN("GOAWAY received on [{}], error code: {}", self->id_, frame->goaway.error_code);
        self->is_closing_ = true;
    }
    return 0;
}

int Http2Connection::on_begin_headers_callback(nghttp2_session *session, const nghttp2_frame *frame, void *user_data) {
    (void) user_data;
    if (frame->hd.type != NGHTTP2_HEADERS) return 0;

    auto stream_ctx = static_cast<StreamContext *>(
        nghttp2_session_get_stream_user_data(session, frame->hd.stream_id)
    );
    if (!stream_ctx) return 0;

    stream_ctx->response_in_progress.version(20);
    return 0;
}

int Http2Connection::on_header_callback(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data) {
    // 这个回调不需要 Http2Connection 的 user_data，因为我们可以从 stream 中获取所有东西
    // auto self = static_cast<Http2Connection*>(user_data);
    (void) user_data;
    auto stream_ctx = static_cast<StreamContext *>(
        nghttp2_session_get_stream_user_data(session, frame->hd.stream_id)
    );
    if (!stream_ctx) return 0;

    std::string_view key((const char *) name, namelen);
    if (key == ":status") {
        try {
            stream_ctx->response_in_progress.result(std::stoi(std::string((const char *) value, valuelen)));
        } catch (...) {
            /* ignore */
        }
    } else if (!key.empty() && key[0] != ':') {
        stream_ctx->response_in_progress.set(key, std::string_view((const char *) value, valuelen));
    }
    return 0;
}

int Http2Connection::on_data_chunk_recv_callback(nghttp2_session *session, uint8_t, int32_t stream_id, const uint8_t *data, size_t len, void *user_data) {
    (void) user_data;
    auto stream_ctx = static_cast<StreamContext *>(
        nghttp2_session_get_stream_user_data(session, stream_id)
    );
    if (!stream_ctx) return 0;

    stream_ctx->response_in_progress.body().append((const char *) data, len);
    return 0;
}

ssize_t Http2Connection::read_request_body_callback(nghttp2_session *, int32_t, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *) {
    //  source->ptr 获取
    auto stream_ctx = static_cast<StreamContext *>(source->ptr);
    if (!stream_ctx) {
        SPDLOG_ERROR("read_request_body_callback failed: StreamContext is null.");
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    size_t remaining = stream_ctx->request_body.size() - stream_ctx->request_body_offset;
    size_t n = std::min(length, remaining);
    if (n > 0) {
        memcpy(buf, stream_ctx->request_body.data() + stream_ctx->request_body_offset, n);
        stream_ctx->request_body_offset += n;
        SPDLOG_DEBUG("Sent {} bytes of request body, offset: {}/{}", n, stream_ctx->request_body_offset, stream_ctx->request_body.size());
    }
    if (stream_ctx->request_body_offset == stream_ctx->request_body.size()) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        SPDLOG_DEBUG("Request body fully sent.");
    }
    return n;
}
