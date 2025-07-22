//
// Created by Aiziboy on 2025/7/18.
//

#include "connection_manager.hpp"
#include "http1_connection.hpp" // 需要包含具体的连接实现
#include <spdlog/spdlog.h>
#include "iconnection.hpp"         // << 提供 IConnection 的完整定义
#include "http_ssl_connection.hpp"
#include "connection_helpers.hpp"
#include "h2_connection.hpp"


ConnectionManager::ConnectionManager(boost::asio::io_context& ioc)
    : ioc_(ioc),
      ssl_ctx_(boost::asio::ssl::context::sslv23_client),
      resolver_(ioc) {

    // **[新增]** 明确禁用不安全的旧协议版本，如 SSLv2, SSLv3, TLSv1.0, TLSv1.1
    // 这会让我们的客户端既有良好的兼容性（支持 TLS 1.2, 1.3），又保持了安全性。
    ssl_ctx_.set_options(
        boost::asio::ssl::context::default_workarounds |
        boost::asio::ssl::context::no_sslv2 |
        boost::asio::ssl::context::no_sslv3 |
        boost::asio::ssl::context::no_tlsv1 |
        boost::asio::ssl::context::no_tlsv1_1 |
        boost::asio::ssl::context::single_dh_use
    );


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
                            supported_protos, sizeof(supported_protos));

}


boost::asio::awaitable<PooledConnection> ConnectionManager::get_connection(std::string_view scheme, std::string_view host, uint16_t port) {
    std::string key = std::string(scheme) + "//" + std::string(host) + ":" + std::to_string(port);
    SPDLOG_DEBUG("Requesting connection for key: {}", key);

    { // --- 临界区 1: 快速检查空闲连接池 ---
        std::lock_guard<std::mutex> lock(pool_mutex_);
        auto& idle_queue = pool_[key];
        while (!idle_queue.empty()) {
            auto conn = idle_queue.front();
            idle_queue.pop();
            if (conn->is_usable() && is_connection_healthy(conn->lowest_layer_socket())) {
                SPDLOG_DEBUG("ConnectionManager: Reusing healthy connection {} for {}", conn->id(), key);
                co_return PooledConnection{conn, true};
            } else {
                SPDLOG_DEBUG("ConnectionManager: Pruning stale connection {} from pool for {}", conn->id(), key);
            }
        }
    }


    // --- 池中无可用连接，进入序列化的创建流程 ---
    boost::asio::strand<boost::asio::any_io_executor> creation_strand(ioc_.get_executor());
    { // --- 临界区 2: 获取或创建该 key 专属的 strand ---
        std::lock_guard<std::mutex> lock(pool_mutex_);
        auto it = creation_strands_.find(key);
        if (it == creation_strands_.end()) {
            creation_strands_.emplace(key, boost::asio::strand<boost::asio::any_io_executor>(ioc_.get_executor()));
        }
        creation_strand = creation_strands_.at(key);
    }

    co_await post(creation_strand, boost::asio::use_awaitable);

    // --- 现在处于序列化执行的上下文中 ---
    { // --- 双重检查 ---
        std::lock_guard<std::mutex> lock(pool_mutex_);
        auto& idle_queue = pool_[key];
        if (!idle_queue.empty()) {
            auto conn = idle_queue.front();
            idle_queue.pop();
            if (conn->is_usable()) {
                SPDLOG_DEBUG("ConnectionManager: Picked up connection created by another coroutine for {}", key);
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
    //  直接在 IConnection 接口上操作
    if (!conn->is_usable()) {
        spdlog::debug("ConnectionManager: Connection {} is not usable, discarding.", conn->id());
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
            tcp::socket socket(ioc_);
            co_await async_connect(socket, endpoints, boost::asio::use_awaitable);
            co_return std::make_shared<Http1Connection>(std::move(socket), key);
        } else if (scheme == "https:") {
            auto stream = std::make_shared<boost::beast::ssl_stream<boost::beast::tcp_stream>>(ioc_, ssl_ctx_);
            co_await async_connect(stream->next_layer().socket(), endpoints, boost::asio::use_awaitable);

            if (!SSL_set_tlsext_host_name(stream->native_handle(), host.data())) {
                throw boost::system::system_error(static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category());
            }
            co_await stream->async_handshake(boost::asio::ssl::stream_base::client, boost::asio::use_awaitable);

            const unsigned char* proto = nullptr;
            unsigned int len = 0;
            SSL_get0_alpn_selected(stream->native_handle(), &proto, &len);
            if (proto && std::string_view((const char*)proto, len) == "h2") {
                SPDLOG_INFO("ALPN selected h2 for {}. Creating Http2Connection.", host);
                auto conn = Http2Connection::create(stream, key);
               co_await conn->start();

                co_return conn;
            } else {
                SPDLOG_INFO("ALPN selected http/1.1 for {}. Creating HttpSslConnection.", host);
                co_return std::make_shared<HttpSslConnection>(std::move(*stream), key);
            }
        }
        throw std::runtime_error("Unsupported scheme: " + std::string(scheme));
    } catch (const boost::system::system_error& e) {
        SPDLOG_ERROR("Failed to create new connection to {}: {}", key, e.what());
        throw; // 将异常向上传递
    }
}

void ConnectionManager::replace_connection(std::shared_ptr<IConnection> old_conn, std::shared_ptr<IConnection> new_conn) {
    if (!new_conn) return;

     std::lock_guard<std::mutex> lock(pool_mutex_);
    const auto& key = new_conn->get_pool_key();
    pool_[key].push(new_conn);
    SPDLOG_DEBUG("Replaced connection in pool for key [{}]. Old conn id: {}, New conn id: {}",
                  key, old_conn ? old_conn->id() : "null", new_conn->id());

}
