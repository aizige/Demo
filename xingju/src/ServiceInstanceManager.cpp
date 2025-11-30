//
// Created by Aiziboy on 2025/11/23.
//

#include "ServiceInstanceManager.hpp"

#include <map>
#include <ranges>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>

#include "error/xingju_error.hpp"


using namespace std::chrono_literals;

ServiceInstanceManager::ServiceInstanceManager(boost::asio::io_context& ioc)
    : strand_(ioc.get_executor()) {
}

/**
 * @brief 注册服务实例
 *
 * 这是一个 "Sink" 类型的函数，采用按值传递 (Pass-by-value) + std::move 的方式，
 *  参数直接按值传递, 让调用者决定是 Copy 还是 Move，实现零拷贝优化。
 *
 * 逻辑流程:
 * 1. 异步调度到 Strand (保证线程安全)
 * 2. 提取必要的 Key (service_name, instance_id)
 * 3. 原地修改 Instance 状态为 UP
 * 4. Move 进 Map，更新版本号
 *
 * @param instance 服务实例对象 (将被移动)
 */
boost::asio::awaitable<bool> ServiceInstanceManager::registerInstance(aizix::Instance instance) {
    // 将后续的维护工作调度到 strand_ 上，以保证对连接池的线程安全访问
    // mutable: 允许我们在 lambda 内部修改 instance (修改 status)
    auto [e, result] = co_await boost::asio::co_spawn(
        strand_,
        [this, instance = std::move(instance)]() mutable -> boost::asio::awaitable<bool> {
            try {
                // 必须先提取 Key，因为下面 instance 会被 move 进 map
                // 这里的 string 拷贝是必须的开销 (Map 的 Key 和 Value 各需一份数据)
                const std::string service_name = instance.service_name;
                std::string i_id = instance.instance_id;

                // 访问外层 map (如果不存在会自动创建)
                auto& instance_map = registry_[service_name];

                // 准备 Value 对象
                ServiceInstance service_instance;
                service_instance.last_heartbeat = std::chrono::steady_clock::now();

                // 原地修改状态 (得益于 mutable，不需要拷贝副本)
                instance.status = aizix::InstanceStatus::UP;

                // 将 instance 移动进 service_instance
                service_instance.instance = std::move(instance);

                // 将 Key 和 Value 移动进 Map，避免深拷贝
                instance_map[std::move(i_id)] = std::move(service_instance);

                // 列表发生变更，更新版本号
                updateVersion(service_name);
                co_return true;
            } catch (std::exception& exc) {
                throw; // 让异常传递到 co_spawn 的 e 中
            }
        },
        // 告诉 co_spawn 我们希望 co_await 它的结果
        boost::asio::as_tuple(boost::asio::use_awaitable)
    );

    if (e) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            SPDLOG_ERROR("registerInstance failed: {}", ex.what());
            co_return false;
        }
    }

    co_return result;
}

/**
 * @brief 主动注销服务实例
 *
 * @param service_name 服务名称
 * @param instance_id 实例 ID
 */
void ServiceInstanceManager::removeInstance(const std::string& service_name, const std::string& instance_id) {
    // 将后续的维护工作调度到 strand_ 上，以保证对连接池的线程安全访问
    boost::asio::post(strand_, [this, service_name, instance_id]() {
        const auto it_service = registry_.find(service_name);
        if (it_service != registry_.end()) {
            auto& instance_map = it_service->second;

            // 尝试删除。erase 返回被删除元素的个数 (0 或 1)
            // 只有真正删除了数据，才需要更新版本号
            if (instance_map.erase(instance_id) > 0) {
                // 有实例记录被删除了，版本必须更新
                updateVersion(service_name);
            }

            // 如果该服务下没有任何实例了，清理顶层 Map 和版本记录
            if (instance_map.empty()) {
                registry_.erase(it_service);
                service_versions_.erase(service_name);
            }
        }
    });
}

/**
 * @brief 处理服务实例的心跳请求 (Ping)
 *
 * 这是一个复合操作，包含以下职责：
 * 1. **保活 (Heartbeat)**: 更新实例的最后活跃时间，防止被清理。
 * 2. **自愈 (Self-Healing)**: 如果实例之前因超时被标记为 UNHEALTHY，收到 Ping 后自动恢复为 UP 状态，并更新版本号。
 * 3. **数据同步 (Piggyback)**: 比较客户端和服务端的版本号。如果客户端版本落后，顺便返回全量服务列表。
 *
 * @param service_name 服务名称 (如 "user-service")
 * @param instance_id 实例唯一标识 (如 "192.168.1.5:8080")
 * @param client_version 客户端持有的本地服务列表版本号
 *
 * @return boost::asio::awaitable<InstanceArray>
 *         返回包含最新版本号的响应对象。
 *         - 如果 version > client_version：response.instances 包含全量服务列表。
 *         - 如果 version == client_version：response.instances 为空。
 *
 * @throw std::system_error(xingju::error::instance_not_found) 当服务或实例不存在时抛出
 * @note 此函数会在 strand_ 上串行执行，确保对 registry_ 和 version_ 的访问是绝对线程安全的。
 */
boost::asio::awaitable<InstanceArray> ServiceInstanceManager::ping(const std::string& service_name, const std::string& instance_id, const uint64_t client_version) {
    // 使用 co_spawn 将任务派发到 strand 执行，并等待结果
    // 这样可以确保以下所有逻辑都在同一线程上下文中串行执行，无锁竞争
    auto [e, result] = co_await boost::asio::co_spawn(
        strand_,
        [this, service_name, instance_id, client_version]() -> boost::asio::awaitable<InstanceArray> {
            // 1. 查找服务组
            const auto service_it = registry_.find(service_name);
            if (service_it == registry_.end()) {
                // 服务名不存在
                throw std::system_error(xingju::error::instance_not_found);
            }

            // 2. 查找具体实例
            auto& instance_map = service_it->second;
            const auto it = instance_map.find(instance_id);
            if (it == instance_map.end()) {
                // 实例不存在 (可能已过期被移除)
                throw std::system_error(xingju::error::instance_not_found);
            }

            // 3. 执行续约 (更新最后心跳时间)
            it->second.last_heartbeat = std::chrono::steady_clock::now();

            // 4. 执行自愈 (亚健康 -> 健康)
            if (it->second.instance.status != aizix::InstanceStatus::UP) {
                it->second.instance.status = aizix::InstanceStatus::UP;
                updateVersion(service_name); // 状态变更引发版本更新
                SPDLOG_INFO("Instance recovered: {}", instance_id);
            }

            // 5. 构建响应数据
            InstanceArray response{};
            // 始终返回服务端当前的最新版本号，供客户端校对
            response.version = version_;

            // 6. 检查是否需要同步列表 (Piggyback 机制)
            if (version_ > client_version) {
                // 客户端版本落后，构建全量服务列表
                std::map<std::string, std::vector<aizix::Instance>> pairs;

                // 遍历所有服务和实例
                for (auto& [name, map] : registry_) {
                    std::vector<aizix::Instance> serviceInstances;
                    serviceInstances.reserve(map.size());

                    // 使用结构化绑定和 views 优化遍历，只提取 Value (ServiceInstance)
                    for (auto& [instance, last_heartbeat] : map | std::views::values) {
                        serviceInstances.push_back(instance); // 拷贝 Instance 数据
                    }
                    pairs[name] = std::move(serviceInstances);
                }

                // 填充数据载荷
                response.instances = std::move(pairs);
            }
            // else: 版本一致，response.instances 保持为空 (nullopt 或空 map)，节省流量

            co_return response;
        },
        // 使用 as_tuple 捕获可能抛出的异常，而不是直接抛出导致协程崩溃
        boost::asio::as_tuple(boost::asio::use_awaitable)
    );

    // 将 strand 内部捕获的异常重新抛出给调用者 (Controller)
    if (e) {
        std::rethrow_exception(e);
    }
    SPDLOG_DEBUG("ping: {}", boost::json::serialize(boost::json::value_from(result)));
    co_return result;
}

/**
 * @brief 获取指定服务的所有实例 (快照)
 *
 * @return 协程返回实例列表 vector
 */
boost::asio::awaitable<InstanceArray> ServiceInstanceManager::instances(const std::vector<std::string_view>& service_name_array) {
    // 1. 切换到 strand 上下文，确保读取 Map 时没有并发写入
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);

    // 2. 现在是线程安全的，可以读 map
    std::map<std::string, std::vector<aizix::Instance>> result;

    for (auto service_name : service_name_array) {
        if (!service_name.empty()) {
            std::string key(service_name); // 构造一次
            if (const auto it = registry_.find(key); it != registry_.end()) {
                std::vector<aizix::Instance> serviceInstances;
                serviceInstances.reserve(it->second.size());

                // 使用 views 优化遍历，只取 Value
                for (const auto& [instance, last_heartbeat] : it->second | std::views::values) {
                    serviceInstances.push_back(instance); // 这里好像必须得拷贝，不能 move 走
                }
                result[key] = std::move(serviceInstances);
            }
        }
    }
    InstanceArray serviceInstances;
    // 不好整，直接返回全局版本得了，不按服务名称返回版本了 serviceInstances.version = service_versions_.find(service_name)->second;
    serviceInstances.version = version_;
    serviceInstances.instances = std::move(result);

    SPDLOG_DEBUG("获取指定服务实例: {}", boost::json::serialize(boost::json::value_from(serviceInstances)));
    // 返回局部变量，如果不主动 move 也会自动 move
    co_return std::move(serviceInstances);
}

/**
 * @brief 获取全量服务实例 (全量快照)
 *
 * @warning ：此操作开销较大，会拷贝所有数据，打算仅用于网关全量同步。
 */
boost::asio::awaitable<InstanceArray> ServiceInstanceManager::instances() {
    SPDLOG_DEBUG("获取所有服务实例");
    co_await boost::asio::post(strand_, boost::asio::use_awaitable);

    std::map<std::string, std::vector<aizix::Instance>> result;
    for (auto& [name, map] : registry_) {
        std::vector<aizix::Instance> serviceInstances;
        serviceInstances.reserve(map.size());
        for (auto& [instance, last_heartbeat] : map | std::views::values) {
            serviceInstances.push_back(instance);
        }
        result[name] = std::move(serviceInstances);
    }
    InstanceArray serviceInstances;
    serviceInstances.version = version_;
    serviceInstances.instances = std::move(result);

    SPDLOG_DEBUG("获取所有服务实例: {}", boost::json::serialize(boost::json::value_from(serviceInstances)));
    co_return std::move(serviceInstances);
}

/**
 * @brief 启动后台维护任务
 */
void ServiceInstanceManager::run() {
    // 直接将协程 co_spawn 在 strand 上！
    // 这样协程内部的代码默认就在 strand 保护下运行，不需要再手动 co_await post 了
    boost::asio::co_spawn(strand_, healthCheckTask(), boost::asio::detached);
}

/**
 * @brief 后台健康检查协程
 *
 * 周期性扫描所有服务：
 * 1. 超过 check_interval -> 标记 UNHEALTHY
 * 2. 超过 check_critical_timeout -> 移除实例
 */
boost::asio::awaitable<void> ServiceInstanceManager::healthCheckTask() {
    // 在 run() 里指定 strand_, 这里的 executor 就是 strand
    const auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor);

    while (!stopped_) {
        // 设置定时器
        timer.expires_after(MAINTENANCE_INTERVAL_);

        // 这里的 co_await 会挂起协程，释放 strand 锁，允许其他请求(register/ping/get)插入执行
        co_await timer.async_wait(boost::asio::use_awaitable);

        if (stopped_) break;

        // 醒来后, 依然在 strand 上, 直接操作 map, 不需要再 post！
        auto now = std::chrono::steady_clock::now();

        // 遍历所有服务组
        for (auto it_service = registry_.begin(); it_service != registry_.end();) {
            auto serviceName = it_service->first;
            auto& instance_map = it_service->second;
            bool service_changed = false; // 标记该服务是否有变动

            // 遍历该服务下的所有实例
            for (auto it_inst = instance_map.begin(); it_inst != instance_map.end();) {
                auto& [instance, last_heartbeat] = it_inst->second;
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_heartbeat).count();

                // 逻辑 1: 死亡移除 (Kill)
                if (elapsed > instance.check_critical_timeout) {
                    SPDLOG_WARN("Remove dead: {}", instance.instance_id);
                    it_inst = instance_map.erase(it_inst);

                    service_changed = true; // 标记变动
                } else {
                    // 逻辑 2: 亚健康标记 (Mark Unhealthy)
                    if (elapsed > instance.check_interval && instance.status == aizix::InstanceStatus::UP) {
                        instance.status = aizix::InstanceStatus::UNHEALTHY;
                        SPDLOG_WARN("Mark unhealthy: {}", instance.instance_id);

                        service_changed = true; // 状态变更也视为服务变更，通知客户端刷新
                    }
                    ++it_inst;
                }
            }

            // 如果发生了任何变动（移除或标记不健康），统一更新一次版本号
            if (service_changed) {
                updateVersion(serviceName);
            }

            // 清理空的服务组，防止 Map 无限膨胀
            if (instance_map.empty()) {
                it_service = registry_.erase(it_service);
                service_versions_.erase(serviceName); // 同时清理版本记录
            } else {
                ++it_service;
            }
        }
    }
}

/**
 * @brief 更新服务版本号 (递增)
 * 用于实现客户端的长轮询或增量同步逻辑。
 * 处理了 uint64 溢出回绕的情况 (虽然几乎不可能发生)。
 */
void ServiceInstanceManager::updateVersion(const std::string& service_name) {
    // 更新全局版本
    if (version_ >= UINT64_MAX - 1) { version_ = 1; } else { ++version_; }

    // service_versions_[service_name] >= UINT64_MAX - 1 ? service_versions_[service_name] = 1 : service_versions_[service_name] += 1;
    // 更新服务级版本
    auto& sv = service_versions_[service_name];
    if (sv >= UINT64_MAX - 1) { sv = 1; } else { ++sv; }
}
