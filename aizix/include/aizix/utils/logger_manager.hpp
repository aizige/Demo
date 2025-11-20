//
// Created by Aiziboy on 2025/10/5.
//

#ifndef AIZIX_LOGGER_MANAGER_HPP
#define AIZIX_LOGGER_MANAGER_HPP
#include <memory>
#include <vector>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h> // 用于彩色控制台输出
#include <spdlog/sinks/basic_file_sink.h>    // 用于文件输出
#include <spdlog/async.h>                   // 异步模式需要
#include <spdlog/sinks/null_sink.h>
#include <chrono>               // std::chrono::seconds 等时间单位
#include <spdlog/sinks/rotating_file_sink.h>

#include <aizix/utils/config/AizixConfig.hpp>

namespace aizix {
    class LoggerManager {
    public:
        static void init(const LoggingConfig& config) {
            try {
                // 1. 初始化线程池 (建议: 队列大小 8192，线程数 1)
                spdlog::init_thread_pool(8192, 1);

                std::vector<spdlog::sink_ptr> sinks;

                // 2. 配置 Sinks

                // 控制台 Sink
                if (config.output_type == "console" || config.output_type == "all") {
                    // 开发模式：彩色控制台，DEBUG 级别
                    const auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                    sinks.push_back(console_sink);
                }

                // 文件 Sink
                if (config.output_type == "file" || config.output_type == "all") {
                    // 生产模式：文件 + 控制台，INFO 级别
                    // 创建轮转 file_sink（文件大小上限 5MB，最多保留 3 个文件）
                    const auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                        config.file_path, config.max_size_mb * 1024 * 1024, config.max_files);

                    sinks.push_back(file_sink);
                    //file_sink->set_level(spdlog::level::info);
                }

                // 如果没有配置任何 sink (例如 output 设置为 "off" 或其他无效值)，则使用 null_sink
                if (sinks.empty()) {
                    // 压测模式：关闭日志
                    const auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
                    sinks.push_back(null_sink);
                    spdlog::set_level(spdlog::level::off);
                }

                // 3. 创建异步 Logger
                // 使用局部变量，创建完交给 spdlog 全局管理即可
                const std::shared_ptr<spdlog::logger> async_logger_ptr = std::make_shared<spdlog::async_logger>(
                    "main_logger", // 给 logger 起个名
                    sinks.begin(), sinks.end(),
                    spdlog::thread_pool(),
                    spdlog::async_overflow_policy::overrun_oldest
                );

                // 4. 设置级别
                const spdlog::level::level_enum log_level = spdlog::level::from_str(config.level);
                async_logger_ptr->set_level(log_level);

                // 5. 全局注册 (移交所有权)
                spdlog::set_default_logger(async_logger_ptr);
                spdlog::set_level(log_level);


                // 6. 自动刷盘策略 (重要：异步日志如果不自动刷盘，崩溃时会丢失最近几秒的日志)
                using namespace std::chrono_literals;
                spdlog::flush_every(10s);             // 每 10 秒
                spdlog::flush_on(spdlog::level::err); // 遇到错误立刻刷盘

                // 7. 设置日志格式
                spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e %z] [thread %t] [%s:%#] [%^%l%$] %v");

                SPDLOG_INFO("Logger level : {}", spdlog::level::to_string_view(spdlog::default_logger()->level()));
            } catch (const spdlog::spdlog_ex& ex) {
                // 如果日志初始化失败，只能打印到 stderr
                fprintf(stderr, "Log init failed: %s\n", ex.what());
            }
        }

        // 手动关闭的方法，在 main 退出前调用
        static void shutdown() {
            spdlog::shutdown();
        }

    private:
        LoggerManager() = default;

        ~LoggerManager() {
        }

        // 删除拷贝和赋值
        LoggerManager(const LoggerManager&) = delete;
        LoggerManager& operator=(const LoggerManager&) = delete;
    };
}
#endif //AIZIX_LOGGER_MANAGER_HPP
