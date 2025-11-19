//
// Created by ubuntu on 2025/7/24.
//

#ifndef UNTITLED1_UTILS_HPP
#define UNTITLED1_UTILS_HPP

#include <chrono>

namespace time_utils {
    /**
     * @brief 获取自 steady_clock 纪元以来的秒数。
     * - steady_clock 是测量代码执行时间间隔、网络超时、任务耗时等所有与时间间隔相关的计算的唯一正确选择
     * @return int64_t 类型的秒时间戳。
     */
    inline int64_t steady_clock_seconds_since_epoch() {
        const auto now = std::chrono::steady_clock::now();
        // now.time_since_epoch() 返回一个 duration，是 nanoseconds
        // 我们要把它转换成 seconds
        return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    }

    /**
     * @brief 获取自 steady_clock 纪元以来的毫秒数。
     * - steady_clock 是测量代码执行时间间隔、网络超时、任务耗时等所有与时间间隔相关的计算的唯一正确选择
     * @return int64_t 类型的毫秒时间戳。
     */
    inline int64_t steady_clock_ms_since_epoch() {
        const auto now = std::chrono::steady_clock::now();
        // now.time_since_epoch() 返回一个 duration，是 nanoseconds
        // 我们要把它转换成 milliseconds
        return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    }

    /**
     * @brief 获取自 steady_clock 纪元以来的纳秒数。
     * - steady_clock 是测量代码执行时间间隔、网络超时、任务耗时等所有与时间间隔相关的计算的唯一正确选择
     * @return int64_t 类型的纳秒时间戳。
     */
    inline int64_t steady_clock_ns_since_epoch() {
        const auto now = std::chrono::steady_clock::now();
        // now.time_since_epoch() 返回一个 duration，是 nanoseconds
        // 怕万一以后默认不是nanoseconds精度，所以这里依然转换成nanoseconds，
        return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    }

    /**
     * @brief 获取当前时间的 Unix 时间戳（秒）。
     * - system_clock 基于现实世界的 UTC 时间，可能会被调整。
     * @return int64_t 类型的秒级 Unix 时间戳 (10位数)。
     */
    inline int64_t system_clock_seconds_since_epoch() {
        const auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    }

    /**
     * @brief 获取当前时间的 Unix 时间戳（毫秒）。
     * - system_clock 基于现实世界的 UTC 时间，可能会被调整。
     * @return int64_t 类型的毫秒级 Unix 时间戳 (13位数)。
     */
    inline int64_t system_clock_ms_since_epoch() {
        const auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    }
}
#endif //UNTITLED1_UTILS_HPP
