cmake_minimum_required(VERSION 3.31.8)
project(untitled1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- 查找所有依赖 ---
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
#find_package(Boost REQUIRED COMPONENTS system thread coroutine)
find_package(Boost)
find_package(nlohmann_json REQUIRED)
find_package(libnghttp2 REQUIRED) # **确保是 REQUIRED，如果找不到就应该立即失败**
find_package(spdlog REQUIRED)
find_package(stduuid REQUIRED)
find_package(ada REQUIRED)

# 使用 file(GLOB_RECURSE ...) 自动收集所有源文件
file(GLOB_RECURSE NGHTTP2_ASIO_SOURCES
        "src/third_party/nghttp2-asio/*.cpp"
        "src/third_party/nghttp2-asio/*.cc"
        "src/third_party/nghttp2-asio/*.c"
)

# 创建一个静态库 target 来编译 nghttp2-asio 的代码
add_library(nghttp2_asio_internal STATIC ${NGHTTP2_ASIO_SOURCES})

# **关键**: 为这个库设置正确的 include 目录和依赖
target_include_directories(nghttp2_asio_internal PUBLIC
        # 它自己的头文件
        ${PROJECT_SOURCE_DIR}/src/third_party/nghttp2-asio
)

target_link_libraries(nghttp2_asio_internal PUBLIC
        # 它需要链接到 Conan 提供的 libnghttp2
        libnghttp2::libnghttp2
        # 它也需要 Boost 和 OpenSSL
       # Boost::asio      # 假设 Conan 提供了 Boost::asio target
        #Boost::system
        boost::boost
        openssl::openssl
)

# 调试用的是hhhh
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer -g")
set(CMAKE_BUILD_TYPE Debug)

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
        src/utils/decompression_manager.hpp
        src/core/client/http_ssl_connection.cpp
        src/core/client/h2_connection.cpp
        src/core/client/h2_connection.hpp
        src/core/client/h2c_connection.cpp
        src/core/client/h2c_connection.hpp
        src/utils/utils.hpp

)

add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

target_include_directories(untitled1 PRIVATE ${PROJECT_SOURCE_DIR}/src)


target_link_libraries(untitled1 PRIVATE
        Boost::system
        Boost::thread
        openssl::openssl
        ZLIB::ZLIB
        nlohmann_json::nlohmann_json
        libnghttp2::libnghttp2
        spdlog::spdlog
        stduuid::stduuid
        ada::ada
        nghttp2_asio_internal
)
