//
// Created by Aiziboy on 2025/7/19.
//

#ifndef UNTITLED1_HTTP_COMMON_TYPES_HPP
#define UNTITLED1_HTTP_COMMON_TYPES_HPP
#include <boost/beast/http.hpp>
#include <unordered_map>
#include <string>
#include <optional>

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

using HttpRequest  = http::request<http::string_body>;


using HttpResponse = http::response<http::string_body>;

using Headers = http::fields;

//using PathParams = std::unordered_map<std::string, std::string>;
using PathParams = std::unordered_map<std::string, std::string, StringHash, StringEqual>;

#endif //UNTITLED1_HTTP_COMMON_TYPES_HPP