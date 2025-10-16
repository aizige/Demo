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
#include "spdlog/sinks/rotating_file_sink.h"

// 将 C++14 的时间字面量（如 30s）引入当前作用域
using namespace std::literals::chrono_literals;

class LoggerManager {
public:
    enum class mode {
        // 关闭日志
        off,

        // 开发模式：彩色控制台，DEBUG 级别
        dev,

        // 生产模式：文件轮转
        prod
    };

    static LoggerManager& instance() {
        static LoggerManager inst;
        return inst;
    }

     void init(mode mode) {
        // 初始化线程池（队列大小 16384，后台线程数 1）


        spdlog::init_thread_pool(16384, 1);

        std::vector<spdlog::sink_ptr> sinks;

        switch (mode) {
        case mode::off: {
            // 压测模式：关闭日志
            auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
            sinks.push_back(null_sink);
            default_logger_ = std::make_shared<spdlog::async_logger>("benchmark_logger", sinks.begin(), sinks.end(),
                                                                    spdlog::thread_pool(),
                                                                    spdlog::async_overflow_policy::overrun_oldest);
            spdlog::set_default_logger(default_logger_);
            spdlog::set_level(spdlog::level::off);

            break;
        }
        case mode::dev: {
            // 开发模式：彩色控制台，DEBUG 级别
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            sinks.push_back(console_sink);
            default_logger_ = std::make_shared<spdlog::async_logger>("dev_logger", sinks.begin(), sinks.end(),
                                                                    spdlog::thread_pool(),
                                                                    spdlog::async_overflow_policy::overrun_oldest);
            spdlog::set_default_logger(default_logger_);
            console_sink->set_level(spdlog::level::debug);
            spdlog::set_level(spdlog::level::debug);
            break;
        }
        case mode::prod: {
            // 生产模式：文件 + 控制台，INFO 级别
            // 创建轮转 file_sink（文件大小上限 5MB，最多保留 3 个文件）
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                "logs/app.log", 5 * 1024 * 1024, 50);


            sinks.push_back(file_sink);
            default_logger_ = std::make_shared<spdlog::async_logger>("prod_logger", sinks.begin(), sinks.end(),
                                                                    spdlog::thread_pool(),
                                                                    spdlog::async_overflow_policy::block);

            spdlog::set_default_logger(default_logger_);
            file_sink->set_level(spdlog::level::info);
            spdlog::set_level(spdlog::level::info);
            break;
        }
        }
        spdlog::flush_every(std::chrono::seconds(5s));
        spdlog::flush_on(spdlog::level::err);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e %z] [thread %t] [%s:%#] [%^%l%$] %v");

        SPDLOG_DEBUG("Logger level : {}", spdlog::level::to_string_view(spdlog::default_logger()->level()));
    }
private:
    LoggerManager() = default;
    std::shared_ptr<spdlog::logger> default_logger_;
};
#endif //UNTITLED1_LOGGER_MANAGER_HPP
