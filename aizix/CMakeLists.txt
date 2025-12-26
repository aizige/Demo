cmake_minimum_required(VERSION 4.0)
project(aizix LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(aizix) # 默认是静态库, 可以用 add_library(aizix SHARED) 改为动态库

target_sources(aizix PRIVATE
        src/core/h2_session.cpp
        src/core/HttpSession.cpp
        src/core/HttpsSession.cpp
        src/core/Server.cpp
        src/core/client/http_connection.cpp
        src/core/client/HttpClientPool.cpp
        src/core/client/http_client.cpp
        src/http/request_context.cpp
        src/http/router.cpp
        src/utils/decompressor.cpp
        src/utils/compression_manager.cpp
        src/utils/compressor.cpp
        src/core/client/https_connection.cpp
        src/core/client/h2_connection.cpp
        src/core/client/websocket_connection.cpp
        src/core/client/websocket_client.cpp
        src/utils/config/ConfigLoader.cpp
        src/App.cpp
        src/lib/ada.cpp

)
add_compile_definitions(
        BOOST_ASIO_HAS_IO_URING
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

target_include_directories(aizix
        PUBLIC
        # PUBLIC 路径: 使用这个库的人也能看到这个路径
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        PRIVATE
        # PRIVATE 路径: 只有库自己编译时才需要，使用者看不到
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# --- 查找所有依赖 ---
find_package(liburing REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Boost REQUIRED)
find_package(libnghttp2 REQUIRED)
find_package(spdlog REQUIRED)
find_package(libnuma REQUIRED)

# --- 正确地链接库 ---
target_link_libraries(aizix PUBLIC
        liburing::liburing
        boost::boost
        openssl::openssl
        ZLIB::ZLIB
        libnghttp2::libnghttp2
        spdlog::spdlog
        libnuma::libnuma
)
