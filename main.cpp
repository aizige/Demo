
#include <filesystem>

#include "src/utils/toml.hpp"

#include <iostream>

#include "Application.hpp"

#include "utils/config/ConfigLoader.hpp"







int main() {
    try {
        const AizixConfig config = ConfigLoader::load("../config.toml");
        Application app(config);
        return app.run();
    } catch (const std::exception& e) {
        // 捕获配置加载或应用构造时的早期错误
        std::cerr << "Fatal error during application startup: " << e.what() << std::endl;
        return 1;
    }
}
