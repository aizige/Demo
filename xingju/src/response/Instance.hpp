//
// Created by Aiziboy on 2025/11/23.
//

#ifndef XINGJU_INSTANCE_HPP
#define XINGJU_INSTANCE_HPP
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <boost/json/conversion.hpp>
#include <boost/json/value.hpp>


namespace aizix {

    // 定义服务状态枚举
    enum class InstanceStatus {
        UP,        // 健康，正常服务
        UNHEALTHY, // 亚健康，心跳超时但未达到移除阈值
        // DEAD     // 死亡状态不需要存，直接移除即可
    };


    // 辅助：Status 枚举转字符串
    inline std::string status_to_string(const InstanceStatus s) {
        return s == InstanceStatus::UP ? "UP" : "UNHEALTHY";
    }

    inline InstanceStatus status_from_string(const std::string& s) {
        return s == "UP" ? InstanceStatus::UP : InstanceStatus::UNHEALTHY;
    }


    struct Instance {
        // 状态字段
        InstanceStatus status = InstanceStatus::UNHEALTHY;

        // 服务ID
        std::string instance_id;

        // 服务名称
        std::string service_name;

        // 服务IP((内网IP))
        std::string ip;

        // 服务IP(公网IP)
        std::string public_ip;

        // 服务端口
        uint16_t port = 0;

        // 服务是否开启TLS
        bool tls = false;

        // 健康上报间隔
        uint64_t check_interval = 0;

        // 检查关键超时
        uint64_t check_critical_timeout = 0;

        // --- 服务地理位置信息 ---
        // IDC
        std::string idc;

        // --- Boost.JSON 序列化 ---
        friend void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const Instance& i) {
            jv = {
                {"status", status_to_string(i.status)},
                {"instance_id", i.instance_id},
                {"service_name", i.service_name},
                {"ip", i.ip},
                {"public_ip", i.public_ip},
                {"port", i.port},
                {"tls", i.tls},
                {"check_interval", i.check_interval},
                {"check_critical_timeout", i.check_critical_timeout},
                {"idc", i.idc}
            };
        }

        // --- Boost.JSON 反序列化 ---
        friend Instance tag_invoke(boost::json::value_to_tag<Instance>, const boost::json::value& jv) {
            const boost::json::object& obj = jv.as_object();
            Instance inst;

            auto get_string = [&](const char* key) -> std::string {
                if (const auto it = obj.find(key); it != obj.end() && it->value().is_string())
                    return std::string{it->value().as_string().c_str()};
                return ""; // 缺失或类型不匹配 → 空字符串
            };

            auto get_uint = [&](const char* key) -> uint64_t {
                if (const auto it = obj.find(key); it != obj.end() && it->value().is_int64())
                    return static_cast<uint64_t>(it->value().as_int64());
                return 0; // 缺失 → 0
            };

            auto get_bool = [&](const char* key) -> bool {
                if (const auto it = obj.find(key); it != obj.end() && it->value().is_bool())
                    return it->value().as_bool();
                return false; // 缺失 → false
            };

            // 字段解析
            inst.status = status_from_string(get_string("status"));
            inst.instance_id = get_string("instance_id");
            inst.service_name = get_string("service_name");
            inst.ip = get_string("ip");
            inst.public_ip = get_string("public_ip");
            inst.port = static_cast<uint16_t>(get_uint("port"));
            inst.tls = get_bool("tls");
            inst.check_interval = get_uint("check_interval");
            inst.check_critical_timeout = get_uint("check_critical_timeout");
            inst.idc = get_string("idc");

            return inst;
        }
    };
}

#endif //XINGJU_INSTANCE_HPP
