//
// Created by Aiziboy on 2025/7/16.
//

#include "h2c_session.hpp"
#include <spdlog/spdlog.h>          // 日志库
#include <cstring>                  // 用于 C 风格字符串操作，如 memcpy
#include <boost/asio/experimental/awaitable_operators.hpp> // Asio 实验性功能，提供了协程操作符，如 ||
#include <chrono>                   // C++ 时间库

#include "http/request_context.hpp"

// 将 C++14 的时间字面量（如 30s）引入当前作用域
using namespace std::literals::chrono_literals;

/**
 * @brief H2cSession 构造函数
 */
H2cSession::H2cSession(SocketPtr socket, Router& router)
    : socket_(std::move(socket)), // 移动语义，接管 Socket 的所有权
      router_(router), // 保存对路由器的引用
      strand_(socket_->get_executor()), // 从流中获取执行器并构造一个 strand，保证线程安全
      session_(nullptr), // 初始化 nghttp2 会话指针为空
      idle_timer_(strand_) // 在同一个 strand 上构造空闲计时器

{
    // 初始时，将计时器设置为永不超时
    idle_timer_.expires_at(std::chrono::steady_clock::time_point::max());
}

/**
 * @brief H2cSession 析构函数
 *        在对象销毁时，安全地释放 nghttp2 会话资源
 */
H2cSession::~H2cSession() {
    if (session_) nghttp2_session_del(session_);


}

/**
 * @brief 初始化 nghttp2 会话，设置所有必需的回调函数
 */
void H2cSession::init_session() {
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
void H2cSession::start() {
    boost::asio::co_spawn(
        strand_,
        [self = shared_from_this()]() -> boost::asio::awaitable<void> {
            // 使用 Asio 的 || (or) 操作符，这是一个非常优雅的超时处理模式。
            // 它会同时“监听” session_loop 和 idle_timer_ 的 async_wait。
            // 哪个先完成，就会取消另一个，并结束整个 co_await 表达式。
            using namespace boost::asio::experimental::awaitable_operators;
            try {
                co_await (self->session_loop() || self->idle_timer_.async_wait(boost::asio::use_awaitable));
            } catch (const std::exception&) {
                // 当超时发生 (idle_timer_ 先完成) 或 session_loop 抛出异常时，会进入这里。
                // 唯一的任务就是安全地关闭底层套接字，释放所有资源。
                boost::system::error_code ec;
                self->socket_->close();
            }
        },
        boost::asio::detached
    );
}

/**
 * @brief 执行写操作的协程
 *        从 nghttp2 的发送缓冲区取出数据并写入网络
 */
boost::asio::awaitable<void> H2cSession::do_write() {
    try
    {
        // 只要 nghttp2 告诉我们它有数据想发送，就一直循环
        while (nghttp2_session_want_write(session_))
        {
            const uint8_t* data_ptr = nullptr;
            // 从 nghttp2 获取待发送数据块的指针和长度
            ssize_t len = nghttp2_session_mem_send(session_, &data_ptr);
            if (len <= 0) break; // 如果没有数据或出错，则退出循环

            // 异步写入数据。co_await 在这里提供了最关键的、天然的背压控制。
            // 在数据被完全写入（或进入内核缓冲区）之前，协程会在此挂起，
            // 不会去拉取下一个数据块，从而完美地避免了撑爆 OpenSSL 缓冲区的问题。
            co_await boost::asio::async_write(*socket_, boost::asio::buffer(data_ptr, len), boost::asio::use_awaitable);
        }
    } catch (const std::exception&)
    {
        // 如果写入时发生任何错误（如连接被对方重置），关闭套接字以终止整个会话。
        socket_->close();
    }
}

/**
 * @brief 会话的主事件循环协程 (融合模型)
 */
boost::asio::awaitable<void> H2cSession::session_loop() {
    try
    {
        // 确保在 strand 上下文中执行
        co_await boost::asio::dispatch(strand_, boost::asio::use_awaitable);

        init_session();

        // 定义一个常量作为空闲超时时间
        constexpr auto timeout = 30s;


        // 创建一个用于读取数据的缓冲区
        std::array<char, 8192> buf{};
        // 进入主循环，只要套接字还打开着就一直运行
        while (socket_->is_open())
        {
            // 在每次准备读取数据前，重置空闲计时器。这是超时机制的核心。
            idle_timer_.expires_after(timeout);
            // 使用 as_tuple 和结构化绑定，以非异常的方式异步读取数据。
            // 这能让我们优雅地处理 EOF 等正常关闭情况。
            auto [ec, n] = co_await socket_->async_read_some(boost::asio::buffer(buf), boost::asio::as_tuple(boost::asio::use_awaitable));

            // 如果有错误（包括 EOF），则退出循环
            if (ec) break;

            // 成功读取到数据后，立即取消计时器。
            // 因为接下来我们会进行 CPU密集型 的处理和可能的写操作，
            // 在这期间不应该计算为空闲时间。
            idle_timer_.cancel();

            // 将读取到的原始数据喂给 nghttp2 进行解析
            ssize_t rv = nghttp2_session_mem_recv(session_, (const uint8_t*)buf.data(), n);
            if (rv < 0) break; // 如果 nghttp2 解析出错，也退出循环

            // **核心逻辑**：在每次成功读取并处理后，立即调用 do_write()。
            // 这确保了因读取而产生的任何需要发送的帧（如 SETTINGS ACK, WINDOW_UPDATE, PING ACK）
            // 都能被及时发送出去，从而打破了所有潜在的死锁。
            co_await do_write();
        }
    } catch (const std::exception&)
    {
        boost::system::error_code ec;
        socket_->close(ec);
    }
}

/**
 * @brief 请求分发和处理协程
 */
boost::asio::awaitable<void> H2cSession::dispatch(int32_t stream_id) {
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        co_return;
    }

    StreamContext stream_ctx = std::move(it->second);
    streams_.erase(it);

    // --- 1. [已修复] 从 StreamContext 正确组装 HttpRequest ---
    HttpRequest req;
    req.version(20);
    for (const auto& pair : stream_ctx.headers) {
        const std::string& key = pair.first;
        const std::string& val = pair.second;

        if (key.empty()) continue;

        if (key[0] == ':') { // 处理伪头
            if (key == ":method") {
                req.method(http::string_to_verb(val));
            } else if (key == ":path") {
                req.target(val);
            } else if (key == ":authority") {
                req.set(http::field::host, val);
            }
            // 明确忽略 :scheme 和其他未知伪头
        } else { // 处理普通头
            req.set(key, val);
        }
    }

    // c. 设置 body
    req.body() = std::move(stream_ctx.body);
    req.prepare_payload();

    // --- 2. 后续的路由、上下文创建、handler 调用逻辑保持不变 ---
    RouteMatch match = router_.dispatch(req.method(), req.target());
    RequestContext ctx(std::move(req), std::move(match.path_params));

    try {
        co_await match.handler(ctx);
    } catch (const std::exception& e) {
        spdlog::error("Exception in H2 handler for [{}]: {}",
                      ctx.request().target(), e.what());
        ctx.json(http::status::internal_server_error, "error Internal Server Error");
    }

    // --- 3. 组装并发送 nghttp2 响应（这部分逻辑无需改动）---
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
        pack->offset = 0; // 确保 offset 被初始化
        provider.source.ptr = pack.get();

        // --- 对 read_callback 的性能优化建议 ---
        provider.read_callback = [](nghttp2_session*, int32_t, uint8_t* buf, size_t len, uint32_t* flags, nghttp2_data_source* src, void*) -> ssize_t {
            auto* p = static_cast<ProviderPack*>(src->ptr);
            if (!p || !p->content) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;

            size_t remaining = p->content->size() - p->offset;
            size_t n = std::min(len, remaining);

            if (n > 0) {
                memcpy(buf, p->content->data() + p->offset, n);
                p->offset += n;
            }

            if (p->offset == p->content->size()) {
                *flags |= NGHTTP2_DATA_FLAG_EOF;
            }
            return n;
        };
        provider_pack_[stream_id] = pack;
    }

    nghttp2_submit_response(session_, stream_id, headers.data(), headers.size(), (pack ? &provider : nullptr));
}

/**
 * @brief nghttp2 回调：当一个完整的帧被接收时调用
 */
int H2cSession::on_frame_recv_callback(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
    auto* self = static_cast<H2cSession*>(user_data);
    int32_t sid = frame->hd.stream_id;
    // 我们只关心带有 END_STREAM 标志的帧，因为它代表一个请求的结束
    if (sid != 0 && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
    {
        // 确保这个流的上下文还存在
        if (self->streams_.count(sid) > 0)
        {
            // 启动一个新的协程来处理这个请求，这样不会阻塞主 session_loop
            boost::asio::co_spawn(self->strand_, self->dispatch(sid), boost::asio::detached);
        }
    }
    return 0;
}

/**
 * @brief nghttp2 回调：当一个请求头名/值对被接收时调用
 */
int H2cSession::on_header_callback(nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, size_t namelen, const uint8_t* value, size_t valuelen, uint8_t, void* user_data) {
    auto* self = static_cast<H2cSession*>(user_data); // or H2cSession*
    int32_t sid = frame->hd.stream_id;
    if (sid == 0) return 0;

    if (self->streams_.find(sid) == self->streams_.end()) {
        self->streams_[sid] = {};
    }
    auto& stream_ctx = self->streams_.at(sid);

    std::string_view key((const char*)name, namelen);
    std::string_view val((const char*)value, valuelen);

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
int H2cSession::on_data_chunk_recv_callback(nghttp2_session*, uint8_t, int32_t stream_id, const uint8_t* data, size_t len, void* user_data) {
    auto* self = static_cast<H2cSession*>(user_data);
    auto it = self->streams_.find(stream_id);
    if (it != self->streams_.end())
    {
        // 将数据块追加到对应流的 body 字符串中
        it->second.body.append((char*)data, len);
    }
    return 0;
}

/**
 * @brief nghttp2 回调：当一个流被关闭时调用
 */
int H2cSession::on_stream_close_callback(nghttp2_session*, int32_t stream_id, uint32_t, void* user_data) {
    auto* self = static_cast<H2cSession*>(user_data);
    // 使用 post 将清理工作调度到 strand 上，以确保线程安全
    boost::asio::post(self->strand_, [self, stream_id]() {
        // 移除与已关闭流相关的响应数据提供者，释放内存
        self->provider_pack_.erase(stream_id);
    });
    return 0;
}

// ======================= Http2Session.cpp (增加优雅关闭实现) =======================
// ... (其他函数不变) ...

void H2cSession::graceful_shutdown()
{
    // 使用 post 确保操作在 session 的 strand 上执行，保证线程安全
    boost::asio::post(strand_, [self = shared_from_this()]() {
        if (!self->session_ || !self->socket_->is_open()) {
            return; // 如果会话已经关闭，则什么也不做
        }

        SPDLOG_INFO("Initiating graceful shutdown for H2cSession...");

        // 获取最后一个处理的流 ID。nghttp2 会确保ID小于等于此值的流被处理完。
        int32_t last_stream_id = nghttp2_session_get_last_proc_stream_id(self->session_);

        // 准备 GOAWAY 帧
        boost::system::error_code ec;
        int rv = nghttp2_submit_goaway(
            self->session_,
            NGHTTP2_FLAG_NONE,
            last_stream_id,
            NGHTTP2_NO_ERROR, // 这是一个正常的关闭，没有错误
            nullptr, 0
        );

        if (rv != 0) {
            SPDLOG_ERROR("nghttp2_submit_goaway() failed: {}", nghttp2_strerror(rv));
            return;
        }

        // 提交 GOAWAY 帧后，需要触发一次写操作来把它发送出去
        boost::asio::co_spawn(self->strand_, self->do_write(), boost::asio::detached);
    });
}
