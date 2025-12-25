//
// Created by Aiziboy on 2025/11/23.
//

#ifndef XINGJU_SERVER_MANAGER_HPP
#define XINGJU_SERVER_MANAGER_HPP
#include "response/Instance.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <map>
#include <unordered_map>

using namespace std::chrono_literals;

struct InstanceArray {
    uint64_t version = 0;
    std::optional<std::map<std::string, std::vector<aizix::Instance>>> instances;

    // 序列化
    friend void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const InstanceArray& p) {
        boost::json::object obj;
        obj["version"] = p.version;

        if (p.instances.has_value()) {
            obj["instances"] = boost::json::value_from(*p.instances);
        } else {
            obj["instances"] = boost::json::array();
        }

        jv = std::move(obj);
    }

    // 反序列化
    friend InstanceArray tag_invoke(boost::json::value_to_tag<InstanceArray>, const boost::json::value& jv) {
        const auto& obj = jv.as_object();

        InstanceArray p;
        p.version = obj.at("version").as_uint64();

        const auto it = obj.find("instances");
        if (it != obj.end() && it->value().is_array()) {
            p.instances = boost::json::value_to<std::map<std::string, std::vector<aizix::Instance>>>(it->value());
        } else {
            p.instances = std::nullopt;
        }

        return p;
    }
};


struct ServiceInstance {
    aizix::Instance instance;
    // 最后一次心跳时间
    std::chrono::steady_clock::time_point last_heartbeat;
};


class ServiceInstanceManager {
public:
    explicit ServiceInstanceManager(boost::asio::io_context& ioc);


    boost::asio::awaitable<bool> registerInstance(aizix::Instance instance);

    // 注销实例
    void removeInstance(const std::string& service_name, const std::string& instance_id);
    boost::asio::awaitable<InstanceArray>  ping(const std::string& service_name, const std::string& instance_id, uint64_t client_version);

    // 按服务名称获取实例
    boost::asio::awaitable<InstanceArray> instances(const std::vector<std::string_view>& service_name_array);

    // 获取所有实例，网关需要用到此接口
    boost::asio::awaitable<InstanceArray> instances();


    void run();

private:
    // 清理协程：定期清理注册列表里已经失联的服务实例
    boost::asio::awaitable<void> healthCheckTask();

    // 辅助函数：递增版本号
    void updateVersion(const std::string& service_name);

    /// @brief 核心同步原语。所有对内部状态（registry_）
    ///        的读写操作都必须通过 `post` 到这个 strand 上来执行，以保证线程安全。
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;

    /// @brief 标志位，用于在关闭时安全地停止后台循环。
    std::atomic<bool> stopped_ = false;

    const std::chrono::seconds MAINTENANCE_INTERVAL_ = 10s;

    // 外层 Key 是 ServiceName，内层 Key 是 InstanceID
    using InstanceMap = std::unordered_map<std::string, ServiceInstance>;
    using RegistryMap = std::unordered_map<std::string, InstanceMap>;
    RegistryMap registry_; // std::unordered_map<std::string, unordered_map<std::string, ServiceInstance>>

    uint64_t version_ = 0;                                       // 当前实例列表的版本号
    std::unordered_map<std::string, uint64_t> service_versions_; // 具体到对应server实列列表的版本
};


#endif //XINGJU_SERVER_MANAGER_HPP
