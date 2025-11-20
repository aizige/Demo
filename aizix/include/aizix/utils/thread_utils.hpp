//
// Created by Aiziboy on 2025/10/16.
//

#ifndef AIZIX_THREAD_UTILS_HPP
#define AIZIX_THREAD_UTILS_HPP
#include <thread>
#include <string>

// 根据不同平台包含相应的头文件
#if defined(__linux__) || defined(__APPLE__)
    #include <pthread.h>
#elif defined(_WIN32)
    #include <windows.h>
    #include <string>
#endif

namespace ThreadUtils {

    /**
     * @brief 为一个 std::thread 对象设置一个可调试的名称。
     *
     * @param t 要命名的线程对象。
     * @param name 线程的名称。在 Linux 上，名字长度会被截断为 15 个字符。
     */
    inline void set_thread_name(std::thread& thread, const std::string& name) {
#if defined(__linux__) || defined(__APPLE__)
        // 使用 pthread_setname_np
        auto handle = thread.native_handle();
        // pthread_setname_np 要求名字长度不能超过 16 字节（包括结尾的 \0）
        std::string short_name = name.substr(0, 15);
        pthread_setname_np(handle, short_name.c_str());
#elif defined(_WIN32)
        // 使用 SetThreadDescription (需要 Windows 10, version 1607 或更高)
        // 将 std::string 转换为 std::wstring
        std::wstring w_name(name.begin(), name.end());
        SetThreadDescription(thread.native_handle(), w_name.c_str());
#else
        // 其他平台，此功能为空操作
        (void)thread;
        (void)name;
#endif
    }

    /**
     * @brief 为当前线程设置一个可调试的名称。
     */
    inline void set_current_thread_name(const std::string& name) {
#if defined(__linux__) || defined(__APPLE__)
        std::string short_name = name.substr(0, 15);
        pthread_setname_np(pthread_self(), short_name.c_str());
#elif defined(_WIN32)
        // ... Windows API to set current thread name ...
#else
        (void)name;
#endif
    }

} // namespace ThreadUtils
#endif //AIZIX_THREAD_UTILS_HPP