//
// Created by ubuntu on 2025/7/21.
//

#ifndef UNTITLED1_H2_CONNECTION_BASE_HPP
#define UNTITLED1_H2_CONNECTION_BASE_HPP

#include "iconnection.hpp"
#include <nghttp2/nghttp2.h>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <spdlog/spdlog.h>
#include <deque>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;

// Awaitable promise for response
using ResponsePromise = net::experimental::promise<HttpResponse>;
using ResponseFuture = net::awaitable<HttpResponse>;

template<typename Derived, typename StreamType>
class H2ConnectionBase : public IConnection, public std::enable_shared_from_this<Derived> {
public:
    using StreamPtr = std::shared_ptr<StreamType>;

    H2ConnectionBase(StreamPtr stream, std::string pool_key)
        : stream_(stream),
          pool_key_(std::move(pool_key)),
          id_(generate_simple_uuid()),
          strand_(stream_->get_executor())
    {}

    virtual ~H2ConnectionBase() {
        if (session_) {
            nghttp2_session_del(session_);
        }
    }

    // --- IConnection Interface ---
    net::awaitable<Response> execute(Request request) override {
        if (!is_usable()) {
            throw std::runtime_error("H2 connection is not usable for new requests.");
        }

        // Convert Beast request to nghttp2 headers
        std::vector<nghttp2_nv> nva;
        prepare_headers(nva, request);

        // Data provider for request body
        nghttp2_data_provider provider{};
        std::string req_body_copy; // Keep body alive
        if (!request.body().empty()) {
            req_body_copy = std::move(request.body());
            provider.source.ptr = &req_body_copy;
            provider.read_callback = &H2ConnectionBase::read_request_body_callback;
        }

        // Create a promise to wait for the response
        auto [promise, future] = ResponsePromise::create(co_await net::this_coro::executor);

        // Submit the request
        int32_t stream_id = nghttp2_submit_request(session_, nullptr, nva.data(), nva.size(),
                                                 request.body().empty() ? nullptr : &provider, nullptr);

        if (stream_id < 0) {
            throw std::runtime_error("nghttp2_submit_request failed: " + std::string(nghttp2_strerror(stream_id)));
        }

        streams_[stream_id].response_promise = std::move(promise);

        co_await do_write(); // Ensure the request is sent

        co_return co_await future;
    }

    bool is_usable() const override {
        return stream_->lowest_layer().is_open() && !is_closing_;
    }

    void close() override {
        is_closing_ = true;
        if (session_) {
            nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE, nghttp2_session_get_last_proc_stream_id(session_), NGHTTP2_NO_ERROR, nullptr, 0);
            net::co_spawn(strand_, do_write(), net::detached);
        }
        boost::system::error_code ec;
        stream_->lowest_layer().close(ec);
    }

    const std::string& id() const override { return id_; }
    const std::string& get_pool_key() const override { return pool_key_; }
    net::ip::tcp::socket& lowest_layer_socket() override { return stream_->lowest_layer(); }

    // --- H2 Specific Methods ---
    net::awaitable<void> start() {
        init_nghttp2_session();
        nghttp2_settings_entry iv[1] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}};
        nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv, 1);

        co_await do_write();

        net::co_spawn(strand_, session_loop(), [self = this->shared_from_this()](std::exception_ptr p){
            if (p) {
                try { std::rethrow_exception(p); }
                catch (const std::exception& e) {
                    spdlog::error("H2 session [{}] loop failed: {}", self->id(), e.what());
                }
            }
            self->close();
        });
    }

protected:
    struct StreamContext {
        ResponsePromise response_promise;
        Response response_in_progress;
    };

    // --- Private Methods ---
    void init_nghttp2_session() {
        nghttp2_session_callbacks* callbacks;
        nghttp2_session_callbacks_new(&callbacks);
        nghttp2_session_callbacks_set_on_header_callback(callbacks, &on_header_callback);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, &on_data_chunk_recv_callback);
        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, &on_stream_close_callback);
        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, &on_frame_recv_callback);
        nghttp2_session_client_new(&session_, callbacks, this);
        nghttp2_session_callbacks_del(callbacks);
    }

    net::awaitable<void> session_loop() {
        std::array<char, 8192> buf;
        while (is_usable()) {
            auto [ec, n] = co_await stream_->async_read_some(net::buffer(buf), net::as_tuple(net::use_awaitable));
            if (ec) {
                if (ec != net::error::eof) spdlog::warn("H2 session [{}] read error: {}", id_, ec.message());
                is_closing_ = true;
                break;
            }

            ssize_t rv = nghttp2_session_mem_recv(session_, reinterpret_cast<const uint8_t*>(buf.data()), n);
            if (rv < 0) {
                spdlog::error("nghttp2_session_mem_recv failed: {}", nghttp2_strerror(rv));
                is_closing_ = true;
                break;
            }
            co_await do_write();
        }
    }

    net::awaitable<void> do_write() {
        while (nghttp2_session_want_write(session_)) {
            const uint8_t* data = nullptr;
            ssize_t len = nghttp2_session_mem_send(session_, &data);
            if (len <= 0) break;
            co_await net::async_write(*stream_, net::buffer(data, len), net::use_awaitable);
        }
    }

    void prepare_headers(std::vector<nghttp2_nv>& nva, const Request& req) {
        // ... (Implementation in .cpp file) ...
    }

    // --- nghttp2 Callbacks ---
    static int on_header_callback(nghttp2_session*, const nghttp2_frame*, const uint8_t*, size_t, const uint8_t*, size_t, uint8_t, void*);
    // ... (All other callbacks) ...

    // Data Members
    StreamPtr stream_;
    std::string pool_key_;
    std::string id_;
    net::strand<net::any_io_executor> strand_;
    nghttp2_session* session_ = nullptr;
    std::unordered_map<int32_t, StreamContext> streams_;
    std::atomic<bool> is_closing_ = false;

    static std::string generate_simple_uuid() { /* ... */ }
    static ssize_t read_request_body_callback(nghttp2_session*, int32_t, uint8_t*, size_t, uint32_t*, nghttp2_data_source*, void*);
};

// --- Concrete Classes ---

class Http2Connection : public H2ConnectionBase<Http2Connection, beast::ssl_stream<beast::tcp_stream>> {
public:
    Http2Connection(StreamPtr stream, std::string key) : H2ConnectionBase(std::move(stream), std::move(key)) {}
};

class H2cConnection : public H2ConnectionBase<H2cConnection, beast::tcp_stream> {
public:
    H2cConnection(StreamPtr stream, std::string key) : H2ConnectionBase(std::move(stream), std::move(key)) {}
};
#endif //UNTITLED1_H2_CONNECTION_BASE_HPP