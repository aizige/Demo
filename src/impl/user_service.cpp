//
// Created by Aiziboy on 25-8-5.
//

#include "user_service.hpp"

#include "core/client/UserStatusHandler.hpp"

UserService::UserService(std::shared_ptr<IHttpClient> http_client,std::shared_ptr<WebSocketClient> ws_client)
: http_client_(std::move(http_client)), ws_client_(std::move(ws_client))


{
}

boost::asio::awaitable<void> UserService::get_user_by_id(RequestContext &ctx) {
    SPDLOG_INFO("User ID:");
    // 从 ctx 中获取路径参数
    // auto id_str = ctx.path_param("id");
    // if (!id_str) {
    //     // 如果参数不存在，填充一个 400 Bad Request 响应
    //     ctx.string(http::status::bad_request, "Missing user ID");
    //     co_return;
    // }
    //
    // // ... 业务逻辑 ...
    // nlohmann::json result = {
    //     {"id", id_str},
    //     {"data", {"Tom", "Jerry", "Spike"}} // 示例数据
    // };
    // SPDLOG_INFO("User ID: {}", id_str.value());
    // // 使用便利函数填充 200 OK 响应
    // ctx.json(http::status::ok, result);
    ctx.string(http::status::ok, "Hello, World!");

    co_return;
}

boost::asio::awaitable<void> UserService::search_users(RequestContext &ctx) {
    // 2. 从上下文中获取名为 "kw" 和 "page" 的查询参数
    auto kw = ctx.query_param("kw");
    auto page = ctx.query_param("page");
    SPDLOG_DEBUG("进来了");
    // 3. 构造一个 JSON 对象来表示搜索结果
    nlohmann::json result = {
        {"kw", kw}, {"page", page},
        {"data", {"Tom", "Jerry", "Spike"}} // 示例数据
    };

    ctx.json(http::status::ok, result);

    // 5. 返回响应
    co_return ;
}

boost::asio::awaitable<void> UserService::test_http_client(RequestContext &ctx) {
    // 2. 从上下文中获取名为 "kw" 和 "page" 的查询参数
    auto url = ctx.query_param("url");
    std::string_view value = url.value();
    try {
        // 直接使用注入的 http_client_ 实例
        SPDLOG_DEBUG("准备对：{} 发起请求", value);
        HttpResponse response = co_await http_client_->get(value, {});

        if (response.result() == http::status::ok) {
            SPDLOG_DEBUG("请求成功 body size = {}", response.body().size());
            std::string_view content_type;
            auto it = response.find(http::field::content_type);
            if (it != response.end()) {
                content_type = it->value();
            }
            ctx.string(http::status::ok, response.body(), content_type);
            co_return ;
        }

        ctx.string(response.result(), http::obsolete_reason(response.result()));
        co_return;
    } catch (const boost::system::system_error &e) {
        // 处理异常
        SPDLOG_ERROR("Failed to get user from external API: {}", e.what());
        ctx.json(http::status::bad_gateway,
                 {
                     {"error", "Failed to fetch from upstream server."},
                     {"reason", e.what()}
                 });
    }
}

boost::asio::awaitable<void> UserService::connect_to_status_stream(RequestContext &ctx) {
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
        ctx.json(http::status::ok, {
            {"status", "WebSocket connection initiated"},
            {"connection_id", connection->id()}
        });

    } catch (const std::exception& e) {
        SPDLOG_ERROR("UserService: Failed to initiate WebSocket connection: {}", e.what());
        ctx.json(http::status::internal_server_error, {
            {"error", "Failed to connect to WebSocket stream"},
            {"reason", e.what()}
        });
    }
}
