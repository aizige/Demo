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
  * <h2>高性能架构概览 (v3.0 - Shared-Nothing & NUMA-Aware)</h2>
  * 本框架采用极致性能的 "One Loop Per Thread (IO)" + "Per-NUMA Node Compute Pool" 模型，
  * 结合 NUMA 亲和性优化，实现零锁竞争和最大化数据局部性。
  *
  * <h3>1. 网络 IO 线程池 (io_context_pool_) - One Loop Per Thread</h3>
  * - <b>设计：</b> 包含 N 个独立的 `io_context`，每个绑定到一个独立的系统线程和 CPU 核心。
  * - <b>IO-0 (主线程)：</b> 负责 Accept 连接（通过 SO_REUSEPORT）、信号处理、以及低频的全局协调任务。
  * - <b>IO-1..N (子线程)：</b> 负责 Socket 的读写、TLS 握手、HTTP 协议解析。
  * - <b>优势：</b> 连接通过内核 `SO_REUSEPORT` 或 Round-Robin 分配给某个 IO 线程后，**终身绑定**。
  *   所有操作都在本地线程内完成，实现**无锁竞争，Cache 友好**。
  * - <b>客户端：</b> 每个 IO 线程拥有专属的 `HttpClientPool` (线程本地实例)，实现出站请求的**完全无锁**。
  *
  * <h3>2. CPU 密集型计算线程池 (compute_context_pool_) - Per-NUMA Node</h3>
  * - <b>设计：</b> 包含 M 个独立的 `io_context`，每个 `io_context` 绑定到一个**特定的 NUMA 节点**。
  *   每个 NUMA 节点下的 Worker 线程共享该节点的 `io_context`。
  * - <b>任务投递：</b> IO 线程投递 CPU 密集型任务时，会根据自身所在的 NUMA 节点，
  *   自动选择**同 NUMA 节点**的计算池。
  * - <b>优势：</b> 避免了全局锁竞争和**跨 NUMA 内存访问延迟**。数据处理始终在靠近数据的 CPU 核心和内存中进行。
  *
  * <h3>3. NUMA 亲和性与精确绑核</h3>
  * - <b>自动探测：</b> 自动探测硬件拓扑，获取每个 NUMA 节点包含的 CPU 核心列表。
  * - <b>IO 优先：</b> 优先将 IO 线程绑定到每个 NUMA 节点的**前排核心**。
  * - <b>计算避让：</b> 将计算线程绑定到每个 NUMA 节点的**剩余核心**，避免与 IO 线程争抢 L1/L2 缓存。
  * - <b>资源隔离：</b> 保留一个核心给操作系统使用，避免所有核心都被占用。
  */
namespace aizix {
    class App {
    public:
        /// @brief 构造函数，加载配置并初始化资源
        explicit App(const std::string& config_path = "config.toml");

        /// @brief 析构函数。负责等待所有线程安全退出，并释放资源。
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

        /// @brief [负载均衡] 获取下一个用于处理新连接的 IO Context。
        /// Server Acceptor 在每次接收新连接时，通过 Round-Robin 策略分配 Socket。
        /// @return 选中的 IO Context 引用。
        boost::asio::io_context& get_ioc();

        /// @brief [协调者] 获取主 IO Context (io-0)。
        /// 用于绑定信号集，以及协调停止所有服务等低频全局管理任务。
        /// @warning 避免在此 Context 上执行高频或阻塞性 I/O 和计算任务。
        boost::asio::io_context& get_main_ioc() const { return *io_context_pool_[0]; }

        /// @brief 获取所有 IO Context 的列表。
        /// 主要供 `Server` 类的 `setup_acceptor` 用于为每个 IO 线程创建 `acceptor`。
        const std::vector<std::shared_ptr<boost::asio::io_context>>& get_io_contexts() const { return io_context_pool_; }


        // --- 计算线程池访问 (Per-NUMA Node) ---
        /// @brief [计算] 获取当前线程所属 NUMA 节点的计算 Context
        /// 确保任务投递不出 NUMA 节点，实现数据局部性。
        /// @note 如果在非 IO/Worker 线程调用，可能会返回默认节点(0)的 Context。
        /// @example
        /// boost::asio::post(app.get_local_compute_context(), [data](){ cpu密集计算... });
        boost::asio::io_context& get_local_compute_context() const;

        ///  @brief [计算] 获取当前 NUMA 节点计算池的 Executor (用于 co_spawn)
        boost::asio::any_io_executor worker_pool_executor() const { return get_local_compute_context().get_executor(); }


        // --- 客户端组件获取 ---
        /// @brief 获取当前 IO 线程专属的 `HttpClientPool` 实例。
        /// @warning <b>此函数必须在 IO 线程中调用！</b> 在非 IO 线程（如计算线程或主线程启动阶段）调用将抛出 std::runtime_error 异常。
        /// @return 当前线程的 `HttpClientPool` 指针，无锁获取，性能极高。
        std::shared_ptr<HttpClientPool> get_local_client_pool();

        /// @brief 获取全局单例 `HttpClient` 实例 (它是 `HttpClientPool` 的 Facade)。
        std::shared_ptr<IHttpClient> httpClient() { return http_client_; }

        std::shared_ptr<WebSocketClient> webSocketClient() { return ws_client_; }


        /**
         * @brief 启动应用程序主循环。
         *
         * 1. 初始化线程拓扑并启动所有辅助线程。
         * 2. 设置信号处理逻辑。
         * 3. 启动 Server 监听。
         * 4. 绑定主线程到 CPU 核心并运行其 IO Context。
         * @return 退出码 (0 for success, 1 for error)。
         */
        int run();

    private:
        // 定义 WorkGuard 类型，用于防止 io_context 在无任务时退出
        using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

        // --- 私有初始化流程 ---
        void setup_threading();       // 创建线程并绑核、初始化 TLS
        void init_services();         // 初始化 Server 和 Client Pools
        void setup_signal_handling(); // 配置信号处理器，监听 Ctrl+C 实现优雅关机

        /// @brief 异步停止所有客户端连接池。
        /// 遍历所有 `HttpClientPool` 实例，派发停止任务到其各自的 IO 线程。
        boost::asio::awaitable<void> stop_client_pools();


        /// @brief 辅助函数：初始化当前线程的 `HttpClientPool` thread_local 指针。
        void init_thread_local_pool(size_t thread_index) const;

        /// @brief 辅助函数：初始化当前线程的 `compute_ioc` thread_local 指针。
        void init_thread_local_compute_pool(size_t numa_node_id) const;


        // --- 静态辅助函数 ---
        static void bind_thread_to_core(size_t core_id);
        static std::vector<std::vector<int>> get_numa_topology();


        // =========================================================
        //  成员变量 (声明顺序决定了初始化和析构顺序，请勿随意调整)
        // =========================================================

        // 1. 全局配置 (必须最先初始化，所有组件的配置源)
        AizixConfig config_;

        // 2. 线程池资源 (IO 和 Compute)

        // --- IO 线程池 (One Loop Per Thread) ---
        /// 包含 N 个独立的 io_context，每个代表一个 IO 线程。
        std::vector<std::shared_ptr<boost::asio::io_context>> io_context_pool_;
        /// Work Guards 防止每个 io_context 在无任务时立即退出。
        std::vector<WorkGuard> io_work_guards_;
        /// Round-Robin 调度器索引，用于 Server 分配新连接。
        std::atomic<size_t> next_io_context_{0};


        // --- 计算线程池 (Per-NUMA Node) ---
        // 存储每个 NUMA 节点对应的 io_context。大小为 NUMA 节点数 (非 NUMA 机器为 1)。
        std::vector<std::shared_ptr<boost::asio::io_context>> compute_context_pool_; // 这里的 vector 大小 == NUMA 节点数 (非 NUMA 机器为 1)
        std::vector<WorkGuard> compute_work_guards_;  // 对应每个 compute_ioc 的 WorkGuard，防止在没有任务时退出。

        // 3. 信号处理 (必须绑定到 main_ioc，使用 unique_ptr 延迟初始化)
        std::unique_ptr<boost::asio::signal_set> signals_;

        // 4. 4. 核心服务组件 (依赖上面的 io_context_pool_)
        std::unique_ptr<Server> server_;
        std::vector<std::shared_ptr<HttpClientPool>> http_client_pools_; // 每个 IO 线程一个 HttpClientPool 实例
        std::shared_ptr<HttpClient> http_client_; // 作为 Per-Thread HttpClientPool 的 Facade
        std::shared_ptr<WebSocketClient> ws_client_;

        // 5. 线程句柄 (JThreads 会在 App 析构时自动 join)
        std::vector<std::jthread> io_threads_; // 运行 io_context_pool_[1...N]
        // Worker 线程 (替代 thread_pool 内部线程)
        std::vector<std::jthread> compute_threads_; // 运行 compute_context_pool_[0...M]

        // 6. 硬件拓扑缓存 ( 存储 NUMA 拓扑，vector 的索引是 NUMA 节点 ID，其值为该节点包含的 CPU 核心 ID 列表) (在 setup_threading 中赋值和使用)
        std::vector<std::vector<int>> cpu_topology_;  // 描述每个 NUMA 节点包含的 CPU 核心
        std::vector<int> all_cpu_cores_;   // 扁平化的所有 CPU 核心列表 (辅助分配)
    };
}
#endif //AIZIX_APPLICATION_HPP
