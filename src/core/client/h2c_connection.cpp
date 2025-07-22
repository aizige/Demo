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

// --- Constructor, Destructor, and Helpers ---
Http2cConnection::Http2cConnection(StreamPtr stream, std::string pool_key)
    : stream_(std::move(stream)),
      pool_key_(std::move(pool_key)),
      id_(generate_simple_uuid()),
      strand_(stream_->get_executor())
{}

Http2cConnection::~Http2cConnection() {
    if (session_) nghttp2_session_del(session_);
}

std::string Http2cConnection::generate_simple_uuid() {
    static std::atomic<uint64_t> counter = 0;
    return "h2-client-conn-" + std::to_string(++counter);
}

boost::asio::ip::tcp::socket& Http2cConnection::lowest_layer_socket() {
    return stream_->socket();
}


// --- Core Logic ---

void Http2cConnection::start() {
    co_spawn(strand_, [self = shared_from_this()]() -> boost::asio::awaitable<void> {
        try {
            self->init_nghttp2_session();

            nghttp2_settings_entry iv[1] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}};
            nghttp2_submit_settings(self->session_, NGHTTP2_FLAG_NONE, iv, 1);

            co_await self->do_write();
            co_await self->session_loop();
        } catch (const std::exception& e) {
            spdlog::error("H2 Client Connection [{}] loop failed: {}", self->id(), e.what());
        }
       co_await self->close();
    }, boost::asio::detached);
}




boost::asio::awaitable<HttpResponse> Http2cConnection::execute(HttpRequest request) { // <-- 改回按值传递以匹配接口
    if (!is_usable()) throw std::runtime_error("H2 connection is not usable.");

    std::vector<nghttp2_nv> nva;
    prepare_headers(nva, request);

    auto ex = co_await boost::asio::this_coro::executor;

    auto stream_ctx_ptr = std::make_unique<StreamContext>(ex);

    nghttp2_data_provider provider{};
    if (!request.body().empty()) {
        stream_ctx_ptr->request_body = std::move(request.body());
        provider.source.ptr = stream_ctx_ptr.get();
        provider.read_callback = &Http2cConnection::read_request_body_callback;
    }

    // 3. 将 unique_ptr 的【裸指针】传递给 nghttp2
    int32_t stream_id = nghttp2_submit_request(session_, nullptr, nva.data(), nva.size(),
                                             request.body().empty() ? nullptr : &provider,
                                             stream_ctx_ptr.get());

    if (stream_id < 0) throw std::runtime_error("nghttp2_submit_request failed: " + std::string(nghttp2_strerror(stream_id)));

    // 4. 将 unique_ptr 的【所有权】移入 map
    streams_.emplace(stream_id, std::move(stream_ctx_ptr));

    co_await do_write();

    // 2. **从 channel 异步接收结果**
    //    我们在这里 co_await，等待回调函数向 channel 发送数据
    auto [ec, response] = co_await streams_.at(stream_id)->response_channel.async_receive(
        boost::asio::as_tuple(boost::asio::use_awaitable));

    if (ec) throw boost::system::system_error(ec);

    // 这里可以加上解压逻辑，因为它现在是 H2 连接的 execute
    // ...

    co_return response;
}

void Http2cConnection::init_nghttp2_session() {
    nghttp2_session_callbacks* callbacks;
    nghttp2_session_callbacks_new(&callbacks);

    // --- **[已修复]** ---
    // 设置所有【实际存在】的回调
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, &on_begin_headers_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, &on_header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, &on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, &on_stream_close_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, &on_frame_recv_callback);

    nghttp2_session_client_new(&session_, callbacks, this);
    nghttp2_session_callbacks_del(callbacks);
}

boost::asio::awaitable<void> Http2cConnection::session_loop() {
    std::array<char, 8192> buf;
    while (is_usable()) {
        auto [ec, n] = co_await stream_->async_read_some( boost::asio::buffer(buf),  boost::asio::as_tuple( boost::asio::use_awaitable));
        if (ec) {
            if (ec !=  boost::asio::error::eof) spdlog::warn("H2C connection [{}] read error: {}", id_, ec.message());
            break;
        }
        ssize_t rv = nghttp2_session_mem_recv(session_, reinterpret_cast<const uint8_t*>(buf.data()), n);
        if (rv < 0) {
            spdlog::error("nghttp2_session_mem_recv failed for [{}]: {}", id_, nghttp2_strerror(rv));
            break;
        }
        co_await do_write();
    }
}

boost::asio::awaitable<void> Http2cConnection::do_write() {
    while (is_usable() && nghttp2_session_want_write(session_)) {
        const uint8_t* data = nullptr;
        ssize_t len = nghttp2_session_mem_send(session_, &data);
        if (len <= 0) break;
        co_await boost::asio::async_write(*stream_, boost::asio::buffer(data, len), boost::asio::use_awaitable);
    }
}

bool Http2cConnection::is_usable() const {
    // 我们的 is_closing_ 标志现在是关键
    // 它会在 close() 被调用时，或者在 on_frame_recv_callback 收到 GOAWAY 时被设置为 true
    return
        !is_closing_ &&
        stream_->socket().is_open() &&
        session_ != nullptr;
}

boost::asio::awaitable<void> Http2cConnection::close() {
    if (is_closing_.exchange(true)) co_return;
    boost::asio::post(strand_, [self = shared_from_this()] {
        if (self->session_ && self->stream_->socket().is_open()) {
            nghttp2_submit_goaway(self->session_, NGHTTP2_FLAG_NONE, nghttp2_session_get_last_proc_stream_id(self->session_), NGHTTP2_NO_ERROR, nullptr, 0);
            boost::asio::co_spawn(self->strand_, self->do_write(), boost::asio::detached);
        }
        auto streams_to_cancel = std::move(self->streams_);
        for (auto& [id, stream_ctx_ptr] : streams_to_cancel) {
            if (stream_ctx_ptr) {
                stream_ctx_ptr->response_channel.try_send(boost::asio::error::operation_aborted, HttpResponse{});
            }
        }
        boost::system::error_code ec;
        if (self->stream_->socket().is_open()) {
            self->stream_->socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            self->stream_->socket().close(ec);
        }
    });
}

void Http2cConnection::prepare_headers(std::vector<nghttp2_nv>& nva, const HttpRequest& req) {
    // 1. 手动计算大小
    size_t header_count = 0;
    for (const auto& field : req) {
        if (field.name() != http::field::host && field.name() != http::field::connection) {
            header_count++;
        }
    }
    // 2. 进行 reserve
    nva.reserve(header_count + 4); // 4 个伪头


    std::string authority = std::string(req.at(http::field::host));
    std::string path = std::string(req.target());
    if (path.empty()) path = "/";

    nva.push_back({(uint8_t*)":method", (uint8_t*)req.method_string().data(), 7, req.method_string().length(), NGHTTP2_NV_FLAG_NONE});
    nva.push_back({(uint8_t*)":scheme", (uint8_t*)"http", 7, 5, NGHTTP2_NV_FLAG_NONE});
    nva.push_back({(uint8_t*)":authority", (uint8_t*)authority.c_str(), 10, authority.length(), NGHTTP2_NV_FLAG_NONE});
    nva.push_back({(uint8_t*)":path", (uint8_t*)path.c_str(), 5, path.length(), NGHTTP2_NV_FLAG_NONE});

    for (const auto& field : req) {
        if (field.name() == http::field::host || field.name() == http::field::connection) continue;
        nva.push_back({(uint8_t*)field.name_string().data(), (uint8_t*)field.value().data(),
                       field.name_string().length(), field.value().length(), NGHTTP2_NV_FLAG_NONE});
    }
}

// --- Callbacks Implementation ---
int Http2cConnection::on_begin_headers_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data) {
    auto self = static_cast<Http2cConnection*>(user_data);
    int32_t stream_id = frame->hd.stream_id;

    // 我们只关心服务器发来的响应 HEADERS 帧
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_RESPONSE) {
        return 0;
    }

    auto it = self->streams_.find(stream_id);
    if (it == self->streams_.end()) {
        // 如果这是一个服务器推送(Server Push)的流，我们可能没有它的上下文
        // 对于简单的客户端，我们可以选择忽略它
        // nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_CANCEL);
        return 0;
    }

    // 在这里，我们可以为即将到来的头部设置 response 的版本号
    it->second->response_in_progress.version(20);
    return 0;
}

int Http2cConnection::on_header_callback(nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, size_t namelen, const uint8_t* value, size_t valuelen, uint8_t, void* user_data) {
    auto self = static_cast<Http2cConnection*>(user_data);
    auto it = self->streams_.find(frame->hd.stream_id);
    if (it == self->streams_.end()) return 0;

    auto& stream_ctx = *it->second;
    std::string_view key((const char*)name, namelen);

    if (key == ":status") {
        try {
            stream_ctx.response_in_progress.result(std::stoi(std::string((const char*)value, valuelen)));
        } catch (...) { /* ignore invalid status */ }
    } else if (!key.empty() && key[0] != ':') {
        stream_ctx.response_in_progress.set(key, std::string_view((const char*)value, valuelen));
    }
    return 0;
}

int Http2cConnection::on_data_chunk_recv_callback(nghttp2_session*, uint8_t, int32_t stream_id, const uint8_t* data, size_t len, void* user_data) {
    auto self = static_cast<Http2cConnection*>(user_data);
    auto it = self->streams_.find(stream_id);
    if (it == self->streams_.end()) return 0;
    it->second->response_in_progress.body().append((const char*)data, len);
    return 0;
}

int Http2cConnection::on_stream_close_callback(nghttp2_session*, int32_t stream_id, uint32_t error_code, void* user_data) {
    auto self = static_cast<Http2cConnection*>(user_data);

   boost::asio::post(self->strand_, [self, stream_id, error_code]() {
        auto it = self->streams_.find(stream_id);
        if (it == self->streams_.end()) return;

       // 将 StreamContext 的所有权从 map 中移出
        auto stream_ctx = std::move(it->second);
        self->streams_.erase(it);

       // 3. **向 channel 发送结果，以唤醒 execute 协程**
      if (error_code == NGHTTP2_NO_ERROR) {
          stream_ctx->response_in_progress.prepare_payload();
          // try_send 是非阻塞的，对于容量为 1 的 channel 是安全的
          stream_ctx->response_channel.try_send(boost::system::error_code{}, std::move(stream_ctx->response_in_progress));
      } else {
          stream_ctx->response_channel.try_send(
         boost::system::error_code(error_code, boost::system::generic_category()),
         HttpResponse{} // <-- 使用 Response{} 来创建一个临时的、默认构造的对象
     );
      }
    });
    return 0;
}

int Http2cConnection::on_frame_recv_callback(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
    auto self = static_cast<Http2cConnection*>(user_data);

    // --- **[关键]** 添加 GOAWAY 帧的检测 ---
    if (frame->hd.type == NGHTTP2_GOAWAY) {
        SPDLOG_WARN("H2 client connection [{}] received GOAWAY from server. Shutting down.", self->id_);
        // 设置标志，is_usable() 会立即返回 false，session_loop 会退出
        self->is_closing_ = true;
    }

    return 0; // 客户端通常不需要对其他帧做特殊处理
}


ssize_t Http2cConnection::read_request_body_callback(nghttp2_session*, int32_t, uint8_t* buf, size_t length, uint32_t* data_flags, nghttp2_data_source* source, void*) {
    auto stream_ctx = static_cast<StreamContext*>(source->ptr);
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
