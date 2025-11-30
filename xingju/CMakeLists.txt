cmake_minimum_required(VERSION 4.0)
project(xingju LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(spdlog REQUIRED)

add_executable(xingju
        src/main.cpp
        src/controller/user_controller.cpp
        src/controller/user_service.cpp
        src/controller/XingjuController.cpp
        src/ServiceInstanceManager.cpp
        src/error/xingju_error.hpp
)

target_include_directories(xingju PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

target_link_libraries(xingju PRIVATE
        aizix
        spdlog::spdlog
)


