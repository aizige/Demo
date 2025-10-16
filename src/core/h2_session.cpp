#include "h2_session.hpp"
#include <spdlog/spdlog.h>          // 日志库
#include <cstring>                  // 用于 C 风格字符串操作，如 memcpy
#include <boost/asio/experimental/awaitable_operators.hpp> // Asio 实验性功能，提供了协程操作符，如 || (race) 和 && (parallel)
#include <chrono>                   // C++ 时间库
#include <boost/asio/experimental/parallel_group.hpp> // 引入 parallel_group，一个更强大的并行操作工具（在此代码中未直接使用，但包含进来）
#include "http/request_context.hpp" // 请求上下文定义
#include "utils/finally.hpp"        // RAII 工具，用于确保在作用域退出时执行代码（如此处的 in_progress 标志位重置）

// 将 C++14 的时间字面量（如 30s）引入当前作用域，方便书写时间
using namespace std::literals::chrono_literals;

/**
 * @brief Http2Session 构造函数
 */
Http2Session::Http2Session(StreamPtr stream, Router &router)
    : stream_(std::move(stream)), // 移动语义，接管 SSL 流的所有权
      router_(router), // 保存对路由器的引用
      strand_(stream_->get_executor()), // 从流中获取执行器并构造一个 strand，保证所有异步操作都在此 strand 上串行执行，确保线程安全
      session_(nullptr), // 初始化 nghttp2 会话指针为空，将在 init_session() 中创建
      idle_timer_(strand_), // 在同一个 strand 上构造空闲计时器
      write_trigger_(strand_), // 在同一个 strand 上构造写触发器
      dispatch_channel_(strand_) // 在同一个 strand 上构造分发通道
{
    // 初始时，将计时器设置为永不超时，避免在没有操作时立即触发
    idle_timer_.expires_at(std::chrono::steady_clock::time_point::max());
    write_trigger_.expires_at(std::chrono::steady_clock::time_point::max());
    SPDLOG_DEBUG("Create a connection [{}:{}] ", remote_endpoint().address().to_string(),remote_endpoint().port());
}


/**
 * @brief Http2Session 析构函数
 */
Http2Session::~Http2Session() {
    // 释放 nghttp2 会话占用的内存
    if (session_) nghttp2_session_del(session_);
    SPDLOG_DEBUG("Close a connection [{}:{}] ", remote_endpoint().address().to_string(),remote_endpoint().port());
}

/**
 * @brief 初始化 nghttp2 会话实例
 */
void Http2Session::init_session() {
    nghttp2_session_callbacks *callbacks;
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


/**
 * @brief 启动会话的主入口协程
 */
boost::asio::awaitable<void> Http2Session::start() {
    // 引入协程操作符命名空间
    using namespace boost::asio::experimental::awaitable_operators;
    try {
        // 使用 '&&' 操作符并行运行所有三个核心循环协程。
        // 这个操作会等待所有协程都完成。如果任何一个协程因异常或正常原因退出，
        // 其他协程将会被自动取消，从而优雅地结束整个 session。
        co_await (session_loop() && dispatcher_loop() && writer_loop());
    } catch (const std::exception &e) {
        // 捕获任何未处理的异常，记录日志
        SPDLOG_DEBUG("H2 session ended: {}", e.what());
    }
}


/**
 * @brief 执行一次底层的写操作
 */
boost::asio::awaitable<void> Http2Session::do_write() {
    try {
        // 检查 nghttp2 引擎是否有数据等待发送
        if (session_ && nghttp2_session_want_write(session_)) {
            const uint8_t *data_ptr = nullptr;
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
    } catch (const std::exception &e) {
        // 捕获写操作中的异常（如连接断开），记录日志并关闭套接字
        SPDLOG_WARN("H2 Server do_write failed, closing socket: {}", e.what());
        if (stream_->next_layer().is_open()) {
            boost::system::error_code ignored_ec;
            stream_->next_layer().close(ignored_ec);
        }
    }
}

/**
 *  @brief 调度一次写操作，唤醒 writer_loop
 */
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
        SPDLOG_DEBUG("释放 write_trigger_ 定时器");
        write_trigger_.cancel_one();
    }
}


/**
 * @brief 写入循环协程
 */
boost::asio::awaitable<void> Http2Session::writer_loop() {
    try {
        // 只要连接处于打开状态，就一直循环
        while (stream_->next_layer().is_open()) {
            boost::system::error_code ec;
            // 等待被 schedule_write() 唤醒。正常情况下，这里会一直阻塞。
            co_await write_trigger_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

            // 如果等待被取消 (ec == operation_aborted)，这是正常唤醒，继续执行。
            // 如果是其他错误，则退出循环。
            if (ec && ec != boost::asio::error::operation_aborted) {
                break;
            }
            // 再次检查连接状态
            if (!stream_->next_layer().is_open()) {
                break;
            }

            if (write_in_progress_) continue; // 防止重入
            write_in_progress_ = true;
            // 使用 RAII guard 确保在协程退出时（无论是正常还是异常）都能重置标志位
            auto guard = Finally([this] { write_in_progress_ = false; });

            // **关键的 "清空队列" 循环**
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
    } catch (const std::exception &e) {
        // 捕获 writer_loop 自身的异常
        SPDLOG_WARN("H2 writer_loop ended with exception: {}", e.what());
        if (stream_->next_layer().is_open()) {
            boost::system::error_code ignored_ec;
            stream_->next_layer().close(ignored_ec);
        }
    }
}



/**
 * @brief 会话/读取循环协程
 */
boost::asio::awaitable<void> Http2Session::session_loop() {
    // 确保当前协程在 strand 上下文中执行
    co_await boost::asio::dispatch(strand_, boost::asio::use_awaitable);
    init_session(); // 初始化 nghttp2

    // 为新连接的第一个请求设置一个较短的超时，防止客户端连接后不发数据 TODO:来自配置文件
    constexpr auto initial_timeout = 10s;
    // 为已建立的 keep-alive 空闲连接设置一个较长的超时
    constexpr auto keep_alive_timeout = 60s;

    // 发送服务器的 SETTINGS 帧，设置最大并发流数量
    nghttp2_settings_entry iv[1] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}};
    nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv, 1);
    schedule_write(); // 调度发送 SETTINGS 帧

    std::array<char, 8192> buf{}; // 读缓冲区

    using namespace boost::asio::experimental::awaitable_operators;

    idle_timer_.expires_after(initial_timeout); // 启动初始超时计时
    SPDLOG_DEBUG("第一次设置 idle_timer_ 定时器");

    while (stream_->next_layer().is_open()) {
        // 使用 '||' 操作符同时等待“读操作完成”和“定时器超时”这两个事件，哪个先发生就继续执行。
        // as_tuple 将协程的返回值包装成 std::tuple，方便后续处理。
        auto read_op = stream_->async_read_some(
            boost::asio::buffer(buf),
            boost::asio::as_tuple(boost::asio::use_awaitable)
        );
        auto timer_op = idle_timer_.async_wait(
            boost::asio::as_tuple(boost::asio::use_awaitable)
        );

        // co_await 等待其中一个操作完成
        auto result = co_await(std::move(read_op) || std::move(timer_op));

        if (result.index() == 1) { // --- 超时发生 ---
            auto [ec_timer] = std::get<1>(result);
            if (!ec_timer) { // 如果不是因为取消而超时
                SPDLOG_DEBUG("H2 connection idle timeout. Sending GOAWAY.");
                // 调用优雅关闭协程，通知客户端连接即将关闭
                co_await graceful_shutdown(NGHTTP2_NO_ERROR);
            } else if (ec_timer != boost::asio::error::operation_aborted) {
                SPDLOG_WARN("H2 timer error: {}", ec_timer.message());
            }
            dispatch_channel_.close(); // 关闭 channel，通知 dispatcher_loop 退出
            break; // 退出 session_loop
        }

        // --- 读取成功或读取出错 ---
        idle_timer_.cancel_one(); // 读取成功，取消当前的空闲计时器
        SPDLOG_DEBUG("读取成功取消 idle_timer_ 定时器");
        auto [ec, n] = std::get<0>(result); // 获取读操作的结果

        // 检查读取操作的 error_code
        if (ec) {
            if (ec != boost::asio::error::eof) { // EOF 是客户端正常关闭连接，不算错误
                SPDLOG_WARN("H2 session_loop read error: {} (value: {}, category: {})",
                   ec.message(), ec.value(), ec.category().name());
            }
            dispatch_channel_.close(); // 关闭 channel，通知 dispatcher_loop 退出
            break; // 退出 session_loop
        }


        // 读取成功，重置计时器为 keep-alive 超时
        SPDLOG_DEBUG("为下一次读取设置 idle_timer_ 定时器");
        idle_timer_.expires_after(keep_alive_timeout);

        // 将从 socket 读取到的数据喂给 nghttp2 引擎进行协议解析
        const ssize_t rv = nghttp2_session_mem_recv(session_, reinterpret_cast<const uint8_t*>(buf.data()), n);
        if (rv < 0) {
            SPDLOG_ERROR("nghttp2_session_mem_recv() failed: {}", nghttp2_strerror(rv));
            dispatch_channel_.close();
            break;
        }

        // 接收数据后，nghttp2 可能会产生响应帧（如 PING 的 ACK），需要调度写操作
        schedule_write();
    }

    idle_timer_.cancel(); // 循环结束，确保计时器被取消
    SPDLOG_DEBUG("离开 session_loop 取消 idle_timer_ 定时器");
}

/**
 * @brief 分发循环协程
 */
boost::asio::awaitable<void> Http2Session::dispatcher_loop() {
    for (;;) {
        // 异步地从 channel 接收一个 stream_id。这里会一直阻塞直到有数据或 channel 关闭。
        auto [ec, stream_id] = co_await dispatch_channel_.async_receive(boost::asio::as_tuple(boost::asio::use_awaitable));
        if (ec) {
            // Channel 已关闭（通常由 session_loop 退出时关闭），意味着会话结束，我们也应该退出。
            break;
        }
        // 对于每个接收到的 stream_id，co_spawn 一个独立的协程来处理它。
        // 这使得请求处理可以并行化，而不会阻塞 dispatcher_loop 接收下一个请求。
        // boost::asio::detached 表示我们不关心这个新协程的完成或返回值。
        boost::asio::co_spawn(strand_, dispatch(stream_id), boost::asio::detached);
    }
}

/**
 * @brief 处理单个请求的协程
 */
boost::asio::awaitable<void> Http2Session::dispatch(int32_t stream_id) {
    // 查找并获取此 stream_id 对应的上下文
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) co_return; // 如果找不到，可能已被处理或出错，直接返回

    // 移动上下文内容，然后从 map 中删除，避免重复处理和内存泄漏
    StreamContext stream_ctx = std::move(it->second);
    streams_.erase(it);

    // --- 将 nghttp2 的原始头数据转换为 Beast HTTP 请求对象 ---
    HttpRequest req;
    req.version(20); // HTTP/2.0
    for (const auto &pair: stream_ctx.headers) {
        const std::string &key = pair.first;
        const std::string &val = pair.second;
        if (key.empty()) continue;
        if (key[0] == ':') { // 处理 HTTP/2 伪头字段
            if (key == ":method") req.method(http::string_to_verb(val));
            else if (key == ":path") req.target(val);
            else if (key == ":authority") req.set(http::field::host, val);
        } else { // 处理普通头字段
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
    } catch (const std::exception &e) {
        // 捕获业务逻辑中的异常，并返回一个 500 错误
        SPDLOG_ERROR("Exception in H2 handler for [{}]: {}", ctx.request().target(), e.what());
        ctx.json(http::status::internal_server_error, "error Internal Server Error");
    }

    // --- 准备并提交响应 ---
    auto &resp = ctx.response();
    // TODO: 最佳压缩时机
    resp.prepare_payload(); // 准备响应，计算 Content-Length

    // 将 Beast HTTP 响应头转换为 nghttp2 的 nghttp2_nv 格式
    std::vector<nghttp2_nv> headers;
    std::string status_code = std::to_string(resp.result_int());
    headers.push_back({(uint8_t *) ":status", (uint8_t *) status_code.c_str(), 7, status_code.size(), NGHTTP2_NV_FLAG_NONE});
    for (const auto &field: resp) {
        if (field.name() == http::field::connection) continue; // HTTP/2 中不使用 Connection 头
        headers.push_back({(uint8_t *) field.name_string().data(), (uint8_t *) field.value().data(), field.name_string().length(), field.value().length(), NGHTTP2_NV_FLAG_NONE});
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
        provider.read_callback = [](nghttp2_session *, int32_t, uint8_t *buf, size_t len, uint32_t *flags, nghttp2_data_source *src, void *) -> ssize_t {
            auto *p = static_cast<ProviderPack *>(src->ptr);
            if (!p || !p->content) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
            size_t remaining = p->content->size() - p->offset;
            size_t n = std::min(len, remaining); // 计算本次能发送多少数据
            if (n > 0) {
                memcpy(buf, p->content->data() + p->offset, n);
                p->offset += n;
            }
            if (p->offset == p->content->size()) { // 如果数据全部发送完毕
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


/**
 * @brief nghttp2 回调：当一个完整的帧被接收时调用
 */
int Http2Session::on_frame_recv_callback(nghttp2_session *, const nghttp2_frame *frame, void *user_data) {
    auto *self = static_cast<Http2Session *>(user_data);
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



/**
 * @brief nghttp2 回调：当一个 header 键值对被接收时调用
 */
int Http2Session::on_header_callback(nghttp2_session *, const nghttp2_frame *frame,const uint8_t *name, size_t name_len,const uint8_t *value, size_t value_len,uint8_t, void *user_data) {
    auto *self = static_cast<Http2Session *>(user_data);
    int32_t sid = frame->hd.stream_id;
    if (sid == 0) return 0; // 忽略连接控制流（stream_id 0）的头

    // 如果是这个流的第一个头，则创建新的上下文
    if (self->streams_.find(sid) == self->streams_.end()) {
        self->streams_[sid] = {};
    }
    auto &stream_ctx = self->streams_.at(sid);

    // 直接存储原始的头数据，不做任何解析，留给 dispatch 协程处理
    stream_ctx.headers.emplace_back(
        std::string(reinterpret_cast<const char*>(name), name_len),
        std::string(reinterpret_cast<const char*>(value), value_len)
    );

    return 0;
}


/**
 * @brief nghttp2 回调：当一个 DATA 帧的数据块被接收时调用
 */
int Http2Session::on_data_chunk_recv_callback(nghttp2_session *, uint8_t, int32_t stream_id, const uint8_t *data, size_t len, void *user_data) {
    auto *self = static_cast<Http2Session *>(user_data);
    auto it = self->streams_.find(stream_id);
    if (it != self->streams_.end()) {
        // 将接收到的数据块追加到对应流的 body 字符串中
        it->second.body.append((char *) data, len);
    }
    return 0;
}


/**
 * @brief nghttp2 回调：当一个流被关闭时调用
 */
int Http2Session::on_stream_close_callback(nghttp2_session *, int32_t stream_id, uint32_t, void *user_data) {
    auto *self = static_cast<Http2Session *>(user_data);
    // 使用 post 将清理工作调度到 strand 上，以确保线程安全地修改 provider_pack_
    boost::asio::post(self->strand_,
                      [self, stream_id]() {
                          // 移除与已关闭流相关的响应数据提供者，释放其持有的响应体内存
                          self->provider_pack_.erase(stream_id);
                      });
    return 0;
}


/**
 * @brief 优雅关闭的公共接口协程
 */
boost::asio::awaitable<void> Http2Session::graceful_shutdown(uint32_t error_code) {
    // 确保我们总是在 strand 上下文中执行此操作。
    // 如果调用者已经在 strand 上，dispatch 是一个 no-op（无操作）。
    // 如果不在，它会将协程的剩余部分 post 到 strand 上执行。
    co_await boost::asio::dispatch(strand_, boost::asio::use_awaitable);

    // 检查是否已经关闭或正在关闭，防止重复操作
    if (!session_ || !stream_->next_layer().is_open()) {
        co_return;
    }

    SPDLOG_DEBUG("Initiating graceful shutdown for Http2Session with error_code={}", error_code);

    // 获取最后一个由服务器处理的流 ID。客户端会重试 ID 大于此值的请求。
    int32_t last_stream_id = nghttp2_session_get_last_proc_stream_id(session_);

    // 提交 GOAWAY 帧给 nghttp2 引擎
    int rv = nghttp2_submit_goaway(
        session_,
        NGHTTP2_FLAG_NONE,
        last_stream_id,
        error_code,
        nullptr, // 没有额外的调试数据
        0
    );

    if (rv != 0) {
        SPDLOG_ERROR("nghttp2_submit_goaway() failed: {}", nghttp2_strerror(rv));
        // 即使提交失败，我们也要继续尝试关闭
    }

    // 触发 writer_loop 来发送这个 GOAWAY 帧。
    // 因为我们已经在 strand 上，所以可以直接调用。
    schedule_write();

    // 注意：我们不在此处等待写操作完成。关闭流程的其他部分（如 session_loop 退出）会处理后续的 socket 关闭。
}


/**
 * @brief 获取远程客户端的端点信息
 */
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


/**
 * @brief 优雅关闭的内部实现协程（与 `graceful_shutdown` 功能重复，但保留以符合原始代码）
 */
boost::asio::awaitable<void> Http2Session::do_graceful_shutdown(uint32_t error_code) {
    // 确保在 strand 上下文中执行
    co_await boost::asio::dispatch(strand_, boost::asio::use_awaitable);

    if (!session_ || !stream_->next_layer().is_open()) {
        co_return; // 已经关闭了
    }

    SPDLOG_INFO("Initiating graceful shutdown for Http2Session with error_code={}", error_code);

    const int32_t last_stream_id = nghttp2_session_get_last_proc_stream_id(session_);

    const int rv = nghttp2_submit_goaway(
        session_,
        NGHTTP2_FLAG_NONE,
        last_stream_id,
        error_code, // 使用传入的错误码 (例如 NGHTTP2_NO_ERROR 或其他)
        nullptr,
        0
    );

    if (rv != 0) {
        SPDLOG_ERROR("nghttp2_submit_goaway() failed: {}", nghttp2_strerror(rv));
        // 即使提交失败，我们也要继续关闭流程
    }

    // 触发 writer_loop 来发送 GOAWAY 帧
    schedule_write();

    // **可选但推荐**: 等待一小段时间，给 GOAWAY 帧发送出去的机会。
    // 这不是必须的，因为 writer_loop 会尽力发送，但这可以增加在 socket 被强行关闭前成功发送的概率。
    boost::asio::steady_timer t(co_await boost::asio::this_coro::executor, 50ms);
    co_await t.async_wait(boost::asio::use_awaitable);
}
