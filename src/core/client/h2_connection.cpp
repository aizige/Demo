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
// --- Constructor, Destructor, and Helpers ---
Http2Connection::Http2Connection(StreamPtr stream, std::string pool_key)
    : stream_(std::move(stream)),
      pool_key_(std::move(pool_key)),
      id_(generate_simple_uuid()),
      strand_(stream_->get_executor()) {
}

Http2Connection::~Http2Connection() {
    if (session_) nghttp2_session_del(session_);
}

std::string Http2Connection::generate_simple_uuid() {
    static std::atomic<uint64_t> counter = 0;
    return "h2-client-conn-" + std::to_string(++counter);
}

boost::asio::ip::tcp::socket& Http2Connection::lowest_layer_socket() {
    return stream_->next_layer().socket();
}

// --- Core Logic ---

void Http2Connection::start() {
    co_spawn(strand_, [self = shared_from_this()]() -> boost::asio::awaitable<void> {
        try {
            // 1. 初始化
            self->init_nghttp2_session();

            // 2. 准备并发送客户端的初始帧 (preface + settings)
            nghttp2_settings_entry iv[1] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}};
                          nghttp2_submit_settings(self->session_, NGHTTP2_FLAG_NONE, iv, 1);
            co_await self->do_write();

            // 3. 在发送完成后，启动主 I/O 循环
            co_await self->session_loop();
        } catch (const std::exception& e) {
            SPDLOG_ERROR("H2 Client Connection [{}] loop failed: {}", self->id(), e.what());
        }
        co_await self->close();
    }, boost::asio::detached);
}


/**
 * @brief 在 H2 连接上异步执行一个 HTTP 请求。
 *
 * @param request 要发送的 HttpRequest 对象。按值传递以利用移动语义。
 * @return 一个 awaitable，其结果是服务器返回的 HttpResponse 对象。
 */
boost::asio::awaitable<HttpResponse> Http2Connection::execute(HttpRequest request) {
    if (!is_usable()) {
        throw std::runtime_error("H2 connection is not usable for new requests.");
    }

    // 1. 将 Beast Request 转换为 nghttp2 的头部列表
    std::vector<nghttp2_nv> nva;
    prepare_headers(nva, request);

    // 2. 获取当前协程的 executor，用于创建 channel
    auto ex = co_await boost::asio::this_coro::executor;

    // 3. 创建此流的上下文（StreamContext），包含用于通信的 channel
    auto stream_ctx_ptr = std::make_unique<StreamContext>(ex);

    // 4. 准备请求体的数据提供者 (provider)
    nghttp2_data_provider provider{};
    if (!request.body().empty()) {
        // 将请求体从 request 对象【移动】到 stream context 中，避免拷贝
        stream_ctx_ptr->request_body = std::move(request.body());
        provider.source.ptr = stream_ctx_ptr.get();
        provider.read_callback = &Http2Connection::read_request_body_callback;
    }

    // 5. 提交请求给 nghttp2
    //    - provider: 如果 body 为空，则为 nullptr
    //    - stream_user_data: 将 stream_ctx 的【裸指针】与此流关联
    int32_t stream_id = nghttp2_submit_request(
        session_,
        nullptr,
        nva.data(), nva.size(),
        stream_ctx_ptr->request_body.empty() ? nullptr : &provider,
        stream_ctx_ptr.get()
    );

    if (stream_id < 0) {
        SPDLOG_ERROR("nghttp2_submit_request failed with error: {}", nghttp2_strerror(stream_id));
        throw std::runtime_error("nghttp2_submit_request failed: " + std::string(nghttp2_strerror(stream_id)));
    } else {
        SPDLOG_DEBUG("Submitted HTTP/2 request with stream ID: {}", stream_id);
    }

    // 6. 从 channel 获取一个 awaitable 的 future，以便我们等待结果
    //    必须在 stream_ctx_ptr 被移动【之前】获取
    auto future = stream_ctx_ptr->response_channel.async_receive(boost::asio::as_tuple(boost::asio::use_awaitable));

    // 7. 将 StreamContext 的所有权【移动】到 streams_ map 中进行管理
    streams_.emplace(stream_id, std::move(stream_ctx_ptr));

    // 8. 触发网络写操作，确保请求被发送出去
    co_await do_write();

    // 9. **在此处挂起**，等待 on_stream_close_callback 通过 channel 发回结果
    auto [ec, response] = co_await std::move(future);

    // 10. 收到结果，检查错误码
    if (ec) {
        // 如果回调报告了错误（如 operation_aborted 或 nghttp2 错误），则抛出异常
        throw boost::system::system_error(ec);
    }

    // 11. 成功，返回最终的 HttpResponse 对象
    co_return response;
}

void Http2Connection::init_nghttp2_session() {
    nghttp2_session_callbacks* callbacks;
    nghttp2_session_callbacks_new(&callbacks);

    SPDLOG_DEBUG("Binding HTTP/2 session callbacks...");

    // 设置所有【实际存在】的回调
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, &on_begin_headers_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, &on_header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, &on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, &on_stream_close_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, &on_frame_recv_callback);

    SPDLOG_DEBUG("HTTP/2 session callbacks bound successfully.");

    nghttp2_session_client_new(&session_, callbacks, this);
    nghttp2_session_callbacks_del(callbacks);
}

boost::asio::awaitable<void> Http2Connection::session_loop() {
    std::array<char, 8192> buf{};
    while (is_usable()) {
        auto [ec, n] = co_await stream_->async_read_some(boost::asio::buffer(buf), boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec) {
            if (ec != boost::asio::error::eof && ec != boost::asio::ssl::error::stream_truncated) {
                SPDLOG_WARN("H2 client connection [{}] read error: {}", id_, ec.message());
                throw boost::system::system_error(ec); // 向上抛出“操作已取消”的异常
            }
            break;
        }

        ssize_t rv = nghttp2_session_mem_recv(session_, reinterpret_cast<const uint8_t*>(buf.data()), n);
        if (rv < 0) {
            SPDLOG_ERROR("nghttp2_session_mem_recv failed for [{}]: {}", id_, nghttp2_strerror(rv));
            break;
        }
        co_await do_write();
    }
}

boost::asio::awaitable<void> Http2Connection::do_write() {
    while (is_usable() && nghttp2_session_want_write(session_)) {
        const uint8_t* data = nullptr;
        ssize_t len = nghttp2_session_mem_send(session_, &data);
        if (len <= 0) break;
        co_await boost::asio::async_write(*stream_, boost::asio::buffer(data, len), boost::asio::use_awaitable);
    }
}

bool Http2Connection::is_usable() const {
    // 我们的 is_closing_ 标志现在是关键
    // 它会在 close() 被调用时，或者在 on_frame_recv_callback 收到 GOAWAY 时被设置为 true
    return
        !is_closing_ &&
        stream_->lowest_layer().is_open() &&
        session_ != nullptr;
}

boost::asio::awaitable<void> Http2Connection::close() {
    // 1. 使用原子标志确保 close() 只被执行一次，防止竞态条件
    if (is_closing_.exchange(true)) {
        co_return;
    }


    // 尽力发送 GOAWAY
    if (session_ && stream_->lowest_layer().is_open()) {
        nghttp2_submit_goaway(session_,
                              NGHTTP2_FLAG_NONE,
                              nghttp2_session_get_last_proc_stream_id(session_),
                              NGHTTP2_NO_ERROR,
                              nullptr, 0);
        try { co_await do_write(); } catch (...) {
        }
    }

    // 取消等待者
    auto streams_to_cancel = std::move(streams_);
    for (auto& [id, stream_ctx_ptr] : streams_to_cancel) {
        if (stream_ctx_ptr) {
            stream_ctx_ptr->response_channel.try_send(boost::asio::error::operation_aborted, HttpResponse{});
        }
    }

    // 优雅关闭 SSL
    if (stream_->lowest_layer().is_open()) {
        boost::system::error_code ec;
        try {
            co_await stream_->async_shutdown(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        } catch (...) {
        }
        auto error_code = stream_->lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        auto close = stream_->lowest_layer().close(ec);
        SPDLOG_DEBUG("H2 Connection [{}]: Socket closed.", id_);
        SPDLOG_DEBUG("H2 Connection [{}]: Socket closed.", id_);
    }
}

void Http2Connection::prepare_headers(std::vector<nghttp2_nv>& nva, const HttpRequest& req) {
    SPDLOG_DEBUG("Preparing headers for request with target: {}", req.target());

    // 1. 手动计算大小
    size_t header_count = 0;
    for (const auto& field : req) {
        if (field.name() != http::field::host && field.name() != http::field::connection) {
            header_count++;
        } else {
            SPDLOG_DEBUG("Filtered header: {}", field.name_string());
        }
    }

    // 2. 进行 reserve
    SPDLOG_DEBUG("Reserving space for {} headers ({} regular + 4 pseudo-headers).", header_count, header_count + 4);
    nva.reserve(header_count + 4); // 4 个伪头


    std::string authority = std::string(req.at(http::field::host));
    std::string path = std::string(req.target());
    if (path.empty()) path = "/";

    nva.push_back({(uint8_t*)":method", (uint8_t*)req.method_string().data(), 7, req.method_string().length(), NGHTTP2_NV_FLAG_NONE});
    SPDLOG_DEBUG("Added pseudo-header: :method = {}", req.method_string());

    nva.push_back({(uint8_t*)":scheme", (uint8_t*)"https", 7, 5, NGHTTP2_NV_FLAG_NONE});
    SPDLOG_DEBUG("Added pseudo-header: :scheme = https");

    nva.push_back({(uint8_t*)":authority", (uint8_t*)authority.c_str(), 10, authority.length(), NGHTTP2_NV_FLAG_NONE});
    SPDLOG_DEBUG("Added pseudo-header: :authority = {}", authority);

    nva.push_back({(uint8_t*)":path", (uint8_t*)path.c_str(), 5, path.length(), NGHTTP2_NV_FLAG_NONE});
    SPDLOG_DEBUG("Added pseudo-header: :path = {}", path);

    for (const auto& field : req) {
        http::field name_enum = field.name();
        // --- 过滤所有 connection-specific 的头部 ---
        if (name_enum == http::field::host ||
            name_enum == http::field::connection ||
            name_enum == http::field::upgrade ||
            name_enum == http::field::proxy_connection ||
            name_enum == http::field::transfer_encoding ||
            name_enum == http::field::keep_alive ||
            name_enum == http::field::accept_encoding) {
            SPDLOG_DEBUG("Skipping connection-specific header: {}", field.name_string());
            continue;
        }
        // --- 将头部名称转换为小写 ---
        std::string name_lower = boost::algorithm::to_lower_copy(std::string(field.name_string()));

        nva.push_back({
            (uint8_t*)name_lower.c_str(), (uint8_t*)field.value().data(),
            name_lower.length(), field.value().length(), NGHTTP2_NV_FLAG_NONE
        });

        SPDLOG_DEBUG("Added header: {} = {}", name_lower, field.value());
    }
}

// --- Callbacks Implementation ---
int Http2Connection::on_begin_headers_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data) {
    (void)user_data;
    if (frame->hd.type != NGHTTP2_HEADERS) return 0;

    auto stream_ctx = static_cast<StreamContext*>(
        nghttp2_session_get_stream_user_data(session, frame->hd.stream_id)
    );
    if (!stream_ctx) return 0;

    stream_ctx->response_in_progress.version(20);
    return 0;
}


int Http2Connection::on_header_callback(nghttp2_session* session, const nghttp2_frame* frame, const uint8_t* name, size_t namelen, const uint8_t* value, size_t valuelen, uint8_t flags, void* user_data) {
    // 这个回调不需要 Http2Connection 的 user_data，因为我们可以从 stream 中获取所有东西
    // auto self = static_cast<Http2Connection*>(user_data);
    (void)user_data;
    auto stream_ctx = static_cast<StreamContext*>(
        nghttp2_session_get_stream_user_data(session, frame->hd.stream_id)
    );
    if (!stream_ctx) return 0;

    std::string_view key((const char*)name, namelen);
    if (key == ":status") {
        try {
            stream_ctx->response_in_progress.result(std::stoi(std::string((const char*)value, valuelen)));
        } catch (...) {
            /* ignore */
        }
    } else if (!key.empty() && key[0] != ':') {
        stream_ctx->response_in_progress.set(key, std::string_view((const char*)value, valuelen));
    }
    return 0;
}


int Http2Connection::on_data_chunk_recv_callback(nghttp2_session* session, uint8_t, int32_t stream_id, const uint8_t* data, size_t len, void* user_data) {
    (void)user_data;
    auto stream_ctx = static_cast<StreamContext*>(
        nghttp2_session_get_stream_user_data(session, stream_id)
    );
    if (!stream_ctx) return 0;

    stream_ctx->response_in_progress.body().append((const char*)data, len);
    return 0;
}

int Http2Connection::on_stream_close_callback(nghttp2_session* session, int32_t stream_id, uint32_t error_code, void* user_data) {
    auto self = static_cast<Http2Connection*>(user_data);
    auto stream_ctx = static_cast<StreamContext*>(
        nghttp2_session_get_stream_user_data(session, stream_id)
    );
    if (!stream_ctx) return 0;

    // 我们 post 到 strand 以确保线程安全
    post(self->strand_, [self, stream_id, stream_ctx, error_code]() {
        // 从 map 中移除并获取 unique_ptr 的所有权
        auto it = self->streams_.find(stream_id);
        if (it == self->streams_.end()) return;
        auto stream_ctx_ptr = std::move(it->second);
        self->streams_.erase(it);

        if (error_code == NGHTTP2_NO_ERROR) {
            stream_ctx_ptr->response_in_progress.prepare_payload();
            stream_ctx_ptr->response_channel.try_send(boost::system::error_code{}, std::move(stream_ctx_ptr->response_in_progress));
        } else {
            stream_ctx_ptr->response_channel.try_send(boost::system::error_code(error_code, boost::system::generic_category()), HttpResponse{});
        }
        // 当这个 lambda 结束时，stream_ctx_ptr 被销毁，StreamContext 被释放
    });
    return 0;
}

int Http2Connection::on_frame_recv_callback(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
    auto self = static_cast<Http2Connection*>(user_data);
    SPDLOG_DEBUG("Frame received: type = {}, stream ID = {}, error code = {}",
                 frame->hd.type,
                 frame->hd.stream_id,
                 frame->rst_stream.error_code); // 如果是 RST_STREAM

    SPDLOG_DEBUG("Frame received: type = {}, stream ID = {}, flags = {}, length = {}",
                 frame->hd.type, frame->hd.stream_id, frame->hd.flags, frame->hd.length);


    if (frame->hd.type == NGHTTP2_GOAWAY) {
        SPDLOG_WARN("GOAWAY received with error code: {}, debug data: {}", frame->goaway.error_code, std::string((char*)frame->goaway.opaque_data, frame->goaway.opaque_data_len));

        self->is_closing_ = true;
        // 主动唤醒所有挂起的请求
        for (auto& [id, ctx] : self->streams_) {
            if (ctx) ctx->response_channel.try_send(boost::asio::error::operation_aborted, HttpResponse{});
        }
        self->streams_.clear();
    }
    return 0;
}


ssize_t Http2Connection::read_request_body_callback(nghttp2_session*, int32_t, uint8_t* buf, size_t length, uint32_t* data_flags, nghttp2_data_source* source, void*) {
    //  source->ptr 获取
    auto stream_ctx = static_cast<StreamContext*>(source->ptr);
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
