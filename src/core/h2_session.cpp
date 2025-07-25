#include "h2_session.hpp"
#include <spdlog/spdlog.h>          // 日志库
#include <cstring>                  // 用于 C 风格字符串操作，如 memcpy
#include <boost/asio/experimental/awaitable_operators.hpp> // Asio 实验性功能，提供了协程操作符，如 ||
#include <chrono>                   // C++ 时间库
#include <boost/asio/experimental/parallel_group.hpp> // <-- 引入 parallel_group
#include "http/request_context.hpp"

// 将 C++14 的时间字面量（如 30s）引入当前作用域
using namespace std::literals::chrono_literals;

/**
 * @brief Http2Session 构造函数
 */
Http2Session::Http2Session(StreamPtr stream, Router& router)
    : stream_(std::move(stream)), // 移动语义，接管 SSL 流的所有权
      router_(router), // 保存对路由器的引用
      strand_(stream_->get_executor()), // 从流中获取执行器并构造一个 strand，保证线程安全
      session_(nullptr), // 初始化 nghttp2 会话指针为空
      idle_timer_(strand_), // 在同一个 strand 上构造空闲计时器
      dispatch_channel_(strand_)
{
    // 初始时，将计时器设置为永不超时
    idle_timer_.expires_at(std::chrono::steady_clock::time_point::max());
}

/**
 * @brief Http2Session 析构函数
 *        在对象销毁时，安全地释放 nghttp2 会话资源
 */
Http2Session::~Http2Session() {
    if (session_)
        nghttp2_session_del(session_);
    // **在析构时调用回调**
}

/**
 * @brief 初始化 nghttp2 会话，设置所有必需的回调函数
 */
void Http2Session::init_session() {
    nghttp2_session_callbacks* callbacks;
    nghttp2_session_callbacks_new(&callbacks);
    // 绑定各种事件的回调函数，这是 nghttp2 与我们 C++ 代码交互的桥梁
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    // 创建一个服务器端 nghttp2 会话实例，并将 this 指针作为用户数据传递
    nghttp2_session_server_new(&session_, callbacks, this);
    nghttp2_session_callbacks_del(callbacks);
}

/**
 * @brief 启动会话处理的入口点
 */
boost::asio::awaitable<void> Http2Session::start() {
    using namespace boost::asio::experimental::awaitable_operators;
    try {
        // 并行运行 I/O 循环和请求处理循环
        co_await (session_loop() && dispatcher_loop());
    } catch (const std::exception& e) {
        SPDLOG_DEBUG("H2 session ended: {}", e.what());
    }
}

/**
 * @brief 执行写操作的协程
 *        从 nghttp2 的发送缓冲区取出数据并写入网络
 */
boost::asio::awaitable<void> Http2Session::do_write() {
    try {
        // 只要 nghttp2 告诉我们它有数据想发送，就一直循环
        while (nghttp2_session_want_write(session_)) {
            const uint8_t* data_ptr = nullptr;
            // 从 nghttp2 获取待发送数据块的指针和长度
            ssize_t len = nghttp2_session_mem_send(session_, &data_ptr);
            if (len <= 0)
                break; // 如果没有数据或出错，则退出循环

            // 异步写入数据。co_await 在这里提供了最关键的、天然的背压控制。
            // 在数据被完全写入（或进入内核缓冲区）之前，协程会在此挂起，
            // 不会去拉取下一个数据块，从而完美地避免了撑爆 OpenSSL 缓冲区的问题。
            co_await boost::asio::async_write(*stream_, boost::asio::buffer(data_ptr, len), boost::asio::use_awaitable);
        }
    } catch (const std::exception&) {
        // 如果写入时发生任何错误（如连接被对方重置），关闭套接字以终止整个会话。
        stream_->next_layer().close();
    }
}

/**
 * @brief 会话的主事件循环协程 (融合模型)
 */
// in Http2Session.cpp

boost::asio::awaitable<void> Http2Session::session_loop() {
    co_await boost::asio::dispatch(strand_, boost::asio::use_awaitable);
    init_session();
    constexpr auto timeout = 30s;

    idle_timer_.expires_after(timeout);
    nghttp2_settings_entry iv[1] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}};
    nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv, 1);
    co_await do_write();

    std::array<char, 8192> buf{};

    using namespace boost::asio::experimental::awaitable_operators;
    while (stream_->next_layer().is_open()) {

        // **为了清晰，我们明确指定 as_tuple**
        auto read_op = stream_->async_read_some(
            boost::asio::buffer(buf),
            boost::asio::as_tuple(boost::asio::use_awaitable)
        );
        auto timer_op = idle_timer_.async_wait(
            boost::asio::as_tuple(boost::asio::use_awaitable)
        );

        auto result = co_await(std::move(read_op) || std::move(timer_op));

        if (result.index() == 1) { // --- 超时发生 ---
            // timer_op 完成
            // as_tuple(use_awaitable) 会让 timer_op 返回 std::tuple<error_code>
            auto [ec_timer] = std::get<1>(result);
            if (!ec_timer) {
                SPDLOG_INFO("H2 connection idle timeout.");
            } else if (ec_timer != boost::asio::error::operation_aborted) {
                SPDLOG_WARN("H2 timer error: {}", ec_timer.message());
            }
            dispatch_channel_.close();
            break;
        }

        // --- 读取成功或读取出错 ---

        auto [ec, n] = std::get<0>(result);

        // 2. 检查读取操作的 error_code
        if (ec) {
            if (ec != boost::asio::error::eof) {
                SPDLOG_WARN("H2 session_loop read error: {}", ec.message());
            }
            dispatch_channel_.close();
            break;
        }

        // 3. 读取成功，重置计时器
        idle_timer_.expires_after(timeout);

        // 4. 将数据喂给 nghttp2
        ssize_t rv = nghttp2_session_mem_recv(session_, (const uint8_t*)buf.data(), n);
        if (rv < 0) {
            SPDLOG_ERROR("nghttp2_session_mem_recv() failed: {}", nghttp2_strerror(rv));
            dispatch_channel_.close();
            break;
        }

        // 5. 触发写操作
        co_await do_write();
    }
}
// --- **[新增]** 请求处理循环 ---
boost::asio::awaitable<void> Http2Session::dispatcher_loop() {
    for (;;) {
        auto [ec, stream_id] = co_await dispatch_channel_.async_receive(boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec) {
            // Channel 已关闭，意味着 session_loop 已结束，我们也应该退出
            break;
        }
        // co_spawn 一个独立的协程来处理 dispatch，这样多个请求可以并行处理
        boost::asio::co_spawn(strand_, dispatch(stream_id), boost::asio::detached);
    }
}

/**
 * @brief 请求分发和处理协程
 */
boost::asio::awaitable<void> Http2Session::dispatch(int32_t stream_id) {
    // ... 你的 dispatch 逻辑，使用 vector 存储 raw_headers 的最终版本 ...
    // ... 我将使用这个版本，因为它最健壮 ...
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) co_return;

    StreamContext stream_ctx = std::move(it->second);
    streams_.erase(it);

    HttpRequest req;
    req.version(20);
    for (const auto& pair : stream_ctx.headers) {
        const std::string& key = pair.first;
        const std::string& val = pair.second;
        if (key.empty()) continue;
        if (key[0] == ':') {
            if (key == ":method") req.method(http::string_to_verb(val));
            else if (key == ":path") req.target(val);
            else if (key == ":authority") req.set(http::field::host, val);
        } else {
            req.set(key, val);
        }
    }
    req.body() = std::move(stream_ctx.body);
    req.prepare_payload();

    RouteMatch match = router_.dispatch(req.method(), req.target());
    RequestContext ctx(std::move(req), std::move(match.path_params));

    try {
        co_await match.handler(ctx);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Exception in H2 handler for [{}]: {}", ctx.request().target(), e.what());
        ctx.json(http::status::internal_server_error, "error Internal Server Error");
    }

    auto& resp = ctx.response();

    std::vector<nghttp2_nv> headers;
    std::string status_code = std::to_string(resp.result_int());
    headers.push_back({(uint8_t*)":status", (uint8_t*)status_code.c_str(), 7, status_code.size(), NGHTTP2_NV_FLAG_NONE});
    for (const auto& field : resp) {
        if (field.name() == http::field::connection) continue;
        headers.push_back({(uint8_t*)field.name_string().data(), (uint8_t*)field.value().data(), field.name_string().length(), field.value().length(), NGHTTP2_NV_FLAG_NONE});
    }

    auto content = std::make_shared<std::string>(std::move(resp.body()));
    nghttp2_data_provider provider{};
    std::shared_ptr<ProviderPack> pack = nullptr;
    if (content && !content->empty()) {
        pack = std::make_shared<ProviderPack>();
        pack->content = content;
        provider.source.ptr = pack.get();
        // ... read_callback ...
        provider.read_callback = [](nghttp2_session*, int32_t, uint8_t* buf, size_t len, uint32_t* flags, nghttp2_data_source* src, void*) -> ssize_t {
            auto* p = static_cast<ProviderPack*>(src->ptr);
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

    // 在 dispatch 协程的最后，触发一次写操作
    co_await do_write();
}

/**
 * @brief nghttp2 回调：当一个完整的帧被接收时调用
 */
int Http2Session::on_frame_recv_callback(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
    auto* self = static_cast<Http2Session*>(user_data);
    int32_t sid = frame->hd.stream_id;
    // 我们只关心带有 END_STREAM 标志的帧，因为它代表一个请求的结束
    if (sid != 0 && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        // 确保这个流的上下文还存在
        if (self->streams_.count(sid) > 0) {
            // 启动一个新的协程来处理这个请求，这样不会阻塞主 session_loop
            self->dispatch_channel_.try_send(boost::system::error_code{}, sid);
        }
    }
    return 0;
}

/**
 * @brief nghttp2 回调：当一个请求头名/值对被接收时调用
 */
// in H2Session.cpp and H2cSession.cpp

int Http2Session::on_header_callback(nghttp2_session*, const nghttp2_frame* frame,
                                     const uint8_t* name, size_t namelen,
                                     const uint8_t* value, size_t valuelen,
                                     uint8_t, void* user_data)
{
    auto* self = static_cast<Http2Session*>(user_data); // or H2cSession*
    int32_t sid = frame->hd.stream_id;
    if (sid == 0) return 0;

    if (self->streams_.find(sid) == self->streams_.end()) {
        self->streams_[sid] = {};
    }
    auto& stream_ctx = self->streams_.at(sid);

    // **直接存储原始数据，不做任何解析**
    stream_ctx.headers.emplace_back(
        std::string((const char*)name, namelen),
        std::string((const char*)value, valuelen)
    );

    return 0;
}

/**
 * @brief nghttp2 回调：当一小块请求体数据被接收时调用
 */
int Http2Session::on_data_chunk_recv_callback(nghttp2_session*, uint8_t, int32_t stream_id, const uint8_t* data, size_t len, void* user_data) {
    auto* self = static_cast<Http2Session*>(user_data);
    auto it = self->streams_.find(stream_id);
    if (it != self->streams_.end()) {
        // 将数据块追加到对应流的 body 字符串中
        it->second.body.append((char*)data, len);
    }
    return 0;
}

/**
 * @brief nghttp2 回调：当一个流被关闭时调用
 */
int Http2Session::on_stream_close_callback(nghttp2_session*, int32_t stream_id, uint32_t, void* user_data) {
    auto* self = static_cast<Http2Session*>(user_data);
    // 使用 post 将清理工作调度到 strand 上，以确保线程安全
    boost::asio::post(self->strand_,
                      [self, stream_id]() {
                          // 移除与已关闭流相关的响应数据提供者，释放内存
                          self->provider_pack_.erase(stream_id);
                      });
    return 0;
}



void Http2Session::graceful_shutdown() {
    // 使用 post 确保操作在 session 的 strand 上执行，保证线程安全
    boost::asio::post(strand_,
                      [self = shared_from_this()]() {
                          if (!self->session_ || !self->stream_->next_layer().is_open()) {
                              return; // 如果会话已经关闭，则什么也不做
                          }

                          SPDLOG_INFO("Initiating graceful shutdown for Http2Session...");

                          // 获取最后一个处理的流 ID。nghttp2 会确保ID小于等于此值的流被处理完。
                          int32_t last_stream_id = nghttp2_session_get_last_proc_stream_id(self->session_);

                          // 准备 GOAWAY 帧
                          boost::system::error_code ec;
                          int rv = nghttp2_submit_goaway(
                              self->session_,
                              NGHTTP2_FLAG_NONE,
                              last_stream_id,
                              NGHTTP2_NO_ERROR,
                              // 这是一个正常的关闭，没有错误
                              nullptr,
                              0
                          );

                          if (rv != 0) {
                              SPDLOG_ERROR("nghttp2_submit_goaway() failed: {}", nghttp2_strerror(rv));
                              return;
                          }

                          // 提交 GOAWAY 帧后，需要触发一次写操作来把它发送出去
                          boost::asio::co_spawn(self->strand_, self->do_write(), boost::asio::detached);
                      });
}
