//
// Created by Aiziboy on 25-8-5.
//

#ifndef USER_STATUS_HANDLER_HPP
#define USER_STATUS_HANDLER_HPP

#include "core/client/iwebsocket_client_handler.hpp"
#include <spdlog/spdlog.h>

#include "websocket_connection.hpp"

class UserStatusHandler : public IWebsocketClientHandler, public std::enable_shared_from_this<UserStatusHandler> {
public:
    static std::shared_ptr<UserStatusHandler> create() {
        // [修复] 分两步创建，以绕过私有构造函数的限制
        return std::shared_ptr<UserStatusHandler>(new UserStatusHandler());
    }

    // ~~~ IWebsocketClientHandler 接口实现 ~~~

    boost::asio::awaitable<void> on_connect(std::shared_ptr<WebSocketConnection> connection) override {
        SPDLOG_INFO("UserStatusHandler: Connection successful to user status stream.");
        // 保存连接的 shared_ptr，以便后续可以调用 send()
        connection_ = connection;

        // 连接成功后，可以立即发送订阅消息
       // try {
       //     co_await connection_->send(R"({"action": "subscribe", "topic": "user_status"})");
       //     SPDLOG_INFO("UserStatusHandler: Sent subscription request.");
       // } catch (const std::exception& e) {
       //     SPDLOG_ERROR("UserStatusHandler: Failed to send subscription: {}", e.what());
       // }
        co_return;
    }



    // on_disconnect 的实现
    void on_disconnect(boost::beast::websocket::close_reason close_reason, boost::system::error_code ec) override {
        SPDLOG_WARN("UserStatusHandler: Disconnected. Code: {}, Reason: '{}' (ec: {})",
            static_cast<int>(close_reason.code),
            close_reason.reason,
            ec.message());
        connection_.reset(); // 清理连接指针
    }

    boost::asio::awaitable<void> on_message(std::string message) override {
        SPDLOG_DEBUG("UserStatusHandler: Received message: {}", message);
        // 到时候单独在静态类中创建一个消息队列，例如：    using BusinessMessageChannel = boost::asio::experimental::channel<void(boost::system::error_code, std::string)>;   BusinessMessageChannel business_channel_;
        // 然后由websocket客户端链接创建者自行选择是否使用消息队列
        //

        co_return;
    }


private:
    // 私有化构造函数，强制使用 create()
    UserStatusHandler() = default;

    std::shared_ptr<WebSocketConnection> connection_;
};

#endif // USER_STATUS_HANDLER_HPP
