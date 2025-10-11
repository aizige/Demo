//
// Created by Aiziboy on 2025/7/19.
//

#include "router.hpp"

#include <iostream>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <vector>
#include <spdlog/spdlog.h>

#include "http_common_types.hpp" // 包含 RouteMatch, HandlerFunc 等
#include "request_context.hpp"

namespace routing {
// Trie 树节点的定义
struct TrieNode {
    // 子节点。key 是路径段 (e.g., "users")
    std::unordered_map<std::string, std::unique_ptr<TrieNode>> children;

    // 参数化子节点 (e.g., for ":id")
    std::unique_ptr<TrieNode> param_child = nullptr;
    std::string param_name; // 存储参数名，如 "id"

    // 如果这个节点是某个路由的终点，则存储其 handler
    HandlerFunc handler = nullptr;
};
} // namespace routing

// --- 构造与析构 ---
Router::Router() = default;
Router::~Router() = default; // 必须要有，因为 unique_ptr 需要知道 TrieNode 的完整定义才能销毁



// --- 路由注册实现 ---
void Router::add_route(http::verb method, std::string_view path, HandlerFunc handler) {

    if (routes_.find(method) == routes_.end()) {
        routes_[method] = std::make_unique<routing::TrieNode>();
    }
    routing::TrieNode* current = routes_.at(method).get();

    std::vector<std::string> parts;
    boost::split(parts, path, boost::is_any_of("/"), boost::token_compress_on);

    for (const auto& part_str : parts) {
        if (part_str.empty()) continue;
        std::string_view part(part_str);
        if (part[0] == ':') { // 参数化路由
            if (!current->param_child) {
                current->param_child = std::make_unique<routing::TrieNode>();
            }
            current->param_name = part.substr(1);
            current = current->param_child.get();
            SPDLOG_DEBUG("注册路由：{}",current->param_name);
        } else { // 静态路由
            auto it = current->children.find(std::string(part));
            if (it == current->children.end()) {
                current->children[std::string(part)] = std::make_unique<routing::TrieNode>();
                SPDLOG_DEBUG("注册路径路由：{}",part);
            }
            current = current->children.at(std::string(part)).get();
        }
    }
    current->handler = std::move(handler);
}

Router& Router::GET(std::string_view path, HandlerFunc handler) {
    SPDLOG_DEBUG("创建路由：{}",path);
    add_route(http::verb::get, strip_query(path), std::move(handler));
    return *this;
}
Router& Router::POST(std::string_view path, HandlerFunc handler) {
    add_route(http::verb::post, strip_query(path), std::move(handler));
    return *this;
}
Router& Router::PUT(std::string_view path, HandlerFunc handler) {
    add_route(http::verb::put, strip_query(path), std::move(handler));
    return *this;
}
Router& Router::DELETE(std::string_view path, HandlerFunc handler) {
    add_route(http::verb::delete_, strip_query(path), std::move(handler));
    return *this;
}

// --- 路由解析实现 ---

// 新增 match 函数的实现
// in router.cpp

const routing::TrieNode* Router::match(const routing::TrieNode* start_node,
                                       std::string_view path,
                                       PathParams& out_params) const
{
    const routing::TrieNode* current = start_node;
    if (!current) return nullptr;

    std::vector<std::string> parts;
    boost::split(parts, path, boost::is_any_of("/"), boost::token_compress_on);

    for (const auto& part_str : parts) {
        if (part_str.empty()) continue;

        auto it = current->children.find(part_str);
        if (it != current->children.end()) {
            current = it->second.get();
            continue;
        }

        if (current->param_child) {
            // --- *** 关键修复点 *** ---
            // 1. 使用【当前节点】(父节点) 的 param_name 作为 key
            // 2. part_str (如 "123") 作为 value
            out_params[current->param_name] = part_str;

            // 3. 然后再将 current 移动到【下一个节点】
            current = current->param_child.get();
            continue;
        }

        return nullptr;
    }
    return current;
}

RouteMatch Router::dispatch(http::verb method, std::string_view target) const {
    if (auto match_opt = resolve(method, target)) {
        return std::move(*match_opt);
    }

    std::vector<http::verb> allowed_methods = find_allowed_methods(target);

    if (allowed_methods.empty()) {
        return {
            [](RequestContext& ctx) -> boost::asio::awaitable<void> {
                ctx.string(http::status::not_found, "404 Not Found");
                co_return;
            }, {} };
    } else {
        return {
            [methods = std::move(allowed_methods)](RequestContext& ctx) -> boost::asio::awaitable<void> {
                ctx.string(http::status::method_not_allowed, "405 Method Not Allowed");
                std::string allow_str;
                for (size_t i = 0; i < methods.size(); ++i) {
                    allow_str += to_string(methods[i]);
                    if (i < methods.size() - 1) allow_str += ", ";
                }
                ctx.response().set(http::field::allow, allow_str);
                co_return;
            }, {} };
    }
}


std::optional<RouteMatch> Router::resolve(http::verb method, std::string_view target) const {
    auto method_it = routes_.find(method);
    if (method_it == routes_.end()) {
        return std::nullopt;
    }
    PathParams params;
    const routing::TrieNode* node = match(method_it->second.get(), strip_query(target), params);
    if (node && node->handler) {
        return RouteMatch{node->handler, std::move(params)};
    }
    return std::nullopt;
}

std::vector<http::verb> Router::find_allowed_methods(std::string_view target) const {
    std::vector<http::verb> allowed;
    std::string_view path = strip_query(target);
    for (const auto& [method, trie_root] : routes_) {
        PathParams dummy_params;
        const routing::TrieNode* node = match(trie_root.get(), path, dummy_params);
        if (node && node->handler) {
            allowed.push_back(method);
        }
    }
    return allowed;
}

std::string_view Router::strip_query(std::string_view t) const {
    auto p = t.find('?');
    return p == std::string_view::npos ? t : t.substr(0, p);
}




