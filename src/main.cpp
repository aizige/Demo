#include <filesystem>
#include "utils/toml.hpp"
#include <iostream>
#include "Application.hpp"
#include "http/network_constants.hpp"
#include "utils/config/ConfigLoader.hpp"







int main() {
    try {


        const AizixConfig config = ConfigLoader::load("../config.toml");
        Application app(config);
        SPDLOG_DEBUG("{}",std::chrono::steady_clock::now().time_since_epoch().count());
        SPDLOG_DEBUG("steady_clock 纪元以来的秒数:   {}",time_utils::steady_clock_seconds_since_epoch());
        SPDLOG_DEBUG("steady_clock 纪元以来的毫秒数:  {}",time_utils::steady_clock_ms_since_epoch());
        SPDLOG_DEBUG("steady_clock 纪元以来的纳秒数:  {}",time_utils::steady_clock_ns_since_epoch());
        SPDLOG_DEBUG("获取当前时间的 Unix 时间戳（秒）:  {}",time_utils::system_clock_seconds_since_epoch());
        SPDLOG_DEBUG("获取当前时间的 Unix 时间戳（毫秒）: {}",time_utils::system_clock_ms_since_epoch());
        return app.run();
    } catch (const std::exception& e) {
        // 捕获配置加载或应用构造时的早期错误
        std::cerr << "Fatal error during application startup: " << e.what() << std::endl;
        return 1;
    }
}
