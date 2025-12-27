//
// Created by Aiziboy on 2025/11/12.
//

#include <filesystem>
#include <iostream>
#include <numa.h>
#include <fmt/ranges.h>
#include <aizix/App.hpp>
#include <aizix/core/client/http_client.hpp>
#include <aizix/core/client/websocket_client.hpp>
#include <aizix/utils/logger_manager.hpp>
#include <aizix/utils/thread_utils.hpp>
#include <aizix/utils/config/ConfigLoader.hpp>
#include <nghttp2/nghttp2.h>


// 线程局部存储：存储当前线程专属的 HttpClientPool 指针
// 默认初始化为空
thread_local std::shared_ptr<HttpClientPool> t_local_http_client_pool = nullptr;


/// =================================================================================
///                          架构设计说明 (Architecture Overview)
/// =================================================================================
/// 本框架采用高性能的 "One Loop Per Thread (io线程)" + Shared Thread Pool (cpu密集计算) 模型，结合 NUMA 亲和性优化。
///
/// One Loop Per Thread: 就像每辆出租车都有一个司机，各跑各的。
/// Shared Thread Pool: 就像一个巨大的任务队列，所有工人（线程）都盯着这个队列，谁闲谁抢。
///
/// 1. IO 线程池 (io_context_pool_)
///    - 包含 N 个独立的 io_context，每个绑定到一个独立的系统线程和 CPU 核心。
///    - io-0 (主线程): 负责 Accept 连接、信号处理、全局组件的 Strand 串行化。
///    - io-1..N (子线程): 负责 Socket 的读写、TLS 握手、HTTP 协议解析。
///    - 连接分配: 新连接通过 Round-Robin 策略分配给某个 IO 线程，终身绑定，无锁竞争，Cache 友好。
///
/// 2. 计算线程池 (compute_ioc_)
///    - 包含 M 个 Worker 线程，共享同一个 io_context (Thread Pool 模式)。
///    - 负责 CPU 密集型任务（如 gzip 压缩、复杂加解密），避免阻塞 IO 线程。
///
/// 3. NUMA 优化
///    - 自动探测硬件拓扑，优先将 IO 线程和计算线程绑定到不同的 NUMA 节点，减少跨节点内存访问延迟。
/// =================================================================================


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
      compute_ioc_(config_.server.worker_threads),       // 初始化 Worker Context
      compute_work_guard_(make_work_guard(compute_ioc_)) // 初始化 Work Guard，锁住 worker_ioc_
{
    // 2. 初始化 IO Context 池 (One Loop Per Thread 核心)
    const size_t io_threads_count = config_.server.io_threads;
    if (io_threads_count == 0) {
        throw std::runtime_error("IO threads count must be > 0");
    }

    io_context_pool_.reserve(io_threads_count);
    io_work_guards_.reserve(io_threads_count);

    // 为每个线程创建一个独立的 io_context
    for (size_t i = 0; i < io_threads_count; ++i) {
        // hint: 1 表示这是一个单线程 loop，asio 可以据此优化
        auto ioc = std::make_shared<boost::asio::io_context>(1);
        io_context_pool_.push_back(ioc);
        io_work_guards_.emplace_back(make_work_guard(*ioc)); // 创建 guard 防止 run() 在无任务时退出
    }

    // 3. 初始化信号集 (必须绑定到主线程 io-0)
    signals_ = std::make_unique<boost::asio::signal_set>(*io_context_pool_[0], SIGINT, SIGTERM);


    // 初始化日志管理器（单例），应用全局的日志级别、格式等配置
    aizix::LoggerManager::init(config_.logging);

    // 初始化核心服务 (依赖上述所有组件)
    init_services();

    // 打印依赖库的版本和当前工作目录
    const nghttp2_info* lib_info = nghttp2_version(0);
    SPDLOG_INFO("📦 libnghttp2 version: {}", lib_info->version_str);
    SPDLOG_INFO("📁 Workdir: {}", std::filesystem::current_path().string());

    #if defined(BOOST_ASIO_HAS_IO_URING)
    SPDLOG_INFO("Asio backend: io_uring");
    #else
    SPDLOG_INFO("Asio backend: epoll (standard)");
    #endif
}

aizix::App::~App() = default;

/**
 * @brief [负载均衡] 获取下一个 IO Context
 * 用于 Server Acceptor 将新连接均匀分发给各个 IO 线程。
 */
boost::asio::io_context& aizix::App::get_ioc() {
    // 使用原子操作实现无锁 Round-Robin
    const size_t index = next_io_context_.fetch_add(1, std::memory_order_relaxed);
    return *io_context_pool_[index % io_context_pool_.size()];
}


/**
 * @brief 获取当前线程专属的 HttpClientPool。
 * @warning 必须在 IO 线程中调用！
 */
std::shared_ptr<HttpClientPool> aizix::App::get_local_client_pool() {
    // 如果你在非 IO 线程调用（如 main 或 compute），这里可能是 nullptr
    // 这是一个必须遵守的规约：只能在 IO 线程发请求
    if (!t_local_http_client_pool) {
        // 可以在这里加个 fallback 或者报错
        // SPDLOG_WARN("Accessing HttpClientPool from non-IO thread!");
        throw std::runtime_error("Accessing HttpClientPool from non-IO thread");
    }
    return t_local_http_client_pool;
}

///  初始化 TLS 变量
void aizix::App::init_thread_local_pool(size_t thread_index) {
    if (thread_index < http_client_pools_.size()) {
        t_local_http_client_pool = http_client_pools_[thread_index];
    }
}


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
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);         // 将集合清空（所有位设为0）
    CPU_SET(core_id, &cpu_set); // 将指定 core_id 对应的 CPU 加入集合

    // 获取当前线程的 pthread 标识
    const pthread_t current_thread = pthread_self();

    // 调用 pthread_setaffinity_np 设置线程的 CPU 亲和性
    // 作用：强制当前线程只能在指定的 core_id 上运行
    // 参数说明：
    //   - current_thread: 当前线程
    //   - sizeof(cpu_set_t): 集合大小
    //   - &cpu_set: CPU 集合指针
    if (pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpu_set) != 0) {
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
 * 此函数负责根据系统 NUMA 拓扑和配置文件中的线程数，合理分配并绑定 I/O 线程和 Compute 线程：
 *
 * 主要步骤：
 * 1. 调用 get_numa_topology() 探测系统的 NUMA 节点和 CPU 分布，并打印调试信息。
 * 2. 将所有 NUMA 节点的 CPU 核心合并到 all_cpu_cores_，作为后续分配的候选集合。
 * 3. 如果无法探测到 CPU 拓扑，则禁用线程亲和性绑定，仅使用操作系统默认调度。
 * 4. 创建额外的 I/O 线程（除主线程外），优先绑定在低编号核心上，用于运行 io_context。
 * 5. 创建 Compute 线程池中的线程，均匀分布在剩余的 CPU 核心上，避免与 I/O 线程冲突。
 * 6. 保留最后一个核心给系统使用，避免所有核心都被占用导致系统调度压力。
 *
 * 设计目的：
 * - “尽力而为”的核心绑定：允许用户配置的线程数多于物理核心数。只有当可用核心充足时，线程才会被绑定。
 * - I/O 线程数量少，固定在前几个核心，保证网络事件响应的低延迟。
 * - Compute 线程数量多，均匀分布在所有剩余核心上，充分利用 CPU 并行能力。
 * - 保留一个核心给系统，避免后台任务与应用线程争抢资源。
 * - NUMA 亲和性：所有被绑定的线程都会同时设置其 NUMA 节点亲和性，以优化内存访问。
 * @note 主线程作为第一个 I/O 线程（io_context_pool_[0]），在 run() 中绑定并运行 io_context。
 */
void aizix::App::setup_threading() {
    // --- 1. 探测硬件拓扑 ---

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

    // --- 2. 创建并启动额外的 IO 线程  (io-1 到 io-N)---

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

            // 在线程启动之初，初始化 TLS
           this->init_thread_local_pool(thread_index);

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
            // 启动 io_context 事件循环，处理网络 I/O 事件，运行该线程独占的 io_context
            io_context_pool_[thread_index]->run();
        });
    }

    // --- 启动 Compute (Worker) 线程 ---
    // 遍历所有需要创建的 Worker 线程，均匀分布在剩余核心上
    for (size_t i = 0; i < worker_threads_count; ++i) {
        compute_threads_.emplace_back([this, i,io_threads_count]() {
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
            compute_ioc_.run(); // 启动 worker_ioc_ 事件循环，处理网络耗时任务，线程将在此阻塞直到 worker_ioc_ 停止
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
    // 注入 App 自身引用，以便 Server 和 Client 获取 IO Context
    server_ = std::make_unique<Server>(*this);

    //  为每个 IO Context 创建一个独立的 HttpClientPool
    http_client_pools_.reserve(io_context_pool_.size());
    for (const auto& ioc : io_context_pool_) {
        // 每个 Pool 绑定到特定的 ioc，且 config 共享
        http_client_pools_.push_back(std::make_shared<HttpClientPool>(*ioc, config_));
    }


    // 依赖注入链
    http_client_ = std::make_shared<HttpClient>(*this);
    ws_client_ = std::make_shared<WebSocketClient>(*this);
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
    signals_->async_wait([this](const boost::system::error_code& error, int signal_number) {
        if (!error) {
            SPDLOG_INFO("Received signal {}, starting graceful shutdown...", signal_number);
            signals_->cancel(); // 防止重复触发

            // 在 Main Loop 上启动停止协程
            co_spawn(get_main_ioc(), [&]() -> boost::asio::awaitable<void> {
                // 1. 关闭 server 入口
                SPDLOG_INFO("Shutting down server sessions...");
                co_await server_->stop();

                // 2. 关闭 Http Client 出口 (并行)
                SPDLOG_INFO("Shutting down client connections...");
                co_await stop_client_pools();

                // 3. 停止计算线程
                SPDLOG_INFO("Stopping compute pool...");
                compute_work_guard_.reset(); // 释放 guard
                compute_ioc_.stop();         // 强制停止

                // 4. 停止所有 IO 线程
                SPDLOG_INFO("Stopping all IO contexts...");
                io_work_guards_.clear(); // 释放所有 guard
                // 停止io_context
                for (const auto& ioc : io_context_pool_) {
                    ioc->stop();
                }
            }, boost::asio::detached);
        }
    });
}

/// 并行停止所有 Client Pools
boost::asio::awaitable<void> aizix::App::stop_client_pools() {
    SPDLOG_INFO("Stopping {} http client pools...", http_client_pools_.size());

    std::vector<boost::asio::awaitable<void>> tasks;
    tasks.reserve(http_client_pools_.size());

    // 遍历所有 Pool，dispatch 到它们各自的 IO 线程去执行 stop
    for (size_t i = 0; i < http_client_pools_.size(); ++i) {
        auto& pool = http_client_pools_[i];
        auto& ioc = *io_context_pool_[i];

        // 必须在 pool 所属的线程执行 stop
        tasks.push_back(boost::asio::co_spawn(ioc, [pool]() -> boost::asio::awaitable<void> {
            co_await pool->stop();
        }, boost::asio::use_awaitable));
    }

    // 等待所有 Pool 停止
    for (auto& task : tasks) {
        co_await std::move(task);
    }
    SPDLOG_INFO("All http client pools stopped.");
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
        setup_threading();

        setup_signal_handling();

        server_->run();

        SPDLOG_INFO("Server started on port {}. I/O threads: {}, Worker threads: {}. Press Ctrl+C to shut down.",
                    config_.server.port, config_.server.io_threads, config_.server.worker_threads);

        // 主线程 name 配置 io-0 作为第一个 IO 线程
        ThreadUtils::set_current_thread_name("io-0");
        // 主线程也要初始化 TLS
        this->init_thread_local_pool(0);

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
        // 运行第 0 个 io_context
        io_context_pool_[0]->run();


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
