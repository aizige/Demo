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

/**
  * @brief 应用程序核心类 (Application Kernel)
  *
  * 负责整个 Web 框架的生命周期管理，包括：
  * 1. 线程模型管理 (IO 线程池、计算线程池、绑核策略)
  * 2. 核心组件初始化 (Server, HttpClient, Router)
  * 3. 信号处理与优雅退出
  *
  * One Loop Per Thread: 就像每辆出租车都有一个司机，各跑各的。
  * Thread Pool (Shared Loop): 就像一个巨大的任务队列，所有工人（线程）都盯着这个队列，谁闲谁抢。
  *
  * @par 线程模型 (One Loop Per Thread)
  * - **Main IO (io-0)**: 负责 Accept 连接、信号处理、全局组件的 Strand 串行化。
  * - **Sub IO (io-1..N)**: 负责 Socket 读写、HTTP 解析、TLS 握手。
  * - **Compute (compute-0..M)**: 负责 gzip 压缩、图片处理等 CPU 密集型任务。
  */
namespace aizix {
    class App {
    public:
        /// @brief 构造函数，加载配置并初始化资源
        explicit App(const std::string& config_path = "config.toml");

        /// @brief 析构函数 (自动 join 所有 jthread)
        ~App();

        // 禁止拷贝和移动 (App 是全局单例性质的资源管理器)
        App(const App&) = delete;
        App& operator=(const App&) = delete;

        // --- 服务注册接口 ---


        /// @brief 注册一组 HTTP 控制器
        /// @param controllers 控制器列表

        void addController(const std::vector<std::shared_ptr<aizix::HttpController>>& controllers);
        void addController(const std::shared_ptr<aizix::HttpController>& controller);

        // --- 核心资源访问 (Getters) ---

        const AizixConfig& config() const { return config_; }

        /// @brief 获取 Server 实例 (用于路由配置等)
        /// @note 此函数不要设为 const (获取server可能要修改)
        // ReSharper disable once CppMemberFunctionMayBeConst
        Server& server() { return *server_; }


        /// @brief [负载均衡] 获取下一个用于处理连接的 IO Context
        ///
        /// Server Acceptor 会在每次接收新连接时调用此方法，采用 Round-Robin 策略
        /// 将 Socket 均匀分配给 io_context_pool_ 中的线程。
        ///
        /// @return boost::asio::io_context& 选中的 IO 上下文
        boost::asio::io_context& get_ioc();


        /// @brief [主控] 获取主 IO Context (io-0)
        ///
        /// 用于绑定全局单例组件（如 Service 请求分发）、监听器（Acceptor）和信号集（Signals）。
        /// 它是 io_context_pool_ 的第 0 个元素。
        boost::asio::io_context& get_main_ioc() const { return *io_context_pool_[0]; }
        const std::vector<std::shared_ptr<boost::asio::io_context>>& get_io_contexts() const { return io_context_pool_; }


        // 3. Worker Pool 获取
        // ReSharper disable once CppMemberFunctionMayBeConst
        // 获取用于 CPU 密集型任务的 Context

        /// @brief [计算] 获取计算线程池的 Context
        /// 用于投递 CPU 密集型任务（如压缩），避免阻塞 IO 线程。
        boost::asio::io_context& get_compute_context() { return compute_ioc_; }
        ///  @brief [计算] 获取计算线程池的 Executor (用于 co_spawn)
        // ReSharper disable once CppMemberFunctionMayBeConst
        boost::asio::any_io_executor worker_pool_executor() { return compute_ioc_.get_executor(); }



        // --- 客户端组件获取 ---
        /// @brief 获取当前线程专属的 HttpClientPool。
        /// @warning 必须在 IO 线程中调用！
        /// @return 当前线程的 Pool 指针。如果不在 IO 线程调用，行为未定义(可能返回空)。
        std::shared_ptr<HttpClientPool> get_local_client_pool();

        /// @brief 供 setup_threading 使用的 helper，用于初始化 thread_local 变量
        void init_thread_local_pool(size_t thread_index);

        std::shared_ptr<IHttpClient> httpClient() { return http_client_; }
        std::shared_ptr<WebSocketClient> webSocketClient() { return ws_client_; }


        /**
         * @brief 启动应用程序主循环
         *
         * 1. 启动所有辅助线程
         * 2. 绑定主线程到 CPU 核心
         * 3. 运行主 IO Context
         * @return 退出码
         */
        int run();

    private:
        // 定义 WorkGuard 类型，用于防止 io_context 在无任务时退出
        using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

        // --- 私有初始化流程 ---
        void setup_threading();       // 创建线程并绑核
        void init_services();         // 初始化 Server 和 Client
        void setup_signal_handling(); // 优雅关机, 监听Ctrl+C

        // 并行停止所有 Client Pool
        boost::asio::awaitable<void> stop_client_pools();


        // --- 静态辅助函数 ---
        static void bind_thread_to_core(size_t core_id);
        static std::vector<std::vector<int>> get_numa_topology();



        // =========================================================
        //  成员变量 (声明顺序决定了初始化和析构顺序，请勿随意调整)
        // =========================================================

        // 1. 全局配置 (必须最先初始化)
        AizixConfig config_;

        // 2. 线程池资源 (IO 和 Compute)

        // --- IO 线程池 (One Loop Per Thread) ---
        // 使用 shared_ptr 管理，避免 vector 扩容时的对象拷贝问题
        std::vector<std::shared_ptr<boost::asio::io_context>> io_context_pool_; // IO 线程池 (One Loop Per Thread)
        std::vector<WorkGuard> io_work_guards_;                                 // IO Work Guards (防止每个 ioc 退出)
        std::atomic<size_t> next_io_context_{0};                                //  轮询索引


        // --- 计算线程池 (Shared Loop Thread Pool) ---
        // 专门处理耗时任务，所有 Compute 线程共享这一个 Context
        boost::asio::io_context compute_ioc_;
        WorkGuard compute_work_guard_; //  Worker 的 Work Guard (防止 worker_ioc_.run() 在没任务时立刻返回)

        // 3. 信号处理 (必须绑定到 main_ioc，使用 unique_ptr 延迟初始化)
        std::unique_ptr<boost::asio::signal_set> signals_;

        // 4. 核心服务组件 (依赖上面的 ioc)
        std::unique_ptr<Server> server_;
        std::vector<std::shared_ptr<HttpClientPool>> http_client_pools_;  // 存储所有 IO 线程对应的 Pool 实例
        std::shared_ptr<HttpClient> http_client_;
        std::shared_ptr<WebSocketClient> ws_client_;

        // 5. 线程句柄 (最后初始化，确保所有资源就绪后再启动线程)
        // 使用 jthread 自动管理生命周期 (自动 join)
        std::vector<std::jthread> io_threads_; // 运行 io_context_pool_[1...N]
        // Worker 线程 (替代 thread_pool 内部线程)
        std::vector<std::jthread> compute_threads_; // 运行 compute_ioc_

        // 6. 硬件拓扑缓存
        std::vector<std::vector<int>> cpu_topology_;
        std::vector<int> all_cpu_cores_;
    };
}
#endif //AIZIX_APPLICATION_HPP
