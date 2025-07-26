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
#include <nghttp2/n>

#include <boost/system/system_error.hpp>

#include <vector>
#include <string>

#include <boost/beast/ssl.hpp>
#include <atomic>
#include <deque>
#include <boost/asio/co_spawn.hpp>
#include <ranges>
#include <boost/algorithm/string/case_conv.hpp>
#include "utils/utils.hpp"

struct finally {
    std::function<void()> func;
    ~finally() { func(); }
};

// --- Constructor, Destructor, and Helpers ---
Http2Connection::Http2Connection(StreamPtr stream, std::string pool_key)
    : stream_(std::move(stream)),
      pool_key_(std::move(pool_key)),
      id_(generate_simple_uuid()),
      strand_(stream_->get_executor()),
      last_used_timestamp_seconds_(steady_clock_seconds_since_epoch()),
      execute_mutex_(stream_->get_executor(), 1) {
    SPDLOG_DEBUG("Http2Connection [{}] created.", id_);
    // 构造时，立即向 channel 发送一个“令牌”
    execute_mutex_.try_send(std::error_code{});
}

Http2Connection::~Http2Connection() {
    if (session_) {
        nghttp2_session_del(session_);
        session_ = nullptr; // 关键: 置为 nullptr
    }
    SPDLOG_DEBUG("Http2Connection [{}] destroyed.", id_);
}


std::string Http2Connection::generate_simple_uuid() {
    static std::atomic<uint64_t> counter = 0;
    return "h2-client-conn-" + std::to_string(++counter);
}


void Http2Connection::update_last_used_time() {
    last_used_timestamp_seconds_ = steady_clock_seconds_since_epoch();
}

boost::asio::awaitable<bool> Http2Connection::ping() {
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);
    if (!is_usable()) {
        co_return false;
    }
    if (active_streams_.load() > 0) {
        // 已经有业务在用了，那肯定是活的，直接返回成功，不要去添乱
        co_return true;
    }
    nghttp2_submit_ping(session_, NGHTTP2_FLAG_NONE, nullptr);

    try {
        co_await do_write();
    } catch (const std::exception &e) {
        spdlog::warn("H2 Connection [{}]: Ping failed during write: {}", id_, e.what());
        is_closing_ = true;
        co_return false;
    }

    co_return true;
}

// in Http2Connection.cpp

boost::asio::awaitable<void> Http2Connection::start() {
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);

    // 获取 executor
    auto ex = co_await boost::asio::this_coro::executor;

    // 创建一个定时器，它将作为我们的超时和成功信号
    boost::asio::steady_timer handshake_timer(ex);
    handshake_timer.expires_after(std::chrono::seconds(10));

    // 在后台启动握手 I/O 循环。
    // 注意 lambda 捕获了定时器的引用。

    boost::asio::co_spawn(strand_, [self = shared_from_this(), &handshake_timer]() -> boost::asio::awaitable<void> {
        try {
            // 在 co_spawn 的新协程中初始化 session，以防万一
            self->init_nghttp2_session();

            // 1. 提交客户端的 SETTINGS 帧

            nghttp2_settings_entry iv[] = {
                {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
                {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, (1 << 24)} // 设置 16MB 窗口
            };
            nghttp2_submit_settings(self->session_, NGHTTP2_FLAG_NONE, iv, std::size(iv));

            // 2. 启动握手 I/O 循环
            while (!self->handshake_completed_ && self->is_usable()) {
                if (nghttp2_session_want_read(self->session_) == 0 && nghttp2_session_want_write(self->session_) == 0) {
                    // 让出执行权，避免空转
                    co_await boost::asio::post(self->strand_, boost::asio::use_awaitable);
                    continue;
                }
                if (nghttp2_session_want_write(self->session_)) {
                    co_await self->do_write();
                }
                if (nghttp2_session_want_read(self->session_)) {
                    co_await self->do_read();
                }
            }

            // 3. 检查握手结果
            if (self->handshake_completed_) {
                // **成功**: 取消定时器，这将让 start() 中的 await 操作立即返回一个 `operation_canceled` 错误。
                SPDLOG_DEBUG("Handshake I/O completed successfully, canceling timer.");
                handshake_timer.cancel();
            } else {
                // **失败**: 不取消定时器，让它自然超时。
                SPDLOG_WARN("Handshake I/O loop exited but handshake not complete.");
            }
        } catch (const std::exception &e) {
            SPDLOG_ERROR("Exception in handshake coroutine for [{}]: {}", self->id(), e.what());
            // 出现异常，让定时器自然超时。
        }
    }, boost::asio::detached);


    // **等待信号**: co_await 等待定时器。
    // - 如果定时器被后台协程取消，async_wait会立即返回，并带有 ec == boost::asio::error::operation_aborted
    // - 如果定时器正常到期，async_wait会正常返回，ec 为 success
    boost::system::error_code ec;
    co_await handshake_timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

    // **判断结果**
    if (ec == boost::asio::error::operation_aborted) {
        // 这是我们期望的成功路径！
        SPDLOG_INFO("H2 Connection [{}]: Handshake complete. Ready for requests.", id_);

        // 握手已完成，现在启动真正的后台 I/O 循环
        co_spawn(strand_, [self = shared_from_this()]() -> boost::asio::awaitable<void> {
            try {
                co_await self->session_loop();
            } catch (...) {
                /* 忽略 */
            }
            SPDLOG_DEBUG("H2 Connection [{}] I/O loop finished.", self->id_);
            co_await self->close();
        }, boost::asio::detached);

        co_return; // 成功返回
    }

    // **关键修复 B**: 在失败路径上不再调用 co_await close()
    is_closing_ = true;
    throw std::runtime_error("HTTP/2 handshake timed out or failed.");
}


boost::asio::awaitable<HttpResponse> Http2Connection::execute(HttpRequest request) {
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);
    boost::asio::async_result<boost::asio::use_awaitable_t<>, void(std::tuple<boost::system::error_code, http::message<false, http::basic_string_body<char> > >)>::return_type response_future;
    int32_t stream_id;
    try {
        // **关键修复**: 在 execute 的最开始“获取锁”
        // co_await 从 channel 接收一个值。如果 channel 为空，它会挂起
        // 直到有值被送回。这实现了异步的上锁。
        co_await execute_mutex_.async_receive(boost::asio::use_awaitable);
        // RAII guard 来确保锁一定会被释放
        auto guard = finally([this]{
            // 在协程退出时，将“令牌”送回 channel，唤醒下一个等待者
            execute_mutex_.try_send(boost::system::error_code{});
        });




        update_last_used_time();

        if (!is_usable()) {
            throw boost::system::system_error(boost::asio::error::not_connected, "H2 connection is not usable");
        }

        auto ex = co_await boost::asio::this_coro::executor;
        auto stream_ctx_ptr = std::make_unique<StreamContext>(ex);

        response_future = stream_ctx_ptr->response_channel.async_receive(
            boost::asio::as_tuple(boost::asio::use_awaitable)
        );
        // **关键修复 1: 先获取 future**
        // 在 stream_ctx_ptr 的所有权转移之前，安全地获取 future。

        // 现在可以安全地准备和提交请求了
        std::vector<nghttp2_nv> nva;
        prepare_headers(nva, request, *stream_ctx_ptr);

        nghttp2_data_provider provider{};
        bool has_body = !request.body().empty();
        if (has_body) {
            stream_ctx_ptr->request_body = std::move(request.body());
            provider.source.ptr = stream_ctx_ptr.get();
            provider.read_callback = &Http2Connection::read_request_body_callback;
        }

        stream_id = nghttp2_submit_request(
            session_, nullptr, nva.data(), nva.size(),
            has_body ? &provider : nullptr,
            stream_ctx_ptr.get()
        );

        if (stream_id < 0) {
            // 如果提交失败，我们需要手动让 future 失败
            stream_ctx_ptr->response_channel.try_send(boost::asio::error::invalid_argument, HttpResponse{});
            throw std::runtime_error("submit_request failed: " + std::string(nghttp2_strerror(stream_id)));
        }else {
            // **在这里增加计数**
            ++active_streams_;
        }

        // **关键修复 2: 现在再转移所有权**
        streams_.emplace(stream_id, std::move(stream_ctx_ptr));

        // **关键修复 3: 直接 await do_write，而不是 co_spawn**
        // 这确保了请求数据被推送到内核缓冲区后，我们才开始等待响应。
        // 这样做也更简单，避免了额外的协程开销和生命周期问题。

        co_await do_write();
    } catch (const std::exception &e) {
        SPDLOG_ERROR("H2 Connection [{}]: do_write failed during execute: {}", id_, e.what());
        // 如果写入失败，流可能永远不会有响应，我们需要手动取消它
        handle_stream_close(stream_id, NGHTTP2_INTERNAL_ERROR);
        throw; // 重新抛出异常
    }

    // 现在可以安全地等待 future 了
    auto [ec, response] = co_await std::move(response_future);

    if (ec) {
        throw boost::system::system_error(ec, "H2 stream execution");
    }
    SPDLOG_DEBUG("execute end............", id_);
    co_return response;
}


void Http2Connection::init_nghttp2_session() {
    nghttp2_session_callbacks *callbacks;
    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, &on_begin_headers_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, &on_header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, &on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, &on_stream_close_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, &on_frame_recv_callback);
    nghttp2_session_client_new(&session_, callbacks, this);
    nghttp2_session_callbacks_del(callbacks);
}

boost::asio::awaitable<void> Http2Connection::session_loop() {
    auto self_weak = weak_from_this();
    while (is_usable()) {
        if (self_weak.expired()) {
            co_return; // 对象已销毁，安全退出
        }
        try {
            if (nghttp2_session_want_read(session_) == 0 && nghttp2_session_want_write(session_) == 0) {
                // 如果没有 I/O 事件，让出执行权，避免空转
                co_await boost::asio::post(strand_, boost::asio::use_awaitable);
                continue;
            }

            if (nghttp2_session_want_write(session_)) {
                co_await do_write();
            }
            if (nghttp2_session_want_read(session_)) {
                co_await do_read();
            }
        } catch (const boost::system::system_error &e) {
            if (e.code() != boost::asio::error::operation_aborted) {
                SPDLOG_WARN("H2 Connection [{}] loop error: {}", id_, e.what());
            }
            break; // 出现无法恢复的错误，退出循环
        } catch (const std::exception &e) {
            SPDLOG_ERROR("H2 Connection [{}] loop unexpected error: {}", id_, e.what());
            break;
        }
    }
}

// *** 新增的 do_read 辅助函数 ***
boost::asio::awaitable<void> Http2Connection::do_read() {
    auto self_weak = weak_from_this();

    if (is_closing_ || self_weak.expired()) co_return;

    std::array<char, 8192> read_buf_{};
    auto [ec, n] = co_await stream_->async_read_some(boost::asio::buffer(read_buf_), boost::asio::as_tuple(boost::asio::use_awaitable));
    if (self_weak.expired()) co_return;
    if (ec) {
        if (ec != boost::asio::error::eof && ec != boost::asio::ssl::error::stream_truncated) {
            SPDLOG_WARN("H2 Connection [{}] read error: {}", id_, ec.message());
            throw boost::system::system_error(ec); // 向上抛出
        }
        is_closing_ = true; // 标记连接已关闭
        co_return;
    }

    if (session_) {
        ssize_t rv = nghttp2_session_mem_recv(session_, reinterpret_cast<const uint8_t *>(read_buf_.data()), n);
        if (rv < 0) {
            is_closing_ = true;
        }
    }
}

boost::asio::awaitable<void> Http2Connection::do_write() {
    auto self_weak = weak_from_this();

    while (session_ && nghttp2_session_want_write(session_)) {
        if (is_closing_ || self_weak.expired()) co_return;

        const uint8_t *data = nullptr;
        ssize_t len = nghttp2_session_mem_send(session_, &data);

        if (len < 0) {
            SPDLOG_ERROR("nghttp2_session_mem_send failed: {}", nghttp2_strerror(len));
            throw std::runtime_error("nghttp2_session_mem_send failed: " + std::string(nghttp2_strerror(len)));
        }

        auto [ec, n] = co_await boost::asio::async_write(
            *stream_,
            boost::asio::buffer(data, len),
            boost::asio::as_tuple(boost::asio::use_awaitable)
        );

        if (is_closing_ || self_weak.expired()) co_return;

        if (ec) {
            SPDLOG_ERROR("H2 [{}]: async_write error: {}", id_, ec.message());
            is_closing_ = true;
            throw boost::system::system_error(ec); // 向上抛出
            co_return;
        }
    }
}


bool Http2Connection::is_usable() const {
    return !is_closing_ && stream_ && stream_->lowest_layer().is_open() && session_ != nullptr;
}

boost::asio::awaitable<void> Http2Connection::close() {
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);

    if (close_called_.exchange(true)) {
        co_return;
    }
    is_closing_ = true;

    // 1. 发送 GOAWAY 告知服务器我们准备关闭连接
    if (session_) {
        nghttp2_submit_goaway(session_,
                              NGHTTP2_FLAG_NONE,
                              nghttp2_session_get_last_proc_stream_id(session_),
                              NGHTTP2_NO_ERROR,
                              nullptr,
                              0);
        try {
            co_await do_write(); // 刷新写缓冲
        } catch (...) {
            // ignore
        }
    }


    // 2. 通知所有挂起的请求取消
    auto active_streams = std::move(streams_);
    for (auto &[id, stream_ctx_ptr]: active_streams) {
        if (stream_ctx_ptr) {
            stream_ctx_ptr->response_channel.try_send(
                boost::asio::error::operation_aborted, HttpResponse{});
        }
    }

    if (session_) {
        nghttp2_session_del(session_);
        session_ = nullptr; // 关键: 置为 nullptr
    }

    if (stream_ && stream_->lowest_layer().is_open()) {
        boost::system::error_code ec;
        co_await stream_->async_shutdown(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        stream_->lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        stream_->lowest_layer().close(ec);
    }
    SPDLOG_DEBUG("H2 Connection [{}]: Closed.", id_);
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

// --- Callbacks Implementation ---
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

void Http2Connection::handle_stream_close(int32_t stream_id, uint32_t error_code) {
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return;

    auto stream_ctx_ptr = std::move(it->second);
    streams_.erase(it);

    // **在这里减少计数**
    --active_streams_;

    if (error_code == NGHTTP2_NO_ERROR) {
        stream_ctx_ptr->response_in_progress.prepare_payload();
        stream_ctx_ptr->response_channel.try_send(boost::system::error_code{}, std::move(stream_ctx_ptr->response_in_progress));
    } else {
        SPDLOG_WARN("Stream {} closed with error code: {}", stream_id, error_code);
        stream_ctx_ptr->response_channel.try_send(boost::system::error_code(error_code, boost::system::generic_category()), HttpResponse{});
    }
}


int Http2Connection::on_stream_close_callback(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data) {
    auto self = static_cast<Http2Connection *>(user_data);
    // 安全推入 strand 执行
    boost::asio::post(self->strand_, [self, stream_id, error_code]() {
        self->handle_stream_close(stream_id, error_code);
    });

    return 0;
}

int Http2Connection::on_frame_recv_callback(nghttp2_session *, const nghttp2_frame *frame, void *user_data) {
    auto self = static_cast<Http2Connection *>(user_data);
    if (frame->hd.type == NGHTTP2_SETTINGS && (frame->hd.flags & NGHTTP2_FLAG_ACK) == 0) {
        self->handshake_completed_ = true;

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
