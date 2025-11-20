//
// Created by Aiziboy on 25-8-5.
//


#include "user_service.hpp"



#include <boost/asio/as_tuple.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <exception> // for std::exception_ptr
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <utility>

#include "user_status_handler.hpp"

// 引入 asio 协程操作符，如 || (race)
using namespace boost::asio::experimental::awaitable_operators;
using namespace std::chrono_literals;
UserService::UserService(boost::asio::any_io_executor io_executor,boost::asio::any_io_executor worker_executor,const std::shared_ptr<IHttpClient>& http_client, const std::shared_ptr<WebSocketClient>& ws_client)
    : ioc_executor_(std::move(io_executor)), worker_executor_(std::move(worker_executor)), http_client_(http_client), ws_client_(ws_client) {
}

/**
 * @brief 模拟向服务器并行发起大量（N个）HTTP GET请求。
 */
boost::asio::awaitable<void> UserService::simulate_high_concurrency(int num_requests, std::string_view url) {
    if (num_requests <= 0) {
        co_return;
    }
    
    // 1. 创建一个 channel 作为“完成计数器”。
    //    它的容量等于请求数量。
    auto completion_channel = std::make_shared<
        boost::asio::experimental::channel<void(boost::system::error_code)>
    >(ioc_executor_, num_requests);

    // 2. 循环启动所有工作协程
    for (int i = 0; i < num_requests; ++i) {
        co_spawn(
            ioc_executor_,
            // 工作协程
            [this, url, channel_ptr = completion_channel]() -> boost::asio::awaitable<void> {
                try {
                    // a. 执行单个请求，我们不关心它的返回值
                    co_await http_client_->get(url, {});
                } catch (const std::exception& e) {
                    // b. 捕获并记录失败，但不让异常逃逸
                    SPDLOG_ERROR("A request in high concurrency test failed: {}", e.what());
                }

                // c.  任务完成（无论成功或失败），向 channel 发送一个信号
                constexpr boost::system::error_code ignored_ec;
                co_await channel_ptr->async_send(ignored_ec, boost::asio::use_awaitable);
            },
            boost::asio::detached
        );
    }

    // 3. 在主协程中，等待接收到所有 N 个完成信号
    SPDLOG_INFO("Waiting for all {} requests to complete...", num_requests);
    for (int i = 0; i < num_requests; ++i) {
        try {

            co_await completion_channel->async_receive(boost::asio::use_awaitable);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Error waiting on completion channel: {}", e.what());
            break;
        }
    }

    SPDLOG_INFO("All {} requests have completed.", num_requests);
}

boost::asio::awaitable<std::string> UserService::get_user_by_id(int id, const std::string_view name) {
    SPDLOG_DEBUG("User ID: {}，name: {}", id, name);
    // 取路径参数


    // ... 业务逻辑 ...


    // 使用便利函数填充 200 OK 响应

    co_return "Hello, World!";
}

/**
 * @brief 并发网络请求
 * @param sum 并发线程数
 * @param url 要并发请求的url
 * @return 简单返回一个字符串
 */
boost::asio::awaitable<std::string> UserService::concurrent_test(int sum, std::string_view url) {

    try {
      // // 直接使用注入的 http_client_ 实例
      // HttpResponse response = co_await http_client_->get("http://192.168.1.176:9001/user/56/DSDS", {});
      // HttpResponse response2 = co_await http_client_->get("http://192.168.1.176:9001/user/56/123123123", {});
      co_await  simulate_high_concurrency(sum,url);
        SPDLOG_DEBUG("<<<<<<<<<<<<<<< {} >>>>>>>>>>>>>>> ");
        co_return "成功 ";
    } catch (const boost::system::system_error& e) {
        // 处理异常
        SPDLOG_ERROR("Failed to get user from external API: {}", e.what());
        co_return "Failed ";
    }

}

boost::asio::awaitable<std::string> UserService::test_http_client(std::string_view url) {
    // 2. 从上下文中获取名为 "kw" 和 "page" 的查询参数
    SPDLOG_DEBUG("准备对：{} 发起请求", url);
    try {
        // 直接使用注入的 http_client_ 实例
        HttpResponse response = co_await http_client_->get(url, {});



        std::string_view content_type;
        auto it = response.find(http::field::content_type);
        if (it != response.end()) {
            content_type = it->value();
            SPDLOG_DEBUG("请求成功 body size = {}，content-type {} ", response.body().size(),content_type);
        }

        co_return response.body();
    } catch (const boost::system::system_error& e) {
        // 处理异常
        SPDLOG_ERROR("Failed to get user from external API: {}", e.what());
        co_return "Failed ";
    }

}

boost::asio::awaitable<std::string> UserService::connect_to_status_stream(std::string_view body) const {
        try {
            // 1. 创建一个 Handler 实例
            const auto handler = UserStatusHandler::create();

            SPDLOG_INFO("UserService: Attempting to connect to WebSocket status stream...");

            // 2. 调用 WebSocketClient 的 connect 方法
            //    connect 是一个协程，它会在 TCP/TLS 连接建立后返回。
            //    我们不需要等待 WebSocket 握手。
            const auto connection = co_await ws_client_->connect(
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

boost::asio::awaitable<double> UserService::intensiveComputing() {

    // [!!! 这才是正确的卸载方式 !!!]
    // 我们在 worker_pool_ 上 co_spawn 一个新的协程来执行阻塞任务，
    // 然后 co_await 它的结果。
    auto [e, result] = co_await boost::asio::co_spawn(
        // 1. 目标执行器：工作线程池
        worker_executor_,

        // 2. 要在 worker 线程上执行的【协程 lambda】
        [this]() -> boost::asio::awaitable<double> {
            // 这个协程的【全部】内容都在 worker 线程上运行
            SPDLOG_INFO("进入工作线程...");

            // 调用阻塞函数
            double compute_result = heavy_compute();



            // 使用 co_return 返回结果
            co_return compute_result;
        },

        // 3. 完成令牌：告诉 co_spawn 我们希望 co_await 它的结果
        boost::asio::as_tuple(boost::asio::use_awaitable)
    );


    co_return result;
}

double UserService::heavy_compute() {
    SPDLOG_INFO("工作线程：开始 30 秒的阻塞工作...");
    auto start = std::chrono::steady_clock::now();
    double result = 0;
    while (std::chrono::steady_clock::now() - start < 10s) {
        // 执行一些复杂的数学计算，例如

        for (int i = 0; i < 1000; ++i) {
            result += std::sin(i) * std::cos(i);
        }
    }
    SPDLOG_INFO("工作线程：工作完成...");
    return result;
}
