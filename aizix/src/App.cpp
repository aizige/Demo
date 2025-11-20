//
// Created by Aiziboy on 2025/11/12.
//
#include <filesystem>
#include <iostream>
#include <numa.h>
#include <fmt/ranges.h>   // 关键头文件，提供对 STL 容器的格式化支持

#include <aizix/App.hpp>

#include <aizix/core/client/http_client.hpp>
#include <aizix/core/client/websocket_client.hpp>

#include <aizix/utils/logger_manager.hpp>
#include <aizix/utils/thread_utils.hpp>
#include <aizix/utils/config/ConfigLoader.hpp>


/// 当前架构评
/// 你的分配方式（3 个核心跑 io_context.run()，5 个核心跑 asio::thread_pool，1 核 1 线程、维护连接池与保活）在可用性和吞吐上是合理的，但在低抖动和尾延迟方面有改进空间。两个关键点：线程亲和性与执行器边界。没有亲和性会导致线程在核心间迁移、缓存失效；把关键路径任务 post 到共享 thread_pool 会引入调度不确定性和队列竞争。
///
/// 核心建议与重构方向
/// • 	**绑定与隔离：**为每个 io_context 线程与计算/下单线程设置 CPU 亲和与隔离（避免被系统任务抢占）。在双路（NUMA）机器上，确保线程与其主要内存分配在同一 NUMA 节点。
/// • 	**减少跨执行器迁移：**行情接收与初步解码在同核完成，不再把关键计算任务 post 到共享 thread_pool。改为每策略/市场一个固定线程（或少量固定线程）消费 SPSC 队列，线程绑定核心。
/// • 	**连接池分层：**HTTP/2 与 WebSocket 连接池置于独立“下单线程”或少量下单线程；避免与计算线程共享 thread_pool。下单线程串行（或有限并行度）处理，使用预分配缓冲。
/// • 	**协程纪律：**协程只在同一执行器内 await，避免跨执行器导致线程迁移；关键路径中避免 co_await 通用 thread_pool。
/// • 	**批量与背压：**计算线程批量消费 N 条行情（例如 8/16），下单线程合并或限速提交，必要时丢弃低价值中间帧以稳住尾延迟。
///
/// 具体资源布局建议（8 核示例）
/// • 	**核 0–2：**3 个 io_context 线程（每核一个），负责网络接收与同核解码，写入各自的 SPSC 队列（每会话或每策略一条）。
/// • 	**核 3–6：**4 个策略计算线程（或按策略数设置），每线程消费一个或多个 SPSC 队列，批量处理、生成下单指令，写入“下单队列”（SPSC）。
/// • 	**核 7：**1 个下单线程，维护连接池（HTTP/2、WebSocket、必要时 HTTP/1.1），串行或小并发度发送，处理回执与重试。
/// • 	**后台池：**如确实需要 thread_pool，仅用于日志、快照、风控批处理，不参与行情-计算-下单链路。
/// 如果你的策略数较多，可以将核 6 也分给下单，形成 2 个下单线程（各自绑定核心），将订单按 venue 或账户分片，减少同一连接池的竞争。
///
/// 针对 Http/1.1、Http/2、WebSocket 的下单细化
/// • 	**HTTP/2：**优先使用，单连接多路复用，减少连接管理与队头阻塞；开启 ALPN 与持久连接，预构建请求头与正文缓冲，复用 HPACK 上下文。
/// • 	**HTTP/1.1：**必须时使用 keep-alive，连接池要限制并发度与每连接排队长度；请求模板预分配，尽量避免分配与格式化开销。
/// • 	**WebSocket（wss）：**若交易所对下单/推送支持 WS，尽量使用二进制帧，维护少量持久连接；序列化为扁平（flat）缓冲，避免 JSON 热路径。
/// • 	**TLS：**复用会话、禁用过度证书链检查开销（在允许范围内）、预热握手；为关键连接设置更短的超时与快速失败策略。
/// • 	**超时与幂等：**严格 per-op 超时（毫秒级、甚至更小），请求携带序列号，失败快速重试但限速，避免队列爆炸。
///
/// 操作系统与网络栈调优
/// • 	**IRQ 与 RSS：**将 NIC 队列的中断亲和到对应 io_context 核心，启用 RSS 保证流按队列分片；避免跨核跳转。
/// • 	**套接字选项：**开启 TCP_NODELAY，合理设置 SO_SNDBUF/SO_RCVBUF；对小消息合并发送（应用层批处理），同时控制 Nagle 影响。
/// • 	**内存与分页：**HugePages、锁定关键内存（mlock），避免缺页；选择线程本地分配器（jemalloc/tcmalloc），预热对象池。
/// • 	**时钟与计时：**统一使用单一时间源（例如 TSC 或稳态时钟），避免跨核时间漂移影响度量与策略节奏。
///
/// 🖼 推荐分配方案（8 核）
///  核心编号	     分配角色	               说明
///  Core 0	     系统后台	               预留给操作系统内核线程、中断处理、后台服务
///  Core 1	     io_context #1	       网络 I/O 事件循环，绑定 NIC 队列
///  Core 2	     io_context #2	       网络 I/O 事件循环，绑定 NIC 队列
///  Core 3	     io_context #3	       网络 I/O 事件循环，绑定 NIC 队列
///  Core 4	     策略计算 #1            消费行情队列，执行策略逻辑
///  Core 5	     策略计算 #2            消费行情队列，执行策略逻辑
///  Core 6	     策略计算 #3            消费行情队列，执行策略逻辑
///  Core 7	     下单线程	维护           TTP/2 / WebSocket 连接池，串行或有限并发下单


/**
 * @brief Application 类的构造函数。
 *
 * 负责应用启动的早期初始化工作，包括：
 * 1. 保存配置对象的引用。
 * 2. 创建用于后台计算的 worker 线程池。
 * 3. 设置用于接收终止信号的 signal_set。
 * 4. 初始化日志系统。
 * 5. 打印应用启动时的基本环境信息。
 *
 * @param config_path 从配置文件路径加载配置。
 */
aizix::App::App(const std::string& config_path)
    : config_(ConfigLoader::load(config_path)),
      worker_ioc_(config_.server.worker_threads),                    // 初始化 Worker Context
      worker_work_guard_(boost::asio::make_work_guard(worker_ioc_)), // 初始化 Work Guard，锁住 worker_ioc_
      signals_(ioc_, SIGINT, SIGTERM) {

    // 初始化日志管理器（单例），应用全局的日志级别、格式等配置
    aizix::LoggerManager::init(config_.logging);

    // 初始化核心服务 (依赖上述所有组件)
    init_services();

    // 打印依赖库的版本和当前工作目录
    const nghttp2_info* lib_info = nghttp2_version(0);
    SPDLOG_DEBUG("📦 libnghttp2 version: {}", lib_info->version_str);
    SPDLOG_DEBUG("📁 Workdir: {}", std::filesystem::current_path().string());
}

aizix::App::~App() = default;

// NOLINTNEXTLINE(readability-make-member-function-const)
void aizix::App::addController(const std::vector<std::shared_ptr<aizix::HttpController>>& controllers) {
    //std::vector<std::unique_ptr<IHttpClient>> controllers;
    //controllers.emplace_back(std::make_unique<UserController>());
    //controllers.emplace_back(std::make_unique<AuthController>());
    //controllers.emplace_back(std::make_unique<FileController>());

    for (auto& controller : controllers) {
        controller->registerRoutes(server_->router());
    }
}

// NOLINTNEXTLINE(readability-make-member-function-const)
void aizix::App::addController(const std::shared_ptr<aizix::HttpController>& controller) {
    controller->registerRoutes(server_->router());
}

/**
 * @brief 将当前线程绑定到指定 CPU 核心。
 *
 * 使用 Linux 的 pthread_setaffinity_np 系统调用，将线程固定在某个核心上运行。
 * 这种做法称为“CPU亲和性”（CPU Affinity），可以带来以下好处：
 * - 减少缓存失效：线程总是在同一个CPU上运行，能更好地利用该CPU的L1/L2缓存。
 * - 减少上下文切换开销：避免操作系统在不同核心间频繁调度线程。
 * - 改善NUMA性能：确保线程访问的内存与它所在的CPU在同一个NUMA节点上。
 *
 * @param core_id 要绑定的 CPU 核心编号（从 0 开始）。
 */
void aizix::App::bind_thread_to_core(const size_t core_id) {
    // 定义一个 CPU 集合，用来描述线程可以运行在哪些 CPU 上
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);         // 将集合清空（所有位设为0）
    CPU_SET(core_id, &cpuset); // 将指定 core_id 对应的 CPU 加入集合

    // 获取当前线程的 pthread 标识
    const pthread_t current_thread = pthread_self();

    // 调用 pthread_setaffinity_np 设置线程的 CPU 亲和性
    // 作用：强制当前线程只能在指定的 core_id 上运行
    // 参数说明：
    //   - current_thread: 当前线程
    //   - sizeof(cpu_set_t): 集合大小
    //   - &cpuset: CPU 集合指针
    if (pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset) != 0) {
        // 如果返回值非0，说明设置失败，打印错误信息
        std::cerr << "Error setting thread affinity for core " << core_id << std::endl;
        SPDLOG_ERROR("Error setting thread affinity for core {}", core_id);
    }
}

/**
 * @brief 获取系统的 NUMA (非统一内存访问架构) 拓扑。
 *
 * 在多路CPU服务器上，每个CPU有自己的本地内存，访问本地内存的速度远快于访问
 * 其他CPU的远程内存。此函数探测系统硬件，返回一个描述哪个CPU属于哪个NUMA节点的结构。
 *
 * @return 一个二维向量，`topology[node_id]` 包含了属于该节点的所有CPU核心ID。
 *         如果系统不支持NUMA，则返回空向量。
 */
std::vector<std::vector<int>> aizix::App::get_numa_topology() {
    // 检查系统是否支持 NUMA
    // 如果返回 -1，说明 NUMA 不可用（例如单路 CPU 或未启用 NUMA）
    if (numa_available() == -1) {
        return {}; // 返回空拓扑
    }

    // 获取系统中配置的 NUMA 节点数量
    const int nodes = numa_num_configured_nodes();

    // 获取系统中配置的 CPU 总数
    const int cpus = numa_num_configured_cpus();

    // 定义一个二维数组（vector of vector），每个元素代表一个 NUMA 节点的 CPU 列表
    std::vector<std::vector<int>> topology(nodes);

    // 遍历所有 CPU，查询它属于哪个 NUMA 节点
    for (int cpu = 0; cpu < cpus; ++cpu) {
        // numa_node_of_cpu 返回该 CPU 所属的 NUMA 节点编号
        int node = numa_node_of_cpu(cpu);

        // 如果节点编号合法（>=0 且 < 节点总数），则将该 CPU 加入对应节点的列表
        if (node >= 0 && node < nodes) {
            topology[node].push_back(cpu);
        }
    }

    // 返回完整的 NUMA 拓扑结构
    // 例如：在双路 CPU 系统上，可能返回：
    // topology[0] = {0,1,2,...,15}   // Node0 的 CPU
    // topology[1] = {16,17,...,31}   // Node1 的 CPU
    return topology;
}


/**
 * @brief 初始化线程拓扑与线程绑定策略。
 *
 * 此函数负责根据系统 NUMA 拓扑和配置文件中的线程数，合理分配并绑定 I/O 线程和 Worker 线程：
 *
 * 主要步骤：
 * 1. 调用 get_numa_topology() 探测系统的 NUMA 节点和 CPU 分布，并打印调试信息。
 * 2. 将所有 NUMA 节点的 CPU 核心合并到 all_cpu_cores_，作为后续分配的候选集合。
 * 3. 如果无法探测到 CPU 拓扑，则禁用线程亲和性绑定，仅使用操作系统默认调度。
 * 4. 创建额外的 I/O 线程（除主线程外），优先绑定在低编号核心上，用于运行 io_context。
 * 5. 创建 Worker 线程池中的线程，均匀分布在剩余的 CPU 核心上，避免与 I/O 线程冲突。
 * 6. 保留最后一个核心给系统使用，避免所有核心都被占用导致系统调度压力。
 *
 * 设计目的：
 * - “尽力而为”的核心绑定：允许用户配置的线程数多于物理核心数。只有当可用核心充足时，线程才会被绑定。
 * - I/O 线程数量少，固定在前几个核心，保证网络事件响应的低延迟。
 * - Worker 线程数量多，均匀分布在所有剩余核心上，充分利用 CPU 并行能力。
 * - 保留一个核心给系统，避免后台任务与应用线程争抢资源。
 * - NUMA 亲和性：所有被绑定的线程都会同时设置其 NUMA 节点亲和性，以优化内存访问。
 * @note 主线程作为第一个 I/O 线程（io_worker_main），在 run() 中绑定并运行 io_context。
 */
void aizix::App::setup_threading() {
    // --- 1. 探测系统拓扑结构 ---

    // 调用辅助函数获取系统的 NUMA 拓扑（一个二维数组，外层是节点，内层是该节点上的 CPU 核心 ID）
    cpu_topology_ = get_numa_topology();

    // 打印探测到的 NUMA 节点数量
    SPDLOG_DEBUG("Detected {} NUMA nodes", cpu_topology_.size());

    // 遍历每个 NUMA 节点，打印该节点包含的 CPU 核心编号
    for (size_t n = 0; n < cpu_topology_.size(); ++n) {
        SPDLOG_DEBUG("Node {} CPUs: {}", n, fmt::join(cpu_topology_[n], ", "));
    }

    // 如果探测到的NUMA不为空，将所有节点的 CPU 核心合并到 all_cpu_cores_ 中
    if (!cpu_topology_.empty()) {
        for (const auto& node_cpus : cpu_topology_) {
            all_cpu_cores_.insert(all_cpu_cores_.end(), node_cpus.begin(), node_cpus.end());
        }
    }
    // 如果最终没有探测到任何 CPU 核心，发出警告，
    if (all_cpu_cores_.empty()) {
        SPDLOG_WARN("Could not detect CPU topology. Thread affinity will be disabled.");
        // 如果没有核心信息，则无法进行绑定，直接启动线程即可。
        // (这部分逻辑在后面的线程创建循环中处理)
    }

    // 获取配置中指定的线程数
    const size_t io_threads_count = config_.server.io_threads;
    const size_t worker_threads_count = config_.server.worker_threads;

    // --- 2. 创建并启动额外的 IO 线程 ---

    // 主线程将作为第一个 IO 线程，因此我们只需要创建 (总数 - 1) 个额外的线程。
    // 如果总数只有1，则不创建任何额外线程。
    // IO 线程优先使用低编号的核心
    const size_t num_extra_io_threads = (io_threads_count > 1) ? (io_threads_count - 1) : 0;
    io_threads_.reserve(num_extra_io_threads); // 预分配 vector 容量，避免循环中发生内存重分配

    // 循环创建每一个额外的 IO 线程
    for (size_t i = 0; i < num_extra_io_threads; ++i) {
        // 计算当前线程的全局索引。主线程索引为0，额外线程从1开始。
        size_t thread_index = i + 1;

        // 使用 emplace_back 直接在 vector 中构造线程对象
        io_threads_.emplace_back([this, thread_index]() {
            // 这部分代码将在新创建的线程中执行
            // 设置线程名称
            const std::string thread_name = "io-" + std::to_string(thread_index);
            ThreadUtils::set_current_thread_name(thread_name);

            // 如果探测到 CPU 核心列表不为空，并且当前线程索引没有超出核心列表的范围，则绑定该 I/O 线程到指定 CPU
            if (!all_cpu_cores_.empty() && thread_index < all_cpu_cores_.size()) {
                // 分配 CPU 核心
                int cpu_id = all_cpu_cores_[thread_index];
                // 查询该核心所属的 NUMA 节点
                const int node_id = numa_node_of_cpu(cpu_id);

                // 调用 bind_thread_to_core 将线程绑定到具体 CPU
                bind_thread_to_core(cpu_id);
                if (node_id != -1) {
                    // 设置内存分配策略，NUMA 节点亲和性。这是一个策略性的指令。它告诉操作系统：“请优先在这个 NUMA 节点上为这个线程分配内存，并且线程也应该优先运行在这个节点的所有 CPU 核心上。
                    numa_run_on_node(node_id);
                }
                SPDLOG_INFO("IO thread '{}' bound to CPU {}, Node {}.", thread_name, cpu_id, node_id);
            } else {
                // 如果没有足够的核心，则不进行绑定，让操作系统自由调度
                SPDLOG_INFO("IO thread '{}' started without core affinity.", thread_name);
            }
            // 启动 io_context 事件循环，处理网络 I/O 事件，线程将在此阻塞直到 io_context 停止
            ioc_.run();
        });
    }

    // --- 绑定 Worker 线程 ---
    // 遍历所有需要创建的 Worker 线程，均匀分布在剩余核心上
    for (size_t i = 0; i < worker_threads_count; ++i) {
        worker_threads_.emplace_back([this, i,io_threads_count]() {
            // 设置线程名称，便于调试和日志分析
            const std::string thread_name = "worker-" + std::to_string(i);
            ThreadUtils::set_current_thread_name(thread_name);

            // 计算该 Worker 线程的全局索引。它排在所有 IO 线程之后。
            const size_t cpu_index = io_threads_count + i;

            // 检查是否有可用core，并且计算出的索引没有超出core列表范围
            if (!all_cpu_cores_.empty() && cpu_index < all_cpu_cores_.size()) {
                // 分配 CPU 核心
                int cpu_id = all_cpu_cores_[cpu_index];
                // 查询该核心所属的 NUMA 节点
                int node_id = numa_node_of_cpu(cpu_id);

                // 调用 bind_thread_to_core 将线程绑定到具体 CPU
                bind_thread_to_core(cpu_id);

                if (node_id != -1) {
                    numa_run_on_node(node_id);
                }

                SPDLOG_INFO("Worker thread '{}' bound to CPU {}, Node {}.", thread_name, cpu_id, node_id);
            } else {
                // 如果没有足够的核心，则不进行绑定
                SPDLOG_INFO("Worker thread '{}' started without core affinity.", thread_name);
            }
            worker_ioc_.run(); // 启动 worker_ioc_ 事件循环，处理网络耗时任务，线程将在此阻塞直到 worker_ioc_ 停止
        });
    }
}


/**
 * @brief 初始化服务和路由。
 *
 * 负责创建应用的核心服务对象，并完成依赖注入：
 *
 * 此函数完成了应用的服务层和控制层初始化。
 */
void aizix::App::init_services() {
    server_ = std::make_unique<Server>(ioc_, worker_ioc_.get_executor(), config_);
    http_client_pool_ = std::make_shared<HttpClientPool>(ioc_, config_);

    // 依赖注入链
    http_client_ = std::make_shared<HttpClient>(http_client_pool_);
    ws_client_ = std::make_shared<WebSocketClient>(ioc_, config_.client.ssl_verify);
}

/**
 * @brief 设置信号处理逻辑，实现优雅关闭。
 *
 * 使用 boost::asio::signal_set 监听 SIGINT (Ctrl+C) 和 SIGTERM 信号：
 * 1. 捕获信号后，取消后续信号监听，避免重复触发。
 * 2. 启动一个协程，依次关闭服务端、客户端连接管理器等资源。
 * 3. 在所有清理工作完成后，停止 io_context，结束事件循环。
 *
 * 该函数确保应用在接收到终止信号时能够优雅地关闭，而不是直接退出。
 */
void aizix::App::setup_signal_handling() {
    signals_.async_wait([this](const boost::system::error_code& error, int signal_number) {
        if (!error) {
            SPDLOG_INFO("Received signal {}, starting graceful shutdown...", signal_number);
            signals_.cancel(); // 防止重复触发
            boost::asio::co_spawn(ioc_, [&]() -> boost::asio::awaitable<void> {
                SPDLOG_INFO("Shutting down server sessions...");
                co_await server_->stop();
                SPDLOG_INFO("Shutting down client connections...");
                co_await http_client_pool_->stop();
                SPDLOG_INFO("Stopping worker pool and io_context...");
                worker_work_guard_.reset(); // 释放 guard
                worker_ioc_.stop();         // 强制停止
                SPDLOG_INFO("All services stopped. Stopping io_context...");
                ioc_.stop();
            }, boost::asio::detached);
        }
    });
}

/**
 * @brief 主运行函数。
 *
 * 负责启动整个应用的生命周期：
 * 1. 创建 work_guard，防止 io_context 在没有任务时提前退出。
 * 2. 调用 setup_threading() 初始化线程和 CPU 绑定。
 * 3. 调用 setup_services() 初始化服务和路由。
 * 4. 调用 setup_signal_handling() 设置优雅关闭逻辑。
 * 5. 启动 Server，开始监听端口。
 * 6. 将主线程作为第一个 I/O 线程运行 io_context。
 * 7. 等待所有 I/O 线程和 Worker 线程结束。
 * 8. 在退出时关闭日志系统。
 *
 * @return int 返回 0 表示正常退出，返回 1 表示发生异常。
 */
int aizix::App::run() {
    try {
        auto work_guard = boost::asio::make_work_guard(ioc_.get_executor());

        setup_threading();

        setup_signal_handling();

        server_->run();

        SPDLOG_INFO("Server started on port {}. I/O threads: {}, Worker threads: {}. Press Ctrl+C to shut down.",
                    config_.server.port, config_.server.io_threads, config_.server.worker_threads);

        // 主线程作为第一个 IO 线程
        ThreadUtils::set_current_thread_name("io-0");
        if (!all_cpu_cores_.empty()) {
            // 主线程使用第一个核心 (index 0)
            const int cpu_id = all_cpu_cores_[0];
            const int node_id = numa_node_of_cpu(cpu_id);
            bind_thread_to_core(cpu_id);
            if (node_id != -1) {
                numa_run_on_node(node_id);
            }
            SPDLOG_INFO("Main IO thread 'io-worker-0' bound to CPU {}.", all_cpu_cores_[0]);
        }
        ioc_.run();


        // 等待所有线程结束
        // 已经使用了 std::jthread，手动join可以不要了
        ///  for (auto& t : io_threads_) {
        ///      if (t.joinable()) t.join();
        ///  }
        ///  for (auto& w : worker_threads_) {
        ///      if (w.joinable()) w.join();
        ///  }

        SPDLOG_INFO("Server shut down gracefully.");
       //spdlog::shutdown();
        return 0;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Fatal error during server execution: {}", e.what());
        spdlog::shutdown();
        return 1;
    }
}
