#include "h2_session.hpp"
#include <spdlog/spdlog.h>          // 日志库
#include <cstring>                  // 用于 C 风格字符串操作，如 memcpy
#include <boost/asio/experimental/awaitable_operators.hpp> // Asio 实验性功能，提供了协程操作符，如 || (race) 和 && (parallel)
#include <chrono>                   // C++ 时间库
#include <boost/asio/experimental/parallel_group.hpp> // 引入 parallel_group，一个更强大的并行操作工具（在此代码中未直接使用，但包含进来）
#include "version.hpp"
#include "http/request_context.hpp" // 请求上下文定义
#include "utils/finally.hpp"        // RAII 工具，用于确保在作用域退出时执行代码（如此处的 in_progress 标志位重置）
#include "utils/config/AizixConfig.hpp"


/// @brief Http2Session 构造函数
Http2Session::Http2Session(StreamPtr stream, boost::asio::any_io_executor work_executor, Router& router, size_t max_request_body_size_bytes, const std::chrono::milliseconds keep_alive_timeout)
    : stream_(std::move(stream)), // 移动语义，接管 SSL 流的所有权
      work_executor_(std::move(work_executor)),
      router_(router), // 保存对路由器的引用
      strand_(stream_->get_executor()), // 从流中获取执行器并构造一个 strand，保证所有异步操作都在此 strand 上串行执行，确保线程安全
      session_(nullptr), // 初始化 nghttp2 会话指针为空，将在 init_session() 中创建
      idle_timer_(strand_),
      max_request_body_size_bytes_(max_request_body_size_bytes), // 在同一个 strand 上构造空闲计时器
      keep_alive_ms_(keep_alive_timeout),
      write_trigger_(strand_), // 在同一个 strand 上构造写触发器
      dispatch_channel_(strand_) // 在同一个 strand 上构造分发通道
{
    // 初始化时，将两个定时器都设置为永不超时。
    // idle_timer_ 将在连接变为空闲时被重置。
    // write_trigger_ 将被 cancel() 来唤醒，而不是等待超时。
    idle_timer_.expires_at(std::chrono::steady_clock::time_point::max());
    write_trigger_.expires_at(std::chrono::steady_clock::time_point::max());
    SPDLOG_DEBUG("Create a connection [{}:{}] ", remote_endpoint().address().to_string(), remote_endpoint().port());
}


/// @brief Http2Session 析构函数
Http2Session::~Http2Session() {
    // 释放 nghttp2 会话占用的内存
    if (session_) nghttp2_session_del(session_);
    SPDLOG_DEBUG("Close a connection [{}:{}] ", remote_endpoint().address().to_string(), remote_endpoint().port());
}


/// @brief 初始化 nghttp2 会话实例

void Http2Session::init_session() {
    nghttp2_session_callbacks* callbacks;
    nghttp2_session_callbacks_new(&callbacks); // 创建回调结构体
    // 绑定各种事件的回调函数，这是 nghttp2 与我们 C++ 代码交互的桥梁
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    // 创建一个服务器端 nghttp2 会话实例，并将 this 指针作为用户数据 (user_data) 传递
    // 这样在 C 风格的回调函数中，我们就能通过 user_data 获取到 Http2Session 对象实例
    nghttp2_session_server_new(&session_, callbacks, this);
    nghttp2_session_callbacks_del(callbacks); // 释放回调结构体
}


/// @brief 启动会话的主入口协程

boost::asio::awaitable<void> Http2Session::start() {
    // 引入协程操作符命名空间
    using namespace boost::asio::experimental::awaitable_operators;
    try {
        // 使用 '&&' 操作符并行运行几个核心循环协程。
        // 这个操作会等待所有协程都完成。如果任何一个协程因异常或正常原因退出，
        // 其他协程将会被自动取消，从而优雅地结束整个 session。
        co_await (session_loop() && dispatcher_loop() && writer_loop() && idle_timer_loop());
    } catch (const std::exception& e) {
        // 捕获任何未处理的异常，记录日志
        SPDLOG_DEBUG("H2 session ended: {}", e.what());
    }
}


/// @brief 执行一次底层的写操作

boost::asio::awaitable<void> Http2Session::do_write() {
    try {
        // 检查 nghttp2 引擎是否有数据等待发送
        if (session_ && nghttp2_session_want_write(session_)) {
            const uint8_t* data_ptr = nullptr;
            // 从 nghttp2 的内部发送缓冲区获取数据。这是一个零拷贝操作，data_ptr 直接指向内部缓冲区。
            ssize_t len = nghttp2_session_mem_send(session_, &data_ptr);
            if (len < 0) {
                SPDLOG_ERROR("H2 Server: nghttp2_session_mem_send() failed: {}", nghttp2_strerror(len));
                if (stream_->next_layer().is_open()) stream_->next_layer().close();
                co_return;
            }

            if (len > 0) {
                // 如果有数据，则通过 asio 异步写入到 SSL 流
                co_await boost::asio::async_write(*stream_, boost::asio::buffer(data_ptr, len), boost::asio::use_awaitable);
            }
        }
    } catch (const std::exception& e) {
        // 捕获写操作中的异常（如连接断开），记录日志并关闭套接字
        if (stream_->next_layer().is_open()) {
            boost::system::error_code ignored_ec;
            const auto error_code = stream_->next_layer().close(ignored_ec);
            SPDLOG_WARN("do_write failed, closing socket: {}", error_code.what());
        } else {
            SPDLOG_WARN("H2 Server do_write failed, closing socket: {}", e.what());
        }
    }
}


/// @brief 调度一次写操作，唤醒 writer_loop
void Http2Session::schedule_write() {
    // 这个函数必须在 strand 上下文中被调用，以保证对 write_in_progress_ 和 session_ 的访问是线程安全的。
    // 如果正在写，或者会话已关闭，则直接返回，避免重复触发。
    if (write_in_progress_ || !session_ || !stream_->next_layer().is_open()) {
        return;
    }
    // 如果 nghttp2 有数据要写
    if (nghttp2_session_want_write(session_)) {
        // 立即触发 writer_loop。通过取消定时器的等待，使其 async_wait 立即返回（并带有 operation_aborted 错误码）。
        // 这是一种高效的、无锁的协程唤醒机制。
        SPDLOG_TRACE("释放 write_trigger_ 定时器");
        write_trigger_.cancel_one();
    }
}


/// @brief 写入循环协程
boost::asio::awaitable<void> Http2Session::writer_loop() {
    try {
        // 只要连接处于打开状态，就一直循环
        while (stream_->next_layer().is_open()) {
            boost::system::error_code ec;
            // 挂起协程，等待被 schedule_write() 唤醒。
            co_await write_trigger_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

            // 如果等待被取消 (ec == operation_aborted)，这是正常唤醒，继续执行。
            // 如果是其他错误，则退出循环。
            if (ec && ec != boost::asio::error::operation_aborted) break;
            // 再次检查连接状态
            if (!stream_->next_layer().is_open()) break;


            if (write_in_progress_) continue; // 防止重入
            write_in_progress_ = true;
            // 使用互斥标志和 RAII guard 确保同一时间只有一个 do_write 循环在运行。确保在协程退出时（无论是正常还是异常）都能重置标志位
            auto guard = Finally([this] { write_in_progress_ = false; });

            // ** "清空队列" 循环**
            // 一旦被唤醒，就持续写，直到 nghttp2 的发送缓冲区被清空。
            // 这可以减少 writer_loop 被唤醒的次数，提高效率。
            while (session_ && nghttp2_session_want_write(session_)) {
                co_await do_write();
                // 检查 do_write 是否因为错误关闭了套接字
                if (!stream_->next_layer().is_open()) {
                    co_return;
                }
            }
        }
    } catch (const std::exception& e) {
        // 捕获 writer_loop 自身的异常
        if (stream_->next_layer().is_open()) {
            boost::system::error_code ignored_ec;
            const auto error_code = stream_->next_layer().close(ignored_ec);
            SPDLOG_WARN("H2 writer_loop ended with exception: {}", error_code.what());
        } else {
            SPDLOG_WARN("H2 writer_loop ended with exception: {}", e.what());
        }
    }
}

/// @brief 会话/读取循环协程
boost::asio::awaitable<void> Http2Session::session_loop() {
    // 确保当前协程在 strand 上下文中执行
    co_await boost::asio::dispatch(strand_, boost::asio::use_awaitable);
    init_session(); // 初始化 nghttp2

    // 发送服务器的 SETTINGS 帧，设置最大并发流数量
    nghttp2_settings_entry iv[1] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}};
    nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv, 1);
    schedule_write(); // 调度发送 SETTINGS 帧

    std::array<char, 8192> buf{}; // 读缓冲区
    try {
        while (stream_->next_layer().is_open()) {
            auto [ec, n] = co_await stream_->async_read_some(
                boost::asio::buffer(buf),
                boost::asio::as_tuple(boost::asio::use_awaitable)
            );

            // EOF 或 stream_truncated 是客户端正常关闭，不视为错误。
            if (ec) {
                if (ec != boost::asio::error::eof && ec != boost::asio::ssl::error::stream_truncated) {
                    SPDLOG_DEBUG("read error: {} (value: {}, category: {})", ec.message(), ec.value(), ec.category().name());
                }
                break; // 退出 循环
            }

            // 将从 socket 读取到的数据喂给 nghttp2 引擎进行协议解析
            if (const ssize_t rv = nghttp2_session_mem_recv(session_, reinterpret_cast<const uint8_t*>(buf.data()), n); rv < 0) {
                SPDLOG_ERROR("[{}]: nghttp2_session_mem_recv() failed: {}", remote_endpoint().address().to_string(), nghttp2_strerror(rv));
                break;
            }

            // 接收到数据后，触发写操作，以发送 nghttp2 可能生成的响应帧 (如 PING ACK)。
            schedule_write();
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Exception in H2 session_loop [{}]: {}", remote_endpoint().address().to_string(), e.what());
    }
    // 循环结束后，关闭分发通道，通知 dispatcher_loop 退出。
    dispatch_channel_.close();
    // 同时取消任何可能在等待的空闲计时器。
    idle_timer_.cancel();
}


/// @brief 分发循环协程
boost::asio::awaitable<void> Http2Session::dispatcher_loop() {
    for (;;) {
        // 异步地从 channel 接收一个 stream_id。这里会一直阻塞直到有数据或 channel 关闭。
        auto [ec, stream_id] = co_await dispatch_channel_.async_receive(boost::asio::as_tuple(boost::asio::use_awaitable));
        // Channel 已关闭（通常由 session_loop 退出时关闭），意味着会话结束，我们也应该退出。
        if (ec) break;

        // 收到一个新请求，活跃流计数 +1
        if (++active_streams_ == 1) {
            // 如果连接从空闲变为繁忙 (0 -> 1)，取消空闲计时器
            idle_timer_.cancel();
        }

        // 对于每个接收到的 stream_id，co_spawn 一个独立的协程来处理它。
        // 这使得请求处理可以并行化，而不会阻塞 dispatcher_loop 接收下一个请求。
        // boost::asio::detached 表示我们不关心这个新协程的完成或返回值。
        boost::asio::co_spawn(strand_, dispatch(stream_id), boost::asio::detached);
    }
}

/// @brief 处理单个请求的协程
boost::asio::awaitable<void> Http2Session::dispatch(int32_t stream_id) {
    // 使用 RAII guard 确保协程退出时（无论正常还是异常）都能将计数 -1
    auto guard = Finally([this] {
        if (--active_streams_ == 0) {
            // 如果连接从繁忙变为空闲 (1 -> 0)，取消定时器以重置它
            idle_timer_.cancel();
        }
    });
    // 查找并获取此 stream_id 对应的上下文
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) co_return; // 如果找不到，可能已被处理或出错，直接返回
    // --- 检查流是否在接收完成前就被拒绝了 ---
    if (it->second.is_rejected) {
        // 如果流已被拒绝，我们只需将其从 map 中移除即可，
        // 因为 413 响应已经在回调中发送了。
        streams_.erase(it);
        co_return;
    }
    // 移动上下文内容，然后从 map 中删除，避免重复处理和内存泄漏
    StreamContext stream_ctx = std::move(it->second);
    streams_.erase(it);
    // --- 将 nghttp2 的原始头数据转换为 Beast HTTP 请求对象 ---
    HttpRequest req;
    req.version(20); // HTTP/2.0
    for (const auto& pair : stream_ctx.headers) {
        const std::string& key = pair.first;
        const std::string& val = pair.second;
        if (key.empty()) continue;
        if (key[0] == ':') {
            // 处理 HTTP/2 伪头字段
            if (key == ":method") req.method(http::string_to_verb(val));
            else if (key == ":path") req.target(val);
            else if (key == ":authority") req.set(http::field::host, val);
        } else {
            // 处理普通头字段
            req.set(key, val);
        }
    }
    req.body() = std::move(stream_ctx.body); // 移动请求体
    req.prepare_payload(); // 计算 Content-Length 等
    // --- 路由和处理 ---
    RouteMatch match = router_.dispatch(req.method(), req.target());
    RequestContext ctx(std::move(req), std::move(match.path_params));
    try {
        // 调用匹配到的业务逻辑处理函数
        co_await match.handler(ctx);
    } catch (const std::exception& e) {
        // 捕获业务逻辑中的异常，并返回一个 500 错误
        SPDLOG_ERROR("Exception in H2 handler for [{}]: {}", ctx.request().target(), e.what());
        ctx.json(http::status::internal_server_error, "error Internal Server Error");
    }
    // --- 准备并提交响应 ---
    auto& resp = ctx.response();
    co_await ctx.compressIfAcceptable(work_executor_); // 压缩响应体
    resp.prepare_payload(); // 准备响应，计算 Content-Length
    // 将 Beast HTTP 响应头转换为 nghttp2 的 nghttp2_nv 格式
    std::vector<nghttp2_nv> headers;
    std::string status_code = std::to_string(resp.result_int());
    headers.push_back({(uint8_t*)":status", (uint8_t*)status_code.c_str(), 7, status_code.size(), NGHTTP2_NV_FLAG_NONE});
    for (const auto& field : resp) {
        if (field.name() == http::field::connection) continue; // HTTP/2 中不使用 Connection 头
        headers.push_back({(uint8_t*)field.name_string().data(), (uint8_t*)field.value().data(), field.name_string().length(), field.value().length(), NGHTTP2_NV_FLAG_NONE});
    }
    // --- 设置响应体数据提供者 ---
    auto content = std::make_shared<std::string>(std::move(resp.body()));
    nghttp2_data_provider provider{};
    std::shared_ptr<ProviderPack> pack = nullptr;
    if (content && !content->empty()) {
        pack = std::make_shared<ProviderPack>();
        pack->content = content;
        provider.source.ptr = pack.get();
        // 设置 nghttp2 的数据读取回调函数。当 nghttp2 需要发送响应体数据时，会调用这个 lambda。
        provider.read_callback = [](nghttp2_session*, int32_t, uint8_t* buf, size_t len, uint32_t* flags, nghttp2_data_source* src, void*) -> ssize_t {
            auto* p = static_cast<ProviderPack*>(src->ptr);
            if (!p || !p->content) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
            size_t remaining = p->content->size() - p->offset;
            size_t n = std::min(len, remaining); // 计算本次能发送多少数据
            if (n > 0) {
                memcpy(buf, p->content->data() + p->offset, n);
                p->offset += n;
            }
            if (p->offset == p->content->size()) {
                // 如果数据全部发送完毕
                *flags |= NGHTTP2_DATA_FLAG_EOF; // 设置 EOF 标志
            }
            return n;
        };
        // 保存 pack 的 shared_ptr，确保在异步发送过程中响应体数据不会被释放
        provider_pack_[stream_id] = pack;
    }
    // 向 nghttp2 提交响应头和数据提供者
    nghttp2_submit_response(session_, stream_id, headers.data(), headers.size(), (pack ? &provider : nullptr));
    // 调度一次写操作，将响应数据发送出去
    schedule_write();
}

/// @brief nghttp2 回调：当一个完整的帧被接收时调用
int Http2Session::on_frame_recv_callback(nghttp2_session*, const nghttp2_frame* frame, void* user_data) {
    auto* self = static_cast<Http2Session*>(user_data);
    int32_t sid = frame->hd.stream_id;
    // 我们只关心带有 END_STREAM 标志的 HEADERS 或 DATA 帧，因为它代表一个请求的接收已完成
    if (sid != 0 && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        // 确保这个流的上下文还存在（可能之前已因错误关闭）
        if (self->streams_.count(sid) > 0) {
            // 通过 channel 将 stream_id 发送给 dispatcher_loop 进行处理。
            // try_send 是非阻塞的，对于 Asio channel 来说通常是安全的。
            self->dispatch_channel_.try_send(boost::system::error_code{}, sid);
        }
    }
    return 0; // 返回 0 表示成功
}

/// @brief nghttp2 回调：当一个 header 键值对被接收时调用
int Http2Session::on_header_callback(nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, size_t name_len, const uint8_t* value, size_t value_len, uint8_t, void* user_data) {
    auto* self = static_cast<Http2Session*>(user_data);
    int32_t sid = frame->hd.stream_id;
    if (sid == 0) return 0; // 忽略连接控制流（stream_id 0）的头

    // 1. 我们需要复制一份 header 数据，因为原始的 name/value 指针
    //    在回调函数返回后可能就失效了。
    std::string name_str(reinterpret_cast<const char*>(name), name_len);
    std::string value_str(reinterpret_cast<const char*>(value), value_len);

    // 2. 使用 post 将状态修改调度到 strand 上。
    // 这对于多线程 io_context 是绝对安全的。
    boost::asio::post(self->strand_, [self, sid, n = std::move(name_str), v = std::move(value_str)]() {
        // 在收到第一个 header 时，就认为新流开始了
        if (!self->streams_.contains(sid)) {
            // 是新流，为它创建上下文
            self->streams_[sid] = {};
        }

        // 获取流的上下文并添加 header
        // 我们在这里用 .at() 是安全的，因为我们刚确保了它存在
        self->streams_.at(sid).headers.emplace_back(n, v);
    });
    return 0;
}


/// @brief nghttp2 回调：当一个 DATA 帧的数据块被接收时调用
int Http2Session::on_data_chunk_recv_callback(nghttp2_session*, uint8_t, int32_t stream_id, const uint8_t* data, size_t len, void* user_data) {
    auto* self = static_cast<Http2Session*>(user_data);
    const auto it = self->streams_.find(stream_id);
    if (it == self->streams_.end()) {
        return 0; // 流不存在，忽略
    }
    auto& stream_ctx = it->second;
    // 如果此流已被拒绝，则不再接收任何数据
    if (stream_ctx.is_rejected) {
        return 0;
    }
    // --- 检查大小 ---
    if (stream_ctx.body_size + len > self->max_request_body_size_bytes_) {
        SPDLOG_WARN("H2 request body too large for stream_id {}. Rejecting stream.", stream_id);
        // 标记为已拒绝
        stream_ctx.is_rejected = true;

        // 1. 提交 413 响应头。这比直接 RST_STREAM 更友好。
        std::vector<nghttp2_nv> headers;
        const std::string status_code = "413";
        const std::string server_name = aizix::framework::name + "/" + aizix::framework::version;
        headers.push_back({(uint8_t*)":status", (uint8_t*)status_code.c_str(), 7, status_code.size(), NGHTTP2_NV_FLAG_NONE});
        headers.push_back({(uint8_t*)"server", (uint8_t*)server_name.c_str(), 6, server_name.length(), NGHTTP2_NV_FLAG_NONE});
        nghttp2_submit_response(self->session_, stream_id, headers.data(), headers.size(), nullptr);

        // 2. 调度写入，尽快发送 413 响应
        boost::asio::post(self->strand_, [self]() {
            self->schedule_write();
        });

        // 3. 返回一个错误码告诉 nghttp2 我们不希望再接收此流的数据
        //    这会导致 nghttp2 内部发送一个 RST_STREAM 帧
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    // 如果未超限，则正常追加数据
    stream_ctx.body.append(reinterpret_cast<const char*>(data), len);
    stream_ctx.body_size += len;
    return 0;
}

/**
 * @brief nghttp2 回调：当一个流被关闭时调用
 *
 * 这个回调在 nghttp2 内部认为一个流已经结束时被触发（无论是正常结束还是因错误被重置）。
 * 在我们重构后的模型中，它的核心职责是清理与该流相关的服务器端资源，
 * 尤其是为响应体分配的内存。
 *
 * @param session nghttp2 会话指针。
 * @param stream_id 已关闭的流的 ID。
 * @param error_code 关闭的原因码（例如 NGHTTP2_NO_ERROR 表示正常关闭）。
 * @param user_data 指向我们 Http2Session 实例的指针。
 * @return 总是返回 0，表示成功处理。
 */
int Http2Session::on_stream_close_callback(nghttp2_session* session, int32_t stream_id, uint32_t error_code, void* user_data) {
    auto* self = static_cast<Http2Session*>(user_data);

    SPDLOG_TRACE("H2 Stream {} closed with code: {} ({})", stream_id, error_code, nghttp2_strerror(error_code));

    // 关键：清理与此流相关的响应体数据提供者 (ProviderPack)。
    // 如果不清理，provider_pack_ 这个 map 会无限增长，导致内存泄漏。
    //
    // 为什么需要 post 到 strand 上？
    // 因为 nghttp2 的回调可能在任何线程（通常是网络IO线程）上被调用，
    // 而 provider_pack_ 和 streams_ 是被多个协程共享的资源。
    // 通过 post 到 strand_，我们确保了对这些 map 的擦除操作是线程安全的，
    // 不会与 dispatch 协程中对它们的访问或修改操作发生数据竞争。
    boost::asio::post(self->strand_, [self, stream_id, error_code]() {
        // 这个 lambda 会在 strand 的执行上下文中被安全地调用。

        // 1. 清理响应体数据提供者。
        //    这是此回调最重要的职责。
        self->provider_pack_.erase(stream_id);

        // 2. [可选但推荐] 清理可能残留的流上下文。
        //    正常情况下，`dispatch` 协程会在处理完请求后就将流上下文
        //    从 `streams_` map 中移除。但是，在某些异常情况下
        //    （例如，客户端在发送完 HEADERS 后立即发送 RST_STREAM），
        //    `dispatch` 可能还未执行或未完成，但流已经关闭了。
        //    在这里进行一次清理可以防止内存泄漏。
        self->streams_.erase(stream_id);
    });
    return 0; // 按照 nghttp2 API 要求，返回 0 表示成功
}

/// @brief 优雅关闭的公共接口协程
boost::asio::awaitable<void> Http2Session::graceful_shutdown(uint32_t error_code) {
    // 确保我们总是在 strand 上下文中执行此操作。
    // 如果调用者已经在 strand 上，dispatch 是一个 no-op（无操作）。
    // 如果不在，它会将协程的剩余部分 post 到 strand 上执行。
    co_await boost::asio::dispatch(strand_, boost::asio::use_awaitable);
    // 检查是否已经关闭或正在关闭，防止重复操作
    if (!session_ || !stream_->next_layer().is_open()) {
        co_return;
    }
    // 获取最后一个由服务器处理的流 ID。客户端会重试 ID 大于此值的请求。
    int32_t last_stream_id = nghttp2_session_get_last_proc_stream_id(session_);
    // 提交 GOAWAY 帧给 nghttp2 引擎
    int rv = nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE, last_stream_id, error_code, nullptr, 0);
    if (rv != 0) {
        SPDLOG_ERROR("nghttp2_submit_goaway() failed: {}", nghttp2_strerror(rv));
        // 即使提交失败，我们也要继续尝试关闭
    }
    // 触发 writer_loop 来发送这个 GOAWAY 帧。
    // 因为我们已经在 strand 上，所以可以直接调用。
    schedule_write();
    // 注意：我们不在此处等待写操作完成。关闭流程的其他部分（如 session_loop 退出）会处理后续的 socket 关闭。
}

/// @brief 获取远程客户端的端点信息
boost::asio::ip::tcp::endpoint Http2Session::remote_endpoint() const {
    try {
        // stream_ 是 ssl::stream，其 next_layer() 是 tcp::socket。
        return stream_->next_layer().remote_endpoint();
    } catch (const boost::system::system_error&) {
        // 如果 socket 已关闭，调用 remote_endpoint() 会抛出异常。
        // 在这种情况下，返回一个默认构造的、无效的 endpoint。
        return {};
    }
}


boost::asio::awaitable<void> Http2Session::idle_timer_loop() {
    try {
        while (stream_->next_layer().is_open()) {
            // 1. 根据当前状态决定等待策略
            if (active_streams_ > 0) {
                // 如果有活跃请求，永远等待，直到被 cancel() 唤醒
                idle_timer_.expires_at(std::chrono::steady_clock::time_point::max());
            } else {
                // 如果没有活跃请求，设置空闲超时
                idle_timer_.expires_after(keep_alive_ms_);
            }

            boost::system::error_code ec;
            co_await idle_timer_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

            if (ec == boost::asio::error::operation_aborted) {
                // 被取消是正常行为（状态改变），继续循环以重新评估状态
                continue;
            }
            if (ec) {
                // 定时器自身出错
                break;
            }

            // 2. 定时器正常到期，再次检查状态（防御性编程）
            if (active_streams_ == 0) {
                SPDLOG_INFO("H2 connection timed out. Shutting down.");
                co_await graceful_shutdown(NGHTTP2_NO_ERROR);
                // 优雅关闭会最终导致其他循环退出，此循环也会被取消
            }
            // 如果超时后发现又有新流了，就继续循环，啥也不做
        }
    } catch (const std::exception& e) {
        SPDLOG_DEBUG("idle_timer_loop ended with exception: {}", e.what());
    }
}
