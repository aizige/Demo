cmake_minimum_required(VERSION 4.0)
project(untitled1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- 查找所有依赖 ---
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Boost REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(libnghttp2 REQUIRED) # **确保是 REQUIRED，如果找不到就应该立即失败**
find_package(spdlog REQUIRED)
find_package(stduuid REQUIRED)
find_package(ada REQUIRED)

#add_compile_options(-fcoroutines) # 启用编译器对协程的支持（MSVC编译器或某些旧版本编译器需要显式开启）对于 GCC/Clang，通常不需要这个选项，只要设置std=c++20就行。
#set(CMAKE_VERBOSE_MAKEFILE ON)    # 打印详细的构建命令（包括编译器调用、链接参数等）。

# Debug调试设置选项Release构建会关闭它们。
# 添加编译器参数，用于调试和内存错误检测：
# -fsanitize=address	    启用 AddressSanitizer，检测内存越界、use-after-free 等
# -fno-omit-frame-pointer	保留帧指针，便于调试器和 ASan 追踪调用栈
# -g	                    生成调试信息（DWARF），用于 GDB 或 LLDB 调试
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer -g")
#set(CMAKE_BUILD_TYPE Debug)  # 设置构建类型为 Debug

add_executable(untitled1
        src/main.cpp
        src/core/h2_session.cpp
        src/core/h2c_session.cpp
        src/core/client/http1_connection.cpp
        src/core/client/connection_manager.cpp
        src/core/client/http_client.cpp
        src/http/request_context.cpp
        src/http/router.cpp
        src/utils/decompressor.cpp
        src/utils/decompression_manager.cpp
        src/utils/compressor.cpp
        src/core/client/http_ssl_connection.cpp
        src/core/client/h2_connection.cpp
        src/core/client/h2c_connection.cpp
        src/core/client/WebSocketConnection.cpp
        src/core/client/WebSocketClient.cpp
        src/impl/user_service.cpp
        src/utils/config/config_loader.cpp
)

add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

target_include_directories(untitled1 PRIVATE ${PROJECT_SOURCE_DIR}/src)


target_link_libraries(untitled1 PRIVATE
        Boost::boost
        openssl::openssl
        ZLIB::ZLIB
        nlohmann_json::nlohmann_json
        libnghttp2::libnghttp2
        spdlog::spdlog
        stduuid::stduuid
        ada::ada
)
