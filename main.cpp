#include "src/core/server.hpp"
#include "src/utils/spdlog_config.hpp"
#include "src/controller/user_controller.hpp"
#include <filesystem>
#include <chrono>
#include "src/core/client/http_client.hpp"
#include "src/utils/toml.hpp"
#include <spdlog/sinks/null_sink.h>

#include "src/utils/logger_manager.hpp"
#include "src/utils/thread_utils.hpp"

using namespace std::literals::chrono_literals;

int main() {
    try {
        LoggerManager::instance().init(LoggerManager::mode::dev);
        nghttp2_info* lib_info = nghttp2_version(0);
        std::cout << "📦 libnghttp2 version: " << lib_info->version_str << std::endl;
        std::cout << "📁 Workdir: " << std::filesystem::current_path() << std::endl;



        // --- 1. 初始设置 ---
        size_t io_threads_count = std::thread::hardware_concurrency() / 2; // 专门用于I/O的线程数 ÷2一个合理的起点
        size_t worker_threads_count = std::thread::hardware_concurrency() - io_threads_count; // 专门用于计算的线程数
        if (worker_threads_count <= 0) worker_threads_count = 4;


        // --- 2. 创建核心服务和配置 ---
        // I/O Context for network operations
        boost::asio::io_context ioc;

        // 增加一个 work_guard，防止 io_context 在没有任务时退出
        auto work_guard = boost::asio::make_work_guard(ioc.get_executor());

        // Thread Pool for CPU-bound tasks
        auto worker_pool = std::make_shared<boost::asio::thread_pool>(worker_threads_count);

        Server server(ioc, 8080);
        server.set_tls("../dev-cert/server.crt", "../dev-cert/server.key");

        auto connection_manager = std::make_shared<ConnectionManager>(ioc);
        // 1. 创建底层服务
        auto http_client = std::make_shared<HttpClient>(connection_manager);
        auto ws_client = std::make_shared<WebSocketClient>(ioc);

        // 2. 创建业务服务，注入依赖
        auto user_service = std::make_shared<UserService>(ioc.get_executor(), worker_pool->get_executor(), http_client, ws_client);

        // 3. 创建控制器，注入业务服务
        auto user_controller = std::make_shared<UserController>(user_service);
        user_controller->register_routes(server.router());


        // --- 3. 设置信号处理和优雅关闭逻辑 ---
        boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&server, &ioc, &connection_manager,&signals](const boost::system::error_code& error, int signal_number) {
            if (!error) {
                SPDLOG_INFO("Received signal {}, starting graceful shutdown...", signal_number);

                // 立即取消未来的信号等待，防止重复触发
                signals.cancel();

                // 启动一个“顶级关闭协程”，它将负责所有清理工作
                boost::asio::co_spawn(
                    ioc,
                    // 这个 lambda 就是我们的完整关闭流程
                    [&]() -> boost::asio::awaitable<void> {
                        // a. 关闭服务端，停止接受新连接并关闭现有会话
                        SPDLOG_INFO("Shutting down server sessions...");
                        co_await server.stop();

                        // b. 关闭客户端连接管理器，这将关闭所有池中的连接
                        SPDLOG_INFO("Shutting down client connections...");
                        co_await connection_manager->stop();

                        // c. [可选] 关闭其他需要清理的服务
                        // co_await some_other_service->stop();

                        // d. 在所有异步清理工作都完成后，才停止 io_context
                        SPDLOG_INFO("All services stopped. Stopping io_context...");
                        ioc.stop();
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
        threads.reserve(io_threads_count);
        for (size_t i = 0; i < io_threads_count; ++i) {
            ThreadUtils::set_current_thread_name("io_worker_" + std::to_string(i + 1));
            threads.emplace_back([&ioc]() { ioc.run(); });
        }

        SPDLOG_DEBUG("🧵 HTTP service io_context线程数[{}], worker_threads数:[{}]", io_threads_count, worker_threads_count);

        SPDLOG_DEBUG("Server started on port 8080. Press Ctrl+C to shut down.");

        // c. 主线程也加入 I/O 工作，这使得信号处理可以在主线程上被触发. 并为自己命名
        ThreadUtils::set_current_thread_name("io_worker_main");
        ioc.run();


        // --- 5. 等待所有线程结束 ---
        // io.run() 返回后，意味着 io_context 已经停止，所有工作线程也将很快退出
        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }

        SPDLOG_INFO("Server shut down gracefully.");
        spdlog::shutdown(); // 关闭日志
        return 0;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Fatal error during server startup: {}", e.what());
        spdlog::shutdown(); // 关闭日志
        return 1;
    }
}
