cmake_minimum_required(VERSION 4.0)
project(Antsentinel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

#set(CMAKE_VERBOSE_MAKEFILE ON)    # 打印详细的构建命令（包括编译器调用、链接参数等）。

# Debug调试设置选项Release构建会关闭它们。
# 添加编译器参数，用于调试和内存错误检测：
# -fsanitize=address	    启用 AddressSanitizer，检测内存越界、use-after-free 等
# -fno-omit-frame-pointer	保留帧指针，便于调试器和 ASan 追踪调用栈
# -g	                    生成调试信息（DWARF），用于 GDB 或 LLDB 调试
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer -g")

message(STATUS "Current Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "CXX Flags for Release: ${CMAKE_CXX_FLAGS_RELEASE}")

add_subdirectory(aizix)
add_subdirectory(xingju)

