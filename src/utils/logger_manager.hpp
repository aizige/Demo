//
// Created by Aiziboy on 2025/10/5.
//

#ifndef UNTITLED1_LOGGER_MANAGER_HPP
#define UNTITLED1_LOGGER_MANAGER_HPP
#include <memory>
#include <vector>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h> // 用于彩色控制台输出
#include <spdlog/sinks/basic_file_sink.h>    // 用于文件输出
#include <spdlog/async.h>                   // 异步模式需要
#include <spdlog/sinks/null_sink.h>
#include <chrono>               // std::chrono::seconds 等时间单位
#include "config/AizixConfig.hpp"
#include <spdlog/sinks/rotating_file_sink.h>


using namespace std::literals::chrono_literals;
using namespace std::chrono_literals;
class LoggerManager {
public:

    static LoggerManager& instance() {
        static LoggerManager inst;
        return inst;
    }

     void init(const LoggingConfig& config) {
        // 初始化线程池（队列大小 16384，后台线程数 1）


        spdlog::init_thread_pool(16384, 1);

        std::vector<spdlog::sink_ptr> sinks;


        if (config.output_type == "console" || config.output_type == "all") {
            // 开发模式：彩色控制台，DEBUG 级别
            const auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            sinks.push_back(console_sink);

        }

        if (config.output_type == "file" || config.output_type == "all") {
            // 生产模式：文件 + 控制台，INFO 级别
            // 创建轮转 file_sink（文件大小上限 5MB，最多保留 3 个文件）
            const auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                config.file_path, config.max_size_mb  * 1024 * 1024, config.max_files);

            sinks.push_back(file_sink);
            //file_sink->set_level(spdlog::level::info);
        }

        // 如果没有配置任何 sink (例如 output 设置为 "off" 或其他无效值)，则使用 null_sink
        if (sinks.empty()) {
            // 压测模式：关闭日志
            auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
            sinks.push_back(null_sink);
            spdlog::set_level(spdlog::level::off);
        }

        // 3. 创建异步 logger
        // 使用 overrun_oldest 策略，在高负载下丢弃旧日志，避免阻塞业务线程
        default_logger_ = std::make_shared<spdlog::async_logger>(
            "main_logger",
            sinks.begin(), sinks.end(),
            spdlog::thread_pool(),
            spdlog::async_overflow_policy::overrun_oldest
        );

        // 5. 根据 config.level 设置日志级别
        // 我们将字符串级别转换为 spdlog 的枚举
        const spdlog::level::level_enum log_level = spdlog::level::from_str(config.level);
        spdlog::set_default_logger(default_logger_);
        spdlog::set_level(log_level);


        // 6. 设置通用配置 (格式、刷写策略等)
        using namespace std::chrono;

        // 假设 interval 是一个 std::chrono::duration 类型，
        // 比如我们定义它为存储秒数的 duration：
        spdlog::flush_every(10s);   // 每 5 秒
        spdlog::flush_on(spdlog::level::err);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e %z] [thread %t] [%s:%#] [%^%l%$] %v");

        SPDLOG_INFO("Logger level : {}", spdlog::level::to_string_view(spdlog::default_logger()->level()));
    }
private:
    LoggerManager() = default;
    ~LoggerManager() {
        // 在程序退出时确保所有日志都被刷写
        spdlog::shutdown();
    }
    std::shared_ptr<spdlog::logger> default_logger_;
};
#endif //UNTITLED1_LOGGER_MANAGER_HPP
