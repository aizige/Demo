//
// Created by ubuntu on 2025/7/24.
//

#ifndef UNTITLED1_UTILS_HPP
#define UNTITLED1_UTILS_HPP
// 可以在一个通用的 utils.hpp 文件中
#include <chrono>

namespace time_utils {
    /**
     * @brief 获取自 steady_clock 纪元以来的秒数。
     * steady_clock 保证是单调递增的，适合用来测量时间间隔。
     * @return int64_t 类型的秒时间戳。
     */
    inline int64_t steady_clock_seconds_since_epoch() {
        auto now = std::chrono::steady_clock::now();
        // 计算自 steady_clock 纪元以来的秒数
        return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    }

    /**
     * @brief 获取自 steady_clock 纪元以来的毫秒数。
     * steady_clock 保证是单调递增的，适合用来测量时间间隔。
     * @return int64_t 类型的毫秒时间戳。
     */
    inline int64_t steady_clock_ms_since_epoch() {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    }

    /**
     * @brief 获取自 steady_clock 纪元以来的纳秒数。
     * @return int64_t 类型的纳秒时间戳。
     */
    inline int64_t steady_clock_ns_since_epoch() {
        auto now = std::chrono::steady_clock::now();
        // steady_clock 的精度通常就是纳秒，所以可能不需要 cast
        return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    }

    /**
     * @brief 获取当前时间的 Unix 时间戳（秒）。
     * system_clock 基于现实世界的 UTC 时间，可能会被调整。
     * 适合用于日志记录、与外部系统通信等场景。
     * @return int64_t 类型的秒级 Unix 时间戳 (10位数)。
     */
    inline int64_t system_clock_seconds_since_epoch() {
        auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    }

    /**
     * @brief 获取当前时间的 Unix 时间戳（毫秒）。
     * @return int64_t 类型的毫秒级 Unix 时间戳 (13位数)。
     */
    inline int64_t system_clock_ms_since_epoch() {
        auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    }
}
#endif //UNTITLED1_UTILS_HPP
