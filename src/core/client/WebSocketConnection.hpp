//
// Created by Aiziboy on 25-8-5.
//

#ifndef WEBSOCKET_CONNECTION_HPP
#define WEBSOCKET_CONNECTION_HPP
#include "http/http_common_types.hpp"
#include "utils/utils.hpp"
#include "IWebSocketClientHandler.hpp"
#include <boost/beast/websocket.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <variant>
#include <string>
#include <boost/asio/experimental/promise.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>

#include "connection_manager.hpp"

namespace websocket = boost::beast::websocket;
using WebSocketMessage = std::string;

class WebSocketConnection : public std::enable_shared_from_this<WebSocketConnection> {
public:
    // 定义 WebSocket 协议流的类型
    using PlainStream = websocket::stream<boost::beast::tcp_stream>;
    using SslStream = websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>>;
    using WsVariantStream = std::variant<std::shared_ptr<PlainStream>, std::shared_ptr<SslStream>>;

    WebSocketConnection(WsVariantStream stream,
                            HttpRequest upgrade_request,
                            std::shared_ptr<IWebSocketClientHandler> handler,
                            std::string id);
    ~WebSocketConnection();

    WebSocketConnection(const WebSocketConnection&) = delete;
    WebSocketConnection& operator=(const WebSocketConnection&) = delete;

    // --- 公共 API ---
    void run();
    boost::asio::awaitable<void> send(WebSocketMessage message);
    boost::asio::awaitable<void> close(websocket::close_code code = websocket::close_code::normal, std::string_view reason = "");

    const std::string& id() const { return id_; }
    bool is_open() const;
    boost::asio::any_io_executor get_executor() const;

private:
    using SendChannel = boost::asio::experimental::channel<void(boost::system::error_code, WebSocketMessage)>;
    using ReceiveChannel = boost::asio::experimental::channel<void(boost::system::error_code, WebSocketMessage)>;

    // 分离的读写循环
    boost::asio::awaitable<void> reader_loop();
    boost::asio::awaitable<void> writer_loop();

    WsVariantStream stream_;
    HttpRequest initial_upgrade_request_;
    std::shared_ptr<IWebSocketClientHandler> handler_;
    std::string id_;

    SendChannel send_channel_;
    //ReceiveChannel receive_channel_;
    boost::beast::flat_buffer read_buffer_;


    std::atomic<bool> is_closing_{false};
    std::atomic<bool> close_called_{false};
};


#endif //WEBSOCKET_CONNECTION_HPP
