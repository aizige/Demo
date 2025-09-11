//
// Created by Aiziboy on 25-8-5.
//

#ifndef IWEBSOCKET_CLIENT_HANDLER_HPP
#define IWEBSOCKET_CLIENT_HANDLER_HPP
#include <string>
#include <memory>
#include <boost/asio/awaitable.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <boost/system/error_code.hpp>

// 向前声明 WebSocketConnection，以避免头文件循环依赖。
class WebSocketConnection;

/**
 * @class IWebSocketClientHandler
 * @brief WebSocket 客户端事件处理程序的接口。
 *
 * 通过实现此接口，你可以为 WebSocket 连接的不同生命周期事件
 * （如连接成功、收到消息、断开连接）定义自定义的业务逻辑。
 * 这是一个纯虚基类，强制实现所有回调。
 */
class IWebSocketClientHandler {
public:
    virtual ~IWebSocketClientHandler() = default;

    /**
     * @brief 当 WebSocket 握手成功，连接正式建立时调用。
     *        这是一个协程，允许你在连接成功后执行异步操作。
     * @param connection 指向已建立连接的共享指针，你可以保存它以便后续发送消息。
     */
    virtual boost::asio::awaitable<void> on_connect(std::shared_ptr<WebSocketConnection> connection) = 0;

    /**
     * @brief 当连接被关闭时调用，无论是正常关闭还是因错误断开。
     *        这是一个同步函数，因为它通常在协程栈展开或清理阶段被调用。
     * @param close_reason 包含了 WebSocket 关闭码和原因的结构体。
     * @param ec 如果是因网络错误断开，则包含相应的 Boost.System 错误码。
     */
    virtual void on_disconnect(boost::beast::websocket::close_reason close_reason, boost::system::error_code ec) = 0;

    virtual boost::asio::awaitable<void> on_message(std::string message) = 0;
};
#endif //IWEBSOCKET_CLIENT_HANDLER_HPP
