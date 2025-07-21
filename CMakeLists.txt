cmake_minimum_required(VERSION 4.0)
project(untitled1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Boost REQUIRED COMPONENTS system thread coroutine)
find_package(nlohmann_json REQUIRED)
find_package(libnghttp2)
find_package(spdlog)
find_package(stduuid)
find_package(ada)






# 调试用的
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer -g")
#set(CMAKE_BUILD_TYPE Debug)

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
        src/core/client/http_ssl_connection.hpp
        src/core/client/connection_helpers.hpp
        src/core/client/h2_connection_base.hpp
        src/core/client/h2_connection.cpp
        src/core/client/h2_connection.hpp

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
)
