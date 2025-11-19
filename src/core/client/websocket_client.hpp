//
// Created by Aiziboy on 25-8-5.
//

#ifndef WEBSOCKET_CLIENT_HPP
#define WEBSOCKET_CLIENT_HPP

#include "connection_manager.hpp" // 需要它的基础设施
#include "websocket_connection.hpp"
#include "iwebsocket_client_handler.hpp"
#include <string_view>

class WebSocketClient {
public:
    explicit WebSocketClient(boost::asio::io_context& ioc,bool ssl_verify);

    /**
     * @brief 异步连接到一个 WebSocket 端点。
     * @param url WebSocket URL (e.g., "ws://example.com/stream" or "wss://...").
     * @param handler 用于处理此连接事件的 Handler。
     * @param headers [可选] 额外的 HTTP 头部，用于握手请求。
     * @return 返回一个已建立的 WebSocketConnection 的共享指针。
     */
    boost::asio::awaitable<std::shared_ptr<WebSocketConnection>> connect(
        std::string_view url,
        std::shared_ptr<IWebsocketClientHandler> handler,
        const Headers& headers = {});

private:
    boost::asio::io_context& ioc_;
    boost::asio::ssl::context ssl_ctx_;
    boost::asio::ip::tcp::resolver resolver_;

    // 复用 HttpClient 的 URL 解析器
    struct ParsedUrl {
        std::string scheme; // 是带:格式的。例如: ws:,而非: wss:
        std::string host;
        uint16_t port;
        std::string target;
    };

    static ParsedUrl parse_url(std::string_view url_strv);
};

#endif // WEBSOCKET_CLIENT_HPP
