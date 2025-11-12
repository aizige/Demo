//
// Created by Aiziboy on 2025/11/4.
//

#ifndef AIZIX_VERSION_HPP
#define AIZIX_VERSION_HPP


namespace aizix::framework {
    // 使用 std::string_view 避免不必要的字符串拷贝
    constexpr std::string name = "Aizix";

    // 宏会在编译时被替换为 CMake 中定义的值
    constexpr std::string version = "1.0";
}
#endif //AIZIX_VERSION_HPP
