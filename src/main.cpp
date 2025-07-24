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
        spdlog::set_level(spdlog::level::warn); // 生产环境建议 info 级别

        const nghttp2_info* lib_info = nghttp2_version(0);
        std::cout << "📦 libnghttp2 version: " << lib_info->version_str << std::endl;
        std::cout << "📁 Workdir: " << std::filesystem::current_path() << std::endl;

        // --- 2. 创建核心服务和配置 ---
        boost::asio::io_context io;
        Server server(io, 8080);

        server.set_tls("dev-cert/server.crt1", "dev-cert/server.key1");



        // 1. 创建底层服务
        auto http_client = std::make_shared<HttpClient>(io);

        // 2. 创建业务服务，注入依赖
        auto user_service = std::make_shared<UserService>(http_client);

        // 3. 创建控制器，注入业务服务
        auto user_controller = std::make_shared<UserController>(user_service);
        user_controller->register_routes(server.router());


        // --- 3. 设置信号处理和优雅关闭逻辑 ---
        boost::asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&server, &io](const boost::system::error_code& error, int signal_number) {
            if (!error) {
                SPDLOG_WARN("Received signal {}, starting graceful shutdown...", signal_number);

                // a. 调用 server.stop() 来关闭 acceptor 和通知 sessions
                server.stop();

                // b. (可选) 可以在这里启动一个短暂的计时器，给 session 一点时间去完成 GOAWAY
                // 2. 创建一个计时器，给予一小段延时（例如 1 秒）
                //    这使得 io_context 有时间去调度和执行发送 GOAWAY 帧的 do_write() 操作
                auto timer = std::make_shared<boost::asio::steady_timer>(io);
                timer->expires_after(1s); // 给予 1 秒的“宽限期”

                SPDLOG_INFO("Waiting 1s for sessions to send GOAWAY...");

                // 3. 异步等待计时器结束
                timer->async_wait(
                    // **在计时器结束后，再停止 io_context**
                    [&io](const boost::system::error_code& /*ec*/) {
                        SPDLOG_INFO("Grace period finished. Stopping io_context.");
                        io.stop();
                    }
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
        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }

        SPDLOG_INFO("Server shut down gracefully.");
        return 0;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Fatal error during server startup: {}", e.what());
        return 1;
    }
}
