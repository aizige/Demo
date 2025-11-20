//
// Created by Aiziboy on 2025/7/19.
//

#ifndef AIZIX_HTTP_COMMON_TYPES_HPP
#define AIZIX_HTTP_COMMON_TYPES_HPP
#include <boost/beast/http.hpp>
#include <unordered_map>
#include <string>
#include <optional>
#include <boost/asio/ip/tcp.hpp>
#include <string_view> // <<-- 包含 string_view
#include <boost/asio/awaitable.hpp>


class RequestContext;

// 1. 定义一个透明的 Hasher
struct StringHash {
    using is_transparent = void; // 关键：启用透明性

    size_t operator()(std::string_view sv) const {
        return std::hash<std::string_view>{}(sv);
    }
};

// 2. 定义一个透明的 KeyEqual
struct StringEqual {
    using is_transparent = void; // 关键：启用透明性

    bool operator()(std::string_view a, std::string_view b) const {
        return a == b;
    }
};

namespace http = boost::beast::http;

using tcp = boost::asio::ip::tcp;




using HttpRequest  = http::request<http::string_body>;
using HttpResponse = http::response<http::string_body>;

using Headers = http::fields;

/**
 * @brief 定义了所有请求处理函数的统一函数签名。
 *
 * 这是一个 std::function 对象，它可以包装任何可调用对象
 * （如 lambda、函数指针、成员函数指针），只要其签名匹配：
 * - 接收一个对 RequestContext 的引用。
 * - 返回一个 boost::asio::awaitable<void>，表示它是一个协程。
 */
using HandlerFunc = std::function<boost::asio::awaitable<void>(RequestContext&)>;


using PathParams = std::unordered_map<std::string, std::string, StringHash, StringEqual>;

#endif //AIZIX_HTTP_COMMON_TYPES_HPP