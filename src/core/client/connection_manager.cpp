//
// Created by Aiziboy on 2025/7/18.
//

#include "connection_manager.hpp"
#include "http1_connection.hpp" // 需要包含具体的连接实现
#include <spdlog/spdlog.h>
#include "iconnection.hpp"         // << 提供 IConnection 的完整定义
#include "http_ssl_connection.hpp"
#include "connection_helpers.hpp"


ConnectionManager::ConnectionManager(boost::asio::io_context& ioc)
    : ioc_(ioc),
      ssl_ctx_(boost::asio::ssl::context::tlsv13_client),
      resolver_(ioc) {
    // **在构造函数体中，对 ssl_ctx_ 进行配置**
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(boost::asio::ssl::verify_peer);

    // --- **[新增]** 配置 ALPN ---
    // 设置我们客户端支持的应用层协议列表，优先级从高到低
    // "h2" 代表 HTTP/2, "http/1.1" 是备选
    const unsigned char supported_protos[] = {
        2, 'h', '2', // 2字节长的 "h2"
        8, 'h', 't', 't', 'p', '/', '1', '.', '1' // 8字节长的 "http/1.1"
    };
    SSL_CTX_set_alpn_protos(ssl_ctx_.native_handle(),
                            supported_protos, 11);
    // 格式是：[1字节长度][协议名][1字节长度][协议名]...
    // \x02h2 -> 长度为 2 的 "h2"
    // \x08http/1.1 -> 长度为 8 的 "http/1.1"
}


boost::asio::awaitable<PooledConnection> ConnectionManager::get_connection(std::string_view scheme, std::string_view host, uint16_t port) {
    std::string key = std::string(scheme) + "//" + std::string(host) + ":" + std::to_string(port);
    SPDLOG_DEBUG("key =  {}", key);
    {
        // --- 临界区 1: 快速检查空闲连接池 ---
        std::lock_guard<std::mutex> lock(pool_mutex_);

        auto& idle_queue = pool_[key];
        while (!idle_queue.empty()) {
            auto conn = idle_queue.front();
            idle_queue.pop();
            if (conn->is_usable() && is_connection_healthy(conn->lowest_layer_socket())) {
                SPDLOG_DEBUG("ConnectionManager: Reusing connection {} for {}", conn->id(), key);
                co_return PooledConnection{conn, true};
            } else {
                SPDLOG_DEBUG("ConnectionManager: Pruning dead connection {} from pool for {}", conn->id(), key);
            }
        }
    }


    // --- 池中无可用连接，进入序列化的创建流程 ---
    boost::asio::strand<boost::asio::any_io_executor> creation_strand(ioc_.get_executor());
    {
        // --- 临界区 2: 获取或创建该 key 专属的 strand ---
        std::lock_guard<std::mutex> lock(pool_mutex_);
        auto it = creation_strands_.find(key);
        if (it == creation_strands_.end()) {
            // 如果是第一个为这个 key 创建 strand 的协程
            creation_strands_.emplace(key, boost::asio::strand<boost::asio::any_io_executor>(ioc_.get_executor()));
        }
        creation_strand = creation_strands_.at(key);
    }

    // **将后续的创建逻辑 post 到专属的 strand 上下文中执行**
    // 这确保了对于同一个 key，只有一个协程能进入下面的代码块
    co_await post(creation_strand, boost::asio::use_awaitable);

    // --- 现在我们处于序列化执行的上下文中 ---

    // **[关键]** 双重检查：再次检查连接池。
    // 因为在我前面排队的协程可能已经创建好了一个连接并放回了池中。
    // (注意：这个场景很少见，但为了逻辑完备性，我们加上这个检查)
    // 更常见的是，它会创建一个连接，然后被它的调用者使用，
    // 在我们这里检查时，池子可能还是空的。
    // 但这个检查是“双重检查锁定”模式的核心。
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        auto& idle_queue = pool_[key];
        if (!idle_queue.empty()) {
            auto conn = idle_queue.front();
            idle_queue.pop();
            // 理论上这个连接应该是可用的，但我们还是检查一下
            if (conn->is_usable()) {
                SPDLOG_DEBUG("ConnectionManager: Picked up a connection created by another coroutine for {}", key);
                co_return PooledConnection{conn, true};
            }
        }
    }

    // 如果连接池仍然为空，说明我是这个 strand 队列中第一个需要创建连接的。
    SPDLOG_DEBUG("ConnectionManager: No usable connection in pool for {}, creating new one inside strand.", key);
    auto new_conn = co_await create_new_connection(scheme, host, port);

    co_return PooledConnection{new_conn, false};
}

void ConnectionManager::release_connection(std::shared_ptr<IConnection> conn) {
    if (!conn || !conn->is_usable()) {
        SPDLOG_DEBUG("ConnectionManager: Connection {} is not usable, discarding.", conn ? conn->id() : "null");
        // 不可用的连接直接析构，不放回池中
        return;
    }
    // 先动态类型转换为 Http1Connection 来获取 key
    // 当我们有 H2Connection 时，这里需要一个更通用的方法
    // 比如在 IConnection 接口中加入 get_pool_key()
    auto http1_conn = std::dynamic_pointer_cast<Http1Connection>(conn);

    if (!http1_conn || !http1_conn->is_usable()) {
        SPDLOG_DEBUG("ConnectionManager: Connection {} is not usable, discarding.", conn->id());
        return;
    }

    const auto& key = conn->get_pool_key();
    SPDLOG_DEBUG("ConnectionManager: Releasing connection {} back to pool [{}].", conn->id(), key);

    std::lock_guard<std::mutex> lock(pool_mutex_);
    pool_[key].push(conn);
}

boost::asio::awaitable<std::shared_ptr<IConnection>> ConnectionManager::create_new_connection(std::string_view scheme, std::string_view host, uint16_t port) {
    std::string key = std::string(scheme) + "//" + std::string(host) + ":" + std::to_string(port);
    try {
        // 1. DNS 解析
        auto endpoints = co_await resolver_.async_resolve(host, std::to_string(port), boost::asio::use_awaitable);
        if (scheme == "http:") {
            // 2. 创建 socket 并连接
            tcp::socket socket(ioc_);

            // Clion报错：模板形参 Iterator 的推断冲突替换: std::tuple<boost::system::error_code...不用管
            co_await async_connect(socket, endpoints, boost::asio::use_awaitable);

            // 3. 创建 Http1Connection
            co_return std::make_shared<Http1Connection>(std::move(socket), key);

        } else if (scheme == "https:") {
            tcp::socket socket(ioc_);
            // Clion 报错: 模板形参推断冲突，但代码实际可正常编译运行。
            co_await async_connect(socket, endpoints, boost::asio::use_awaitable);

            //////auto conn = std::make_shared<HttpSslConnection>(std::move(socket), ssl_ctx_, key);

            // 我们不再直接创建 HttpSslConnection，而是先创建一个临时的 SSL stream 来进行握手
            auto stream = std::make_shared<boost::beast::ssl_stream<boost::beast::tcp_stream>>(std::move(socket), ssl_ctx_);

            // 设置 SNI
            if (!SSL_set_tlsext_host_name(stream->native_handle(), host.data())) {
                throw boost::system::system_error({/* ... */});
            }

            // 执行 TLS 握手
            co_await stream->async_handshake(boost::asio::ssl::stream_base::client, boost::asio::use_awaitable);

            // **检查 ALPN 协商结果**
            const unsigned char* proto = nullptr;
            unsigned int len = 0;
            SSL_get0_alpn_selected(stream->native_handle(), &proto, &len);

            if (proto && std::string_view((const char*)proto, len) == "h2") {
                // --- **服务器选择了 HTTP/2** ---
                spdlog::info("ALPN selected h2 for {}. Creating Http2Connection.", host);
                // TODO: 创建并返回一个 Http2Connection 实例
                // auto conn = std::make_shared<Http2Connection>(std::move(*stream), router_, key);
                // co_await conn->start(); // H2 连接需要一个启动过程来交换 SETTINGS 帧
                // co_return conn;
                throw std::runtime_error("Http2Connection not implemented yet."); // 占位符

            } else {
                // --- **服务器选择了 HTTP/1.1 或未进行 ALPN** ---
                spdlog::info("ALPN selected http/1.1 for {}. Creating HttpSslConnection.", host);
                // 现在，用这个已经握手成功的 stream 来创建 HttpSslConnection
                co_return std::make_shared<HttpSslConnection>(std::move(*stream), key);
            }

            co_await conn->handshake(host);
            co_return conn;
        }
        throw std::runtime_error("Unsupported scheme: " + std::string(scheme));
    } catch (const boost::system::system_error& e) {
        SPDLOG_ERROR("Failed to create new connection to {}: {}", key, e.what());
        throw; // 将异常向上传递
    }
}
