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
thread_local std::shared_ptr<HttpClientPool> t_local_http_client_pool = nullptr; // 默认初始化为空

// 线程局部变量：当前线程应该使用的本地计算池指针
// 这样 get_local_compute_context 就是 O(1) 的，无锁
thread_local boost::asio::io_context* t_local_compute_ioc = nullptr;

///       架构设计说明 (Architecture Overview)
/// ===========================================================
///
/// 本框架采用高性能的 "One Loop Per Thread" (OLPT) I/O 模型 + "Per-NUMA Node Compute Pool" 计算模型，
/// 并结合了 NUMA 亲和性优化与 Thread-Local 资源管理。
///
/// <h3>1. I/O 线程池 (io_context_pool_) - OLPT 模型</h3>
///    - 包含 N 个独立的 `boost::asio::io_context`，每个绑定到一个独立的系统线程和 CPU 核心。
///    - **io-0 (主线程)**: 负责 `Server` 的连接接受 (Acceptor)、信号处理、及少量全局协调任务。
///    - **io-1..N (子线程)**: 负责 Socket 的读写、TLS 握手、HTTP/2 协议解析、及 **本线程的 HttpClientPool**。
///    - **连接分配**: 新客户端连接通过 Round-Robin 策略分配给某个 IO 线程，终身绑定，无锁竞争，Cache 友好。
///    - **外部请求**: `HttpClient` 在哪个 IO 线程调用，就使用哪个 IO 线程的 `HttpClientPool`，完全去中心化。
///
/// <h3>2. 计算线程池 (compute_context_pool_) - Per-NUMA Node Compute Pool 模型</h3>
///    - 包含 `M` 个 Worker 线程，**按 NUMA 节点分组**，每个组共享一个 `io_context`。
///    - **NUMA 优化核心**: IO 线程投递 CPU 密集型任务时，只会投递给**同一 NUMA 节点**的计算池。
///      这确保了数据处理始终发生在数据所在的 NUMA 节点内存附近，消除跨节点内存访问的延迟和带宽瓶颈。
///    - **职责**: 负责 CPU 密集型任务（如 gzip 压缩、复杂加解密），避免阻塞 IO 线程。
///
/// <h3>3. NUMA 亲和性优化</h3>
///    - 自动探测硬件拓扑，将 IO 线程和计算线程精确绑定到不同的 CPU 核心和 NUMA 节点。
///    - <b>IO 线程：</b> 优先占用每个 NUMA 节点的前几个核心，保证网络事件响应的低延迟。
///    - <b>计算线程：</b> 占用每个 NUMA 节点的剩余核心，避免与 IO 线程的 L1/L2 缓存竞争。
///    - <b>资源隔离：</b> 保留一个核心给操作系统使用，避免所有核心都被占用。
/// =================================================================================


/**
 * @brief Application 类的构造函数。
 *
 * 负责应用启动的早期初始化工作：
 * 1. 探测硬件 NUMA 拓扑。
 * 2. 初始化 Per-NUMA 计算线程池。
 * 3. 初始化 One Loop Per Thread I/O 线程池。
 * 4. 设置用于接收终止信号的 `signal_set`。
 * 5. 初始化日志系统。
 * 6. 初始化核心服务（Server, HttpClientPools）。
 */
aizix::App::App(const std::string& config_path)
    : config_(ConfigLoader::load(config_path)) {
    // 0. 先探测拓扑 (必须最先做，因为后续初始化依赖节点数)
    cpu_topology_ = get_numa_topology();

    // 如果没有探测到 NUMA 节点，或者 numa_available() == -1，
    // 则构造一个虚拟的“单节点”拓扑，以便后续逻辑能兼容运行。
    // 在这种情况下，所有的核心都将被视为属于 Node 0。
    if (cpu_topology_.empty()) {
        cpu_topology_.push_back({}); // Node 0
        SPDLOG_WARN("NUMA not available or detection failed. Operating in non-NUMA compatible mode.");
        // 如果没有 NUMA 信息，无法进行精确绑核，后续分配将退化到简单线性分配。
    }

    size_t num_nodes = std::max(static_cast<size_t>(1), cpu_topology_.size());
    SPDLOG_INFO("Initializing compute pools for {} NUMA nodes.", num_nodes);

    // 1. 为每个 NUMA 节点创建一个独立的 Compute IO Context (Per-NUMA Compute Pool)
    compute_context_pool_.reserve(num_nodes);
    compute_work_guards_.reserve(num_nodes);

    for (size_t i = 0; i < num_nodes; ++i) {
        // 每个节点的并发度 hint：总 Worker 数 / 节点数 (至少为 1)
        int concurrency_hint = std::max(1, static_cast<int>(config_.server.worker_threads) / static_cast<int>(num_nodes));
        auto ioc = std::make_shared<boost::asio::io_context>(concurrency_hint);
        compute_context_pool_.push_back(ioc);
        compute_work_guards_.emplace_back(make_work_guard(*ioc));
    }


    // 2. 初始化 IO Context 池 (One Loop Per Thread IO Pool)
    const size_t io_threads_count = config_.server.io_threads;
    if (io_threads_count == 0) {
        throw std::runtime_error("IO threads count must be > 0");
    }

    io_context_pool_.reserve(io_threads_count);
    io_work_guards_.reserve(io_threads_count);

    // 为每个线程创建一个独立的 io_context
    for (size_t i = 0; i < io_threads_count; ++i) {
        // hint: 1 表示这是一个单线程 loop，Boost.Asio 可以据此优化内部开销。
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
 * @brief [Server 负载均衡] 获取下一个 IO Context。
 *
 * 用于 `Server` 的 `Acceptor` 在每次接收新连接时，采用 Round-Robin 策略
 * 将 Socket 均匀分配给 `io_context_pool_` 中的 IO 线程。
 * @return 选中的 `boost::asio::io_context&` 引用。
 */
boost::asio::io_context& aizix::App::get_ioc() {
    // 使用原子操作实现无锁 Round-Robin
    const size_t index = next_io_context_.fetch_add(1, std::memory_order_relaxed);
    return *io_context_pool_[index % io_context_pool_.size()];
}

/**
 * @brief [计算] 获取当前线程所属 NUMA 节点的计算 Context。
 *
 * @details
 * 任务投递到此 Context，可确保 CPU 密集型计算任务在数据所在的 NUMA 节点执行，
 * 从而实现数据局部性，消除跨 NUMA 内存访问的延迟和带宽瓶颈。
 * @warning 必须在 IO 线程或 Worker 线程中调用，否则可能返回默认 Node 0 的 Context。
 * @return 当前线程所属 NUMA 节点的 `boost::asio::io_context&`。
 */
boost::asio::io_context& aizix::App::get_local_compute_context() const {
    if (t_local_compute_ioc) {
        return *t_local_compute_ioc;
    }
    // Fallback: 如果在非 IO/Worker 线程调用 (如 `main` 线程启动阶段)，
    // 或者 `t_local_compute_ioc` 未初始化 (例如在 `compute_threads` 启动前调用)，
    // 则默认返回第 0 个 Compute Context。
    return *compute_context_pool_[0];
}

/**
 * @brief [客户端] 获取当前 IO 线程专属的 `HttpClientPool` 实例。
 *
 * @warning <b>此函数必须在 IO 线程中调用！</b> 在非 IO 线程（如计算线程或主线程启动阶段）
 *          调用将抛出 `std::runtime_error` 异常。
 * @return 当前 IO 线程的 `HttpClientPool` 共享指针，实现无锁、O(1) 获取。
 */
std::shared_ptr<HttpClientPool> aizix::App::get_local_client_pool() {
    // 如果你在非 IO 线程调用（如 main 或 compute），这里可能是 nullptr
    // 这是一个必须遵守的规约：只能在 IO 线程发请求
    if (!t_local_http_client_pool) {
        throw std::runtime_error("Attempted to access HttpClientPool from a non-IO thread or before TLS initialization");
    }
    return t_local_http_client_pool;
}

/**
 * @brief 辅助函数：初始化当前线程的 `HttpClientPool` thread_local 指针。
 *
 * 在每个 IO 线程启动时被调用，将其对应的 `HttpClientPool` 实例指针存入 TLS。
 * @param thread_index IO 线程的索引 (0 为主线程)。
 */
void aizix::App::init_thread_local_pool(const size_t thread_index) const {
    if (thread_index < http_client_pools_.size()) {
        t_local_http_client_pool = http_client_pools_[thread_index];
    } else {
        // 应该不会发生，除非配置的 IO 线程数和实际创建的 Pool 数不匹配
        SPDLOG_ERROR("Attempted to initialize HttpClientPool TLS for invalid thread_index {}.", thread_index);
        t_local_http_client_pool = nullptr;
    }
}

/**
* @brief 辅助函数：初始化当前线程的 `compute_ioc` thread_local 指针。
*
* 在每个 IO 线程或 Worker 线程启动时被调用，将其所属 NUMA 节点的
* `compute_io_context` 实例指针存入 TLS。
* @param numa_node_id 线程所属的 NUMA 节点 ID。
*/
void aizix::App::init_thread_local_compute_pool(const size_t numa_node_id) const {
    if (numa_node_id < compute_context_pool_.size()) {
        t_local_compute_ioc = compute_context_pool_[numa_node_id].get();
    } else {
        // Fallback: 如果 NUMA ID 无效，退化到 Node 0
        SPDLOG_WARN("Invalid NUMA node ID {}. Using Node 0 compute pool.", numa_node_id);
        t_local_compute_ioc = compute_context_pool_[0].get();
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
 * @details
 * 使用 Linux 的 `pthread_setaffinity_np` 系统调用，将线程固定在某个核心上运行。
 * 这种 CPU 亲和性（CPU Affinity）可以带来：
 * - **减少缓存失效**：线程总在同一个 CPU 上，充分利用 L1/L2 缓存。
 * - **减少上下文切换开销**：避免操作系统在不同核心间频繁调度。
 * - **改善 NUMA 性能**：确保线程访问的内存与它所在的 CPU 在同一个 NUMA 节点上。
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
 * @return 一个二维向量，`topology[node_id]` 包含了属于该节点的所有 CPU 核心 ID。
 *         如果系统不支持 NUMA，则返回空向量。
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
 * @brief [核心] 初始化所有线程（IO 和 Compute）并进行 CPU 亲和性绑定。
 *
 * @details
 * 此函数负责根据 NUMA 拓扑和配置文件中的线程数，合理分配并绑定 I/O 线程和 Compute 线程：
 *
 * <h3>分配策略：</h3>
 * - **IO 线程**：优先占用每个 NUMA 节点的前几个 CPU 核心（独占）。
 * - **Compute 线程**：占用每个 NUMA 节点的剩余 CPU 核心（独占，或在核心不足时复用）。
 *
 * <h3>关键步骤：</h3>
 * 1. <b>预计算核心分配：</b> 在主线程中预先决定每个 IO 和 Compute 线程应绑定到哪个 CPU 核心。
 *    这确保了 IO 线程和 Compute 线程不会争抢相同的物理核心。
 * 2. <b>创建并启动线程：</b>
 *    - 每个线程启动后，首先设置线程名称。
 *    - 进行 CPU 绑核 ( `bind_thread_to_core` ) 和 NUMA 内存策略 (`numa_run_on_node`)。
 *    - <b>初始化 TLS：</b> 将当前线程所属的 `HttpClientPool` 和 `compute_ioc` 指针存入线程局部存储。
 *    - 运行其专属的 `io_context` (IO 线程) 或 `compute_io_context` (Compute 线程)。
 */
void aizix::App::setup_threading() {
    // 获取配置中指定的线程数
    const size_t io_threads_count = config_.server.io_threads;
    const size_t worker_threads_count = config_.server.worker_threads;
    const size_t num_nodes = compute_context_pool_.size(); // NUMA 节点数 (或 1)

    // 1. 打印 NUMA 拓扑信息 (cpu_topology_ 在构造函数已填充)
    SPDLOG_DEBUG("Detected {} NUMA nodes", cpu_topology_.size());
    for (size_t n = 0; n < cpu_topology_.size(); ++n) {
        SPDLOG_DEBUG("Node {} CPUs: {}", n, fmt::join(cpu_topology_[n], ", "));
    }

    // --- 预计算：线程的核心分配方案 ---
    // next_core_idx_in_node[node_id]：追踪每个 NUMA 节点下一个可分配核心的索引
    std::vector<size_t> next_core_idx_in_node(num_nodes, 0);

    // 存储每个 IO 线程和 Compute 线程的最终绑定目标 {core_id, node_id}
    std::vector<std::pair<int, int>> io_thread_assignments(io_threads_count, {-1, -1});
    std::vector<std::pair<int, int>> compute_thread_assignments(worker_threads_count, {-1, -1});

    // --- 1. 分配 IO 线程的核心 (io-0 到 io-N-1) ---
    // 策略：尽量均匀地从不同 NUMA 节点分配核心，优先使用不冲突的。
    if (!cpu_topology_.empty()) {
        // 只有在探测到拓扑时才进行绑核分配
        for (size_t i = 0; i < io_threads_count; ++i) {
            bool assigned = false;
            // 尝试从不同的 NUMA 节点 Round-Robin 分配核心
            for (size_t node_iter = 0; node_iter < num_nodes; ++node_iter) {
                size_t target_node_id = (node_iter + i) % num_nodes; // 轮询 NUMA 节点
                if (next_core_idx_in_node[target_node_id] < cpu_topology_[target_node_id].size()) {
                    int core_id = cpu_topology_[target_node_id][next_core_idx_in_node[target_node_id]];
                    io_thread_assignments[i] = {core_id, (int)target_node_id};
                    next_core_idx_in_node[target_node_id]++; // 标记核心已占用
                    assigned = true;
                    break;
                }
            }
            if (!assigned) {
                SPDLOG_WARN("Not enough physical cores for IO thread {}. It will run without dedicated core.", i);
            }
        }
    } else {
        SPDLOG_WARN("CPU topology not available. IO threads will run without core affinity.");
    }

    // --- 2. 分配 Compute 线程的核心 (worker-0 到 worker-M-1) ---
    // 策略：均匀地将 Worker 线程分配到 NUMA 节点，并从该节点的剩余核心中分配。
    if (!cpu_topology_.empty()) {
        for (size_t i = 0; i < worker_threads_count; ++i) {
            size_t target_node_id = i % num_nodes; // Round-Robin 分配 Worker 到 NUMA 节点
            int assigned_core = -1;

            if (next_core_idx_in_node[target_node_id] < cpu_topology_[target_node_id].size()) {
                // 该 Node 还有空闲核心，独占使用
                assigned_core = cpu_topology_[target_node_id][next_core_idx_in_node[target_node_id]];
                next_core_idx_in_node[target_node_id]++;
            } else {
                // 核心不足 (Over-subscription)：回绕复用该 Node 上的核心。
                // 避免跨 Node 访问，优先在同 Node 内复用。
                if (!cpu_topology_[target_node_id].empty()) {
                    assigned_core = cpu_topology_[target_node_id][i % cpu_topology_[target_node_id].size()];
                    SPDLOG_WARN("Worker thread {} over-subscribed on Node {}. Sharing core {}.", i, target_node_id, assigned_core);
                }
            }
            compute_thread_assignments[i] = {assigned_core, (int)target_node_id};
        }
    } else {
        SPDLOG_WARN("CPU topology not available. Worker threads will run without core affinity.");
    }


    // --- 2. 创建并启动额外的 IO 线程  (io-1 到 io-N)---
    // 主线程 (io-0) 会在 run() 中处理自己的初始化和运行
    const size_t num_extra_io_threads = (io_threads_count > 1) ? (io_threads_count - 1) : 0;
    io_threads_.reserve(num_extra_io_threads); // 预分配 vector 容量，避免循环中发生内存重分配

    // 循环创建每一个额外的 IO 线程
    for (size_t i = 0; i < num_extra_io_threads; ++i) {
        // 计算当前线程的全局索引。主线程索引为0，额外线程从1开始。
        size_t thread_index = i + 1;
        auto assignment = io_thread_assignments[thread_index]; // 获取预分配的核心和节点

        io_threads_.emplace_back([this, thread_index, assignment]() {
            const std::string thread_name = "io-" + std::to_string(thread_index);
            ThreadUtils::set_current_thread_name(thread_name);

            // [TLS] 初始化 HttpClientPool 指针
            this->init_thread_local_pool(thread_index);

            // 绑定 CPU 核心和 NUMA 节点
            if (assignment.first != -1) {
                bind_thread_to_core(assignment.first);
                if (assignment.second != -1 && numa_available() != -1) {
                    numa_run_on_node(assignment.second);
                }
                SPDLOG_INFO("IO thread '{}' bound to CPU {}, Node {}.", thread_name, assignment.first, assignment.second);
            } else {
                SPDLOG_INFO("IO thread '{}' started without core affinity.", thread_name);
            }

            // [TLS] 初始化 Compute Pool 指针
            this->init_thread_local_compute_pool(assignment.second != -1 ? assignment.second : 0); // Fallback to Node 0

            io_context_pool_[thread_index]->run();
        });
    }

    // --- 3. 启动 Compute (Worker) 线程 (Per-NUMA Node + 精确绑核) ---
    compute_threads_.reserve(worker_threads_count);

    for (size_t i = 0; i < worker_threads_count; ++i) {
        auto assignment = compute_thread_assignments[i]; // 获取预分配的核心和节点

        compute_threads_.emplace_back([this, i, assignment]() {
            const std::string thread_name = "worker-" + std::to_string(i); // Worker ID 是其在 config 中的索引
            ThreadUtils::set_current_thread_name(thread_name);

            // 绑定 CPU 核心和 NUMA 节点
            if (assignment.first != -1) {
                bind_thread_to_core(assignment.first);
                if (assignment.second != -1 && numa_available() != -1) {
                    numa_run_on_node(assignment.second);
                }
                SPDLOG_INFO("Worker thread '{}' bound to CPU {}, Node {}.", thread_name, assignment.first, assignment.second);
            } else {
                // Fallback: 如果没有找到核心，只绑内存或什么都不做
                if (assignment.second != -1 && numa_available() != -1) {
                    numa_run_on_node(assignment.second);
                    SPDLOG_INFO("Worker thread '{}' bound to Node {} (no dedicated core).", thread_name, assignment.second);
                } else {
                    SPDLOG_INFO("Worker thread '{}' started without core affinity.", thread_name);
                }
            }

            // [TLS] 初始化 Compute Pool 指针
            this->init_thread_local_compute_pool(assignment.second != -1 ? assignment.second : 0); // Fallback to Node 0

            compute_context_pool_[assignment.second != -1 ? assignment.second : 0]->run(); // 运行该节点专属的 IO Context
        });
    }
}


/**
 * @brief 初始化核心服务和路由。
 * 创建 Server、HttpClientPools 和其他客户端组件。
 */
void aizix::App::init_services() {
    // 注入 App 自身引用，以便 Server 和 Client 获取 IO Context
    server_ = std::make_unique<Server>(*this);

    // 为每个 IO Context 创建一个独立的 HttpClientPool (Per-Thread HttpClientPool)
    http_client_pools_.reserve(io_context_pool_.size());
    http_client_pools_.reserve(io_context_pool_.size());
    for (const auto& ioc : io_context_pool_) {
        http_client_pools_.push_back(std::make_shared<HttpClientPool>(*ioc, config_));
    }


    // HttpClient 和 WebSocketClient 现在通过 App 动态获取当前线程的 Pool
    http_client_ = std::make_shared<HttpClient>(*this);
    ws_client_ = std::make_shared<WebSocketClient>(*this);
}

/**
 * @brief 设置信号处理逻辑，实现优雅关闭。
 * 监听 SIGINT (Ctrl+C) 和 SIGTERM 信号，触发级联关闭流程。
 */
void aizix::App::setup_signal_handling() {
    signals_->async_wait([this](const boost::system::error_code& error, int signal_number) {
        if (!error) {
            SPDLOG_INFO("Received signal {}, starting graceful shutdown...", signal_number);
            signals_->cancel(); // 防止重复触发

            // 在主 IO 线程上启动异步关闭协程
            co_spawn(get_main_ioc(), [&]() -> boost::asio::awaitable<void> {
                // 1. 关闭 Server (停止接受新连接，优雅关闭现有会话)
                SPDLOG_INFO("Shutting down server sessions...");
                co_await server_->stop();

                // 2. 关闭所有 HttpClient Pools (并行关闭所有出站连接)
                SPDLOG_INFO("Shutting down client connections...");
                co_await stop_client_pools();

                // 3. 停止所有计算线程池
                SPDLOG_INFO("Stopping compute pools...");
                compute_work_guards_.clear(); // 释放 WorkGuard，允许 io_context 退出
                // 强制停止所有计算 io_context
                for (const auto& compute_ioc : compute_context_pool_) {
                    compute_ioc->stop();
                }


                // 4. 停止所有 IO 线程池
                SPDLOG_INFO("Stopping all IO contexts...");
                io_work_guards_.clear(); // 释放所有 WorkGuard
                // 强制停止所有 IO io_context
                for (const auto& ioc : io_context_pool_) {
                    ioc->stop();
                }
            }, boost::asio::detached);
        }
    });
}

/**
 * @brief 并行停止所有 `HttpClientPool` 实例。
 * 遍历所有 `HttpClientPool`，并将其 `stop()` 方法调度到其所属的 IO 线程执行。
 */
boost::asio::awaitable<void> aizix::App::stop_client_pools() {
    SPDLOG_INFO("Stopping {} http client pools...", http_client_pools_.size());

    std::vector<boost::asio::awaitable<void>> tasks;
    tasks.reserve(http_client_pools_.size());

    // 遍历所有 Pool，dispatch 到它们各自的 IO 线程去执行 stop
    for (size_t i = 0; i < http_client_pools_.size(); ++i) {
        auto& pool = http_client_pools_[i];
        auto& ioc = *io_context_pool_[i];

        // 必须在 pool 所属的 IO 线程上执行 stop，以保证线程安全。
        tasks.push_back(boost::asio::co_spawn(ioc, [pool]() -> boost::asio::awaitable<void> {
            co_await pool->stop();
        }, boost::asio::use_awaitable));
    }

    // 等待所有 Pool 停止任务完成
    for (auto& task : tasks) {
        co_await std::move(task);
    }
    SPDLOG_INFO("All http client pools stopped.");
}

/**
 * @brief 应用程序主运行函数。
 * 负责启动整个应用的生命周期，包括线程启动、服务运行和优雅退出。
 */
int aizix::App::run() {
    try {
        setup_threading(); // 初始化所有线程和绑核

        setup_signal_handling();  // 设置优雅关机

        server_->run(); // 启动 Server 监听

        SPDLOG_INFO("服务器已在端口 {} 上启动. IO 线程数: {}，工作线程数: {} . 按 Ctrl+C 关闭",
                    config_.server.port, config_.server.io_threads, config_.server.worker_threads);

        // --- 主线程 (io-0) 的初始化和运行 ---

        ThreadUtils::set_current_thread_name("io-0");  // 主线程 name 配置 io-0 作为第一个 IO 线程

        // [TLS] 初始化主线程的 HttpClientPool
        this->init_thread_local_pool(0);

        // 绑定主线程到核心并初始化其计算池 TLS
        int main_node_id = 0; // 默认 Node 0
        if (!cpu_topology_.empty() && !cpu_topology_[0].empty()) {
            const int cpu_id = cpu_topology_[0][0]; // 默认使用 Node 0 的第一个核心
            const int n = numa_node_of_cpu(cpu_id);
            bind_thread_to_core(cpu_id);
            if (n != -1) {
                numa_run_on_node(n);
                main_node_id = n;
            }
            SPDLOG_INFO("Main IO thread 'io-0' bound to CPU {}, Node {}.", cpu_id, main_node_id);
        } else {
            SPDLOG_INFO("Main IO thread 'io-0' started without core affinity.");
        }

        this->init_thread_local_compute_pool(main_node_id);

        // 运行主 IO Context
        io_context_pool_[0]->run();

        // 所有线程 (jthread) 会在 App 析构时自动 join
        SPDLOG_INFO("Server shut down gracefully. Exiting application.");
        return 0;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Fatal error during server execution: {}", e.what());
        spdlog::shutdown(); // 确保日志系统关闭
        return 1;
    }catch (...) {
        SPDLOG_ERROR("Unknown fatal error during server execution.");
        spdlog::shutdown();
        return 1;
    }
}
