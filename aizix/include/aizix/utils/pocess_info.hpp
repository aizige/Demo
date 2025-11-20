//
// Created by Aiziboy on 2025/10/13.
//

#ifndef AIZIX_POCESS_INFO_HPP
#define AIZIX_POCESS_INFO_HPP


#include <string>
#include <random>
#include <algorithm>
#include <unistd.h> // for getpid() on POSIX systems (Linux, macOS)
// 如果需要支持 Windows，可以包含 <process.h> 并使用 _getpid()

/**
 * @class ProcessInfo
 * @brief 提供进程级别的唯一标识符。
 *
 * 这是一个单例工具，用于在程序启动时生成一个前缀，
 * 以便在多进程/多实例环境中区分日志和ID。
 */
class ProcessInfo {
public:
    /**
     * @brief 获取当前进程的唯一前缀字符串。
     *
     * 在第一次调用时生成前缀，并在后续调用中返回缓存的值。
     * 线程安全。
     * @return const std::string& 进程唯一前缀，例如 "12345-"。
     */
    static const std::string& get_prefix() {
        // C++11保证静态局部变量的初始化是线程安全的。
        static const std::string prefix = generate_prefix();
        return prefix;
    }

private:
    /**
     * @brief [私有] 生成进程唯一前缀的实际逻辑。
     */
    static std::string generate_prefix() {
        // 使用进程ID (PID) 是最简单、最可靠的方式来区分同一台机器上的不同进程。
        // getpid() 返回一个整数，我们将其转换为字符串并附加一个分隔符。
#if defined(_WIN32)
#include <process.h>
        return std::to_string(_getpid()) + "-";
#else
        return std::to_string(getpid()) + "-";
#endif
    }
};

#endif //AIZIX_POCESS_INFO_HPP