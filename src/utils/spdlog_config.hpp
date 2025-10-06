//
// Created by ubuntu on 2025/6/29.
//

//
// Created by Aiziboy on 25-6-28.
//
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h> // 用于彩色控制台输出
#include <spdlog/sinks/basic_file_sink.h>    // 用于文件输出
#include <spdlog/async.h>                   // 异步模式需要

/**
 * @class spdlog_config
 * @brief spdlog的配置类
 */

// 将 C++14 的时间字面量（如 30s）引入当前作用域
using namespace std::literals::chrono_literals;
class spdlog_config {
public:
    static void initLoggers() {
        // 初始化 spdlog 的线程池。这是异步日志的关键，
        // 指定了日志消息队列的容量和处理这些消息的后台线程数量。
        spdlog::init_thread_pool(65536, 2); // 队列容量 & 后台线程数


        // 创建用于不同输出目标的 sinks（日志目的地）。
        // stdout_color_sink_mt 用于彩色控制台输出，basic_file_sink_mt 用于文件输出。
        // _mt 后缀表示它们是线程安全的（multi-threaded）。
        const auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        //console_sink->set_level(spdlog::level::debug); // 控制台 sink 级别设置为 debug
        //console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e %z] [thread %t] [%s:%#] [%^%l%$] %v");   // ******不需要设置后面设置全局格式********

        const auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("./logs/async_log.txt", true);
        //file_sink->set_level(spdlog::level::debug); // 控制台 sink 级别设置为 debug
        //file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e %z] [thread %t] [%s:%#] [%^%l%$] %v");  // ******不需要设置后面设置全局格式********

        // 将创建的 sinks 添加到容器中，以便统一管理。
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(console_sink);
        sinks.push_back(file_sink);


        // 创建一个名为 "combined_logger" 的异步日志器。
        // 这个日志器会使用上面定义的 sinks 来输出日志，并利用线程池实现异步写入。
        const auto combined_logger = std::make_shared<spdlog::async_logger>("combined_logger", sinks.begin(), sinks.end(),
                                                                            spdlog::thread_pool(),
                                                                            spdlog::async_overflow_policy::overrun_oldest);


        // 将我们自定义的 combined_logger 设置为 spdlog 的全局默认日志器。
        // 之后所有不指定日志器名称的 SPDLOG_X 调用都将使用此日志器。
        spdlog::set_default_logger(combined_logger);


        // 设置全局日志消息的输出格式。这个模式定义了日志信息的各个组成部分，
        // 如时间、线程ID、源文件、行号、日志级别和具体消息。
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e %z] [thread %t] [%s:%#] [%^%l%$] %v");

        SPDLOG_DEBUG("Logger level : {}",
                     spdlog::level::to_string_view(spdlog::default_logger()->level()));

        // 设置全局日志器的最低输出级别为 DEBUG。
        // 这一步至关重要，因为它确保了 DEBUG 级别的消息能够通过默认日志器输出。
        // 放置在这里是为了在 combined_logger 成为默认日志器后立即对其生效。
        //spdlog::set_level(level == spdlog::level::debug ? spdlog::level::debug : spdlog::level::warn);
        // 配置日志的刷新策略。
        // flush_every(): 每隔 5 秒强制刷新一次日志，确保日志不会长时间滞留在缓冲区。
        // flush_on(): 当日志级别达到 ERROR 或更高时，立即刷新日志。
        spdlog::flush_every(std::chrono::seconds(5s));
        spdlog::flush_on(spdlog::level::err);

    }
};
