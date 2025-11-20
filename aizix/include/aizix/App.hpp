//
// Created by Aiziboy on 2025/11/12.
//

#ifndef AIZIX_APPLICATION_HPP
#define AIZIX_APPLICATION_HPP
#include <boost/asio/io_context.hpp>


#include <aizix/core/Server.hpp>
#include <aizix/core/client/HttpClientPool.hpp>
#include <aizix/core/client/http_client.hpp>
#include <aizix/utils/config/AizixConfig.hpp>

#include <aizix/controller/HttpController.hpp>


class WebSocketClient;
namespace aizix {
    class App {
    public:
        explicit App(const std::string& config_path = "config.toml");

        // 析构函数默认即可，因为使用 jthread 自动管理线程生命周期
        ~App() ;

        // 禁止拷贝和移动 (App 是单例性质的)
        App(const App&) = delete;
        App& operator=(const App&) = delete;

        // 添加控制器
        void addController(const std::vector<std::shared_ptr<aizix::HttpController>>& controllers) ;
        void addController(const std::shared_ptr<aizix::HttpController>& controller) ;

        // --- Getters ---

        // 1. Server 获取
        // 去掉 const，因为获取 Server 通常是为了修改它（比如注册路由）
        // ReSharper disable once CppMemberFunctionMayBeConst
        Server& server() { return *server_; }

        // 2. IO Context 获取
        boost::asio::io_context& io_context() { return ioc_; }
        boost::asio::any_io_executor io_executor() { return ioc_.get_executor(); }

        // 3. Worker Pool 获取
        // ReSharper disable once CppMemberFunctionMayBeConst
        boost::asio::io_context& worker_context() { return worker_ioc_; }
        // ReSharper disable once CppMemberFunctionMayBeConst
        boost::asio::any_io_executor worker_pool_executor() { return worker_ioc_.get_executor(); }

        // 4. Clients 获取
        std::shared_ptr<IHttpClient> httpClient() { return http_client_; }
        std::shared_ptr<WebSocketClient> webSocketClient() { return ws_client_; }

        // 运行应用的主入口
        int run();


    private:
        void setup_threading();
        void init_services();
        void setup_signal_handling();

        // 辅助函数
        static void bind_thread_to_core(size_t core_id);
        static std::vector<std::vector<int>> get_numa_topology();

        // --- 成员变量 (注意初始化顺序) ---

        // 1. 配置 (最先初始化)
        AizixConfig config_;

        // 2. IO和Worker线程池
        boost::asio::io_context ioc_;
        boost::asio::io_context worker_ioc_;   // Worker thread(业务线程池) 使用boost::asio::io_context (替代 thread_pool)

        // 3. Worker 的 Work Guard (防止 worker_ioc_.run() 在没任务时立刻返回)
        using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
        WorkGuard worker_work_guard_;

        // 4. 信号集 (依赖 ioc_)
        boost::asio::signal_set signals_;


        // 5. 核心服务组件 (依赖 ioc_)
        std::unique_ptr<Server> server_;
        std::shared_ptr<HttpClientPool> http_client_pool_;
        std::shared_ptr<HttpClient> http_client_;
        std::shared_ptr<WebSocketClient> ws_client_;


        // 6. 线程管理
        // 使用 std::jthread 替代 std::thread
        // 即使发生异常，jthread 析构时也会自动 join，防止 std::terminate 崩溃
        std::vector<std::jthread> io_threads_;
        // Worker 线程 (替代 thread_pool 内部线程)
        std::vector<std::jthread> worker_threads_;

        std::vector<std::vector<int>> cpu_topology_;
        std::vector<int> all_cpu_cores_;
    };
}
#endif //AIZIX_APPLICATION_HPP