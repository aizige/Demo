//
// Created by Aiziboy on 2025/7/19.
//

#ifndef UNTITLED1_ROUTER_HPP
#define UNTITLED1_ROUTER_HPP

#include "handler.hpp"               // 需要 HandlerFunc
#include "http_common_types.hpp"     // 需要 PathParams
#include <memory>
#include <optional>                  // 因为 resolve 返回 optional
#include <string_view>
#include <unordered_map>
#include <vector>


// --- 将 RouteMatch 的定义放在这里 ---
/**
 * @struct RouteMatch
 * @brief 存储路由成功匹配后的结果。
 */
struct RouteMatch {
    HandlerFunc handler; // 匹配到的处理函数
    PathParams path_params; // 匹配到的路径参数
};


// 向前声明 TrieNode 以隐藏实现细节
namespace routing {
    struct TrieNode;
}

class Router {
public:
    Router();
    ~Router(); // 需要析构函数来处理 pimpl 的内存

    // --- 路由注册 API ---
    Router& GET(std::string_view path, HandlerFunc handler);
    Router& POST(std::string_view path, HandlerFunc handler);
    Router& PUT(std::string_view path, HandlerFunc handler);
    Router& DELETE(std::string_view path, HandlerFunc handler);
    // ... 可以添加其他 HTTP 方法


    /**
     * @brief [新] 路由分发的主入口。
     * 这个方法总是返回一个可执行的 RouteMatch 对象。
     * 如果没有找到匹配的路由，返回的 RouteMatch 会包含一个
     * 能生成 404 或 405 错误响应的 handler。
     * @param method 请求的 HTTP 方法。
     * @param target 请求的 URL target。
     * @return 一个保证有效的 RouteMatch 对象。
     */
    RouteMatch dispatch(http::verb method, std::string_view target) const;

private:
    const routing::TrieNode* match(const routing::TrieNode* start_node,
                               std::string_view path,
                               PathParams& out_params) const;
    /**
    * @brief 解析请求，查找匹配的路由。
    * @param method 请求的 HTTP 方法。
    * @param target 请求的 URL target (e.g., "/users/123?q=test")。
    * @return 如果找到匹配，返回包含 handler 和路径参数的 RouteMatch。否则返回 std::nullopt。
    */
    std::optional<RouteMatch> resolve(http::verb method, std::string_view target) const;

    /**
     * @brief 检查给定路径是否存在于任何 HTTP 方法中。
     * 用于处理 405 Method Not Allowed。
     * @param target 请求的 URL target。
     * @return 如果路径存在，返回所有支持该路径的 HTTP 方法列表。
     */
    std::vector<http::verb> find_allowed_methods(std::string_view target) const;

    void add_route(http::verb method, std::string_view path, HandlerFunc handler);
    std::string_view strip_query(std::string_view t) const;

    // 使用 PIMPL (Pointer to Implementation) 模式来隐藏 Trie 树的复杂性
    // routes_ 的 key 是 HTTP 方法，value 是指向该方法对应 Trie 树根节点的 unique_ptr
    std::unordered_map<http::verb, std::unique_ptr<routing::TrieNode>> routes_;
};

#endif //UNTITLED1_ROUTER_HPP
