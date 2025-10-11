//
// Created by Aiziboy on 25-8-5.
//

#include "user_service.hpp"

#include "core/client/UserStatusHandler.hpp"

UserService::UserService(std::shared_ptr<IHttpClient> http_client, std::shared_ptr<WebSocketClient> ws_client)
    : http_client_(std::move(http_client)), ws_client_(std::move(ws_client)) {
}

boost::asio::awaitable<std::string> UserService::get_user_by_id(int id, const std::string_view name) {
    SPDLOG_DEBUG("User ID: {}，name: {}", id, name);
    // 取路径参数


    // ... 业务逻辑 ...


    // 使用便利函数填充 200 OK 响应

    co_return "Hello, World!";
}

boost::asio::awaitable<std::string> UserService::search_users(std::string_view kw, std::string_view page) {
    SPDLOG_DEBUG("进来了");

    // 5. 返回响应
    co_return "Hello, World!";
}

boost::asio::awaitable<std::string> UserService::test_http_client(std::string_view url) {
    // 2. 从上下文中获取名为 "kw" 和 "page" 的查询参数
    SPDLOG_DEBUG("准备对：{} 发起请求", url);
    try {
        // 直接使用注入的 http_client_ 实例
        HttpResponse response = co_await http_client_->get(url, {});


        SPDLOG_DEBUG("请求成功 body size = {}", response.body().size());
        std::string_view content_type;
        auto it = response.find(http::field::content_type);
        if (it != response.end()) {
            content_type = it->value();
        }

        co_return response.body();
    } catch (const boost::system::system_error& e) {
        // 处理异常
        SPDLOG_ERROR("Failed to get user from external API: {}", e.what());
        co_return "Failed ";
    }

}

boost::asio::awaitable<std::string> UserService::connect_to_status_stream(std::string_view body) {
        try {
            // 1. 创建一个 Handler 实例
            auto handler = UserStatusHandler::create();

            SPDLOG_INFO("UserService: Attempting to connect to WebSocket status stream...");

            // 2. 调用 WebSocketClient 的 connect 方法
            //    connect 是一个协程，它会在 TCP/TLS 连接建立后返回。
            //    我们不需要等待 WebSocket 握手。
            auto connection = co_await ws_client_->connect(
                "wss://stream.binance.com:443/ws/btcusdt@trade", // 目标 URL
                handler // 注入 Handler
            );

            // 3. 连接过程已启动
            //    connect 返回后，后台的 reader_loop 正在进行 WebSocket 握手。
            //    握手成功后，handler->on_connect() 将被自动调用。
            //    所有后续的交互都在 UserStatusHandler 内部发生。

            SPDLOG_INFO("UserService: WebSocket connection process initiated with ID [{}].", connection->id());

            // 4. 向原始的 HTTP 请求返回一个响应，告诉它“启动成功”
            co_return "请求成功";

        } catch (const std::exception& e) {
            SPDLOG_ERROR("UserService: Failed to initiate WebSocket connection: {}", e.what());

            co_return "请求失败";
        }

}
