//
// Created by Aiziboy on 2025/11/12.
//

#ifndef AIZIX_APPLICATION_HPP
#define AIZIX_APPLICATION_HPP
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>

#include "controller/user_controller.hpp"
#include "core/Server.hpp"
#include "core/client/connection_manager.hpp"
#include "core/client/http_client.hpp"
#include "utils/config/AizixConfig.hpp"


class Application {
public:
    explicit Application(const AizixConfig& config);

    // 运行应用的主入口
    int run();

private:
    void setup_threading();
    void setup_services();
    void setup_signal_handling();

    // 辅助函数
    static void bind_thread_to_core(size_t core_id);
    static std::vector<std::vector<int>> get_numa_topology();

    const AizixConfig& config_;

    // 核心组件
    boost::asio::io_context ioc_;
    std::shared_ptr<boost::asio::thread_pool> worker_pool_;
    std::unique_ptr<Server> server_;
    std::shared_ptr<ConnectionManager> connection_manager_;

    // 优雅关闭
    boost::asio::signal_set signals_;

    // 线程管理
    std::vector<std::thread> io_threads_;
    std::vector<std::vector<int>> cpu_topology_;
    std::vector<int> all_cpu_cores_;

    std::shared_ptr<HttpClient> http_client_;
    std::shared_ptr<WebSocketClient> ws_client_;
    std::shared_ptr<UserService> user_service_;
    std::shared_ptr<UserController> user_controller_;
};


#endif //AIZIX_APPLICATION_HPP