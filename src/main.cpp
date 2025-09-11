#include "core/server.hpp"
#include "utils/spdlog_config.hpp"
#include "controller/user_controller.hpp"
#include <filesystem>
#include <chrono>
#include "core/client/http_client.hpp"

using namespace std::literals::chrono_literals;

int main() {
    try {
        // --- 1. 初始设置 ---
        spdlog_config::initLoggers();
        spdlog::set_level(spdlog::level::trace); // 生产环境建议 info 级别

        const nghttp2_info *lib_info = nghttp2_version(0);
        std::cout << "📦 libnghttp2 version: " << lib_info->version_str << std::endl;
        std::cout << "📁 Workdir: " << std::filesystem::current_path() << std::endl;

        // --- 2. 创建核心服务和配置 ---
        boost::asio::io_context io;
        Server server(io, 8080);

        server.set_tls("dev-cert/server.crt", "dev-cert/server.key");

        auto connection_manager = std::make_shared<ConnectionManager>(io);
        // 1. 创建底层服务
        auto http_client = std::make_shared<HttpClient>(connection_manager);
        auto ws_client = std::make_shared<WebSocketClient>(io);

        // 2. 创建业务服务，注入依赖
        auto user_service = std::make_shared<UserService>(http_client,ws_client);

        // 3. 创建控制器，注入业务服务
        auto user_controller = std::make_shared<UserController>(user_service);
        user_controller->register_routes(server.router());


        // --- 3. 设置信号处理和优雅关闭逻辑 ---
        boost::asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&server, &io, &connection_manager,&signals](const boost::system::error_code &error, int signal_number) {
            if (!error) {
                SPDLOG_WARN("Received signal {}, starting graceful shutdown...", signal_number);

                // 立即取消未来的信号等待，防止重复触发
                    signals.cancel();

                // 启动一个“顶级关闭协程”，它将负责所有清理工作
                boost::asio::co_spawn(
                    io,
                    // 这个 lambda 就是我们的完整关闭流程
                    [&]() -> boost::asio::awaitable<void> {

                        // a. 关闭服务端，停止接受新连接并关闭现有会话
                        SPDLOG_INFO("Shutting down server sessions...");
                        server.stop();

                        // b. 关闭客户端连接管理器，这将关闭所有池中的连接
                        SPDLOG_INFO("Shutting down client connections...");
                        co_await connection_manager->stop();

                        // c. [可选] 关闭其他需要清理的服务
                        // co_await some_other_service->stop();

                        // d. 在所有异步清理工作都完成后，才停止 io_context
                        SPDLOG_INFO("All services stopped. Stopping io_context...");
                        io.stop();
                    },
                    boost::asio::detached // 我们不关心这个协程的结果，让它独立运行
                );
            }
        });

        // --- 4. 启动服务器和工作线程 ---

        // a. 提交服务器的监听任务
        server.run();

        // b. 创建工作线程池
        std::vector<std::thread> threads;
        // 建议不要占用所有核心，留一个给操作系统或其他进程
        size_t concurrency = std::max(1u, std::thread::hardware_concurrency() - 1);
        threads.reserve(concurrency);
        for (size_t i = 0; i < concurrency; ++i) {
            threads.emplace_back([&]() { io.run(); });
        }

        // TODO: [BUG-Potential]: ioc.run() 在主线程和工作线程中都被调用。这通常是正确的，但当 ioc.run() 因为没有更多工作而返回时，程序可能会在工作线程仍在运行时就退出。更健壮的模式是在 main 函数的末尾等待所有 ioc.run() 调用结束。你可以通过 asio::signal_set 来优雅地停止 io_context。

        // c. 主线程也加入工作，这使得信号处理可以在主线程上被触发
        SPDLOG_INFO("Server started on port 8080. Press Ctrl+C to shut down.");
        io.run();

        // --- 5. 等待所有线程结束 ---
        // io.run() 返回后，意味着 io_context 已经停止，所有工作线程也将很快退出
        for (auto &t: threads) {
            if (t.joinable()) {
                t.join();
            }
        }

        SPDLOG_INFO("Server shut down gracefully.");
        return 0;
    } catch (const std::exception &e) {
        SPDLOG_ERROR("Fatal error during server startup: {}", e.what());
        return 1;
    }
}
