#ifndef AIZIX_ROUTE_COMPILER_HPP
#define AIZIX_ROUTE_COMPILER_HPP

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>      // 用于表示“可能存在也可能不存在”的值，比返回指针或特殊值更安全、更现代
#include <sstream>       // 用于方便地按分隔符解析字符串

/**
 * @struct RoutePattern
 * @brief 代表一个被“编译”过的路由模式。
 *
 * router.hpp 的内部依赖，负责将路由规则字符串编译成可匹配的模式
 *
 *
 * 这个结构体的作用是将一个字符串形式的路由模式（如 "/user/:id/profile"）
 * 预处理成一种优化的、易于匹配的数据结构。这种“编译”操作只需要在服务器
 * 启动时，注册路由的时候执行一次。之后在每次请求到来时，可以直接使用这个
 * 编译好的结构进行高效的匹配，而无需重复解析模式字符串。
 */
struct RoutePattern {
    // 存储被 '/' 分割后的所有路径段。
    // 例如，对于 "/user/:id/profile"，它会存储 {"user", ":id", "profile"}。
    std::vector<std::string> segments;

    // 与 `segments`一一对应，标记每个路径段是否是一个参数。
    // 例如，对于 {"user", ":id", "profile"}，它会存储 {false, true, false}。
    std::vector<bool> is_param;

    /**
     * @brief 静态工厂方法，用于将一个路由模式字符串编译成一个 RoutePattern 对象。
     * @param pattern 路由模式字符串，例如 "/user/:id"。
     * @return 编译完成的 RoutePattern 对象。
     */
    static RoutePattern compile(const std::string& pattern) {
        RoutePattern out;
        // 使用 istringstream 来方便地按 '/' 分割字符串
        std::istringstream ss(pattern);
        std::string seg;
        while (std::getline(ss, seg, '/')) {
            // 忽略因开头或连续的 '/' 产生的空段
            if (seg.empty()) continue;
            out.segments.push_back(seg);
            // 检查段的第一个字符是否是 ':'，以此判断它是否是一个路径参数
            out.is_param.push_back(seg.front() == ':');
        }
        return out;
    }

    /**
     * @brief 尝试将一个实际的请求路径与这个已编译的路由模式进行匹配。
     * @param path 一个实际的请求路径，例如 "/user/123/profile"。
     * @return 如果匹配成功，返回一个 `std::optional`，其中包含一个 `unordered_map`，
     *         该 map 存储了所有解析出的路径参数（如 `{"id": "123"}`）。
     *         如果匹配失败，返回 `std::nullopt`。
     */
    std::optional<std::unordered_map<std::string, std::string>> match(std::string_view path) const {
        // --- 1. 将实际请求路径也按 '/' 分割 ---
        // 注意：这里为了使用 istringstream，将 string_view 转换为了 string，
        // 在极高性能要求的场景下可以考虑手写分割以避免内存分配。
        std::istringstream ss{std::string(path)};
        std::string seg;
        std::vector<std::string> parts;
        while (std::getline(ss, seg, '/')) {
            if (seg.empty()) continue;
            parts.push_back(seg);
        }

        // --- 2. 进行匹配检查 ---
        // 如果实际路径的段数少于模式的段数，则不可能匹配成功。
        // 注意：这里是 < 而不是 !=，允许了路径后面带有额外的部分，
        // 例如模式 "/user/:id" 可以匹配 "/user/123/anything/else"，
        // 这是一个常见的设计，如果需要严格匹配，应使用 parts.size() != segments.size()。
        if (parts.size() < segments.size()) return std::nullopt;

        std::unordered_map<std::string, std::string> res;
        for (size_t i = 0; i < segments.size(); ++i) {
            if (is_param[i]) {
                // 如果模式段是一个参数，则将实际路径段的值存入结果 map。
                // 键是参数名（需要去掉前缀 ':'），值是实际路径段。
                res[segments[i].substr(1)] = parts[i];
            } else if (segments[i] != parts[i]) {
                // 如果模式段是一个普通字符串，但与实际路径段不匹配，则匹配失败。
                return std::nullopt;
            }
        }
        // 所有段都成功匹配，返回包含路径参数的 map。
        return res;
    }
};

#endif // AIZIX_ROUTE_COMPILER_HPP