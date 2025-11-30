//
// Created by Aiziboy on 2025/11/16.
//

#include <filesystem>
#include <iostream>
#include <aizix/utils/config/ConfigLoader.hpp>
#include <aizix/App.hpp>
#include "controller/user_controller.hpp"
#include "controller/user_service.hpp"
#include "controller/XingjuController.hpp"
#include <exception> // for std::exception_ptr
#include <boost/asio.hpp>
#include <chrono>




int main() {
    try {
        // 1. 初始化容器
        aizix::App app("config.toml");

        // 2. 组装业务 (Dependency Injection)
        // 使用 auto 即可，类型推导很清晰
        auto user_service = std::make_shared<UserService>(
            app.get_main_ioc().get_executor(),
            app.worker_pool_executor(),
            app.httpClient(),
            app.webSocketClient()
        );

        const auto service_instance_manager = std::make_shared<ServiceInstanceManager>(app.get_main_ioc());
        const auto user_controller = std::make_shared<UserController>(user_service);
        const auto xingju_controller = std::make_shared<XingjuController>(service_instance_manager);

        // 3. 注册到容器
        app.addController(user_controller);
        app.addController(xingju_controller);

        // 4. 启动 (阻塞直到退出信号)
        // run() 内部已经处理了大部分异常，但在 main 里兜底是个好习惯
        return app.run();
    } catch (const std::exception& e) {
        // 万一构造函数或者依赖注入阶段抛出异常 (比如配置文件不存在)
        // 使用 fprintf 或 std::cerr 确保能在控制台看到
        fprintf(stderr, "Critical Error during startup: %s\n", e.what());
        return 1; // 返回非 0 表示异常退出
    }
}
