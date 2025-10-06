//
// Created by Aiziboy on 2025/10/5.
//

#ifndef UNTITLED1_LOGGER_MANAGER_HPP
#define UNTITLED1_LOGGER_MANAGER_HPP
#include <memory>
#include <vector>
#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>

// 将 C++14 的时间字面量（如 30s）引入当前作用域
using namespace std::literals::chrono_literals;

class LoggerManager {
public:
    enum class Mode {
        // 压测模式：关闭日志
        Benchmark,

        // 开发模式：彩色控制台，DEBUG 级别
        Development,

        // 生产模式：文件 + 控制台，INFO 级别
        Production
    };

    static LoggerManager& instance() {
        static LoggerManager inst;
        return inst;
    }

    void init(Mode mode) {
        // 初始化线程池（容量、线程数）
        spdlog::init_thread_pool(65536, 2);

        std::vector<spdlog::sink_ptr> sinks;

        switch (mode) {
            case Mode::Benchmark: {
                // 压测模式：关闭日志
                auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
                sinks.push_back(null_sink);
                default_logger_ = std::make_shared<spdlog::async_logger>(
                    "benchmark_logger", sinks.begin(), sinks.end(),
                    spdlog::thread_pool(),
                    spdlog::async_overflow_policy::overrun_oldest);
                spdlog::set_level(spdlog::level::off);
                break;
            }
            case Mode::Development: {
                // 开发模式：彩色控制台，DEBUG 级别
                auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                sinks.push_back(console_sink);
                default_logger_ = std::make_shared<spdlog::async_logger>(
                    "dev_logger", sinks.begin(), sinks.end(),
                    spdlog::thread_pool(),
                    spdlog::async_overflow_policy::overrun_oldest);
                spdlog::set_level(spdlog::level::debug);
                break;
            }
            case Mode::Production: {
                // 生产模式：文件 + 控制台，INFO 级别
                auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                    "./logs/app.log", true);
                sinks.push_back(console_sink);
                sinks.push_back(file_sink);
                default_logger_ = std::make_shared<spdlog::async_logger>(
                    "prod_logger", sinks.begin(), sinks.end(),
                    spdlog::thread_pool(),
                    spdlog::async_overflow_policy::overrun_oldest);
                spdlog::set_level(spdlog::level::info);
                spdlog::flush_every(5s);
                spdlog::flush_on(spdlog::level::err);
                break;
            }
        }

        spdlog::set_default_logger(default_logger_);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e %z] [thread %t] [%s:%#] [%^%l%$] %v");

        SPDLOG_DEBUG("Logger level : {}",spdlog::level::to_string_view(spdlog::default_logger()->level()));
    }

private:
    LoggerManager() = default;
    std::shared_ptr<spdlog::logger> default_logger_;
};
#endif //UNTITLED1_LOGGER_MANAGER_HPP