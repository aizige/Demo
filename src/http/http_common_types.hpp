//
// Created by Aiziboy on 2025/7/19.
//

#ifndef UNTITLED1_HTTP_COMMON_TYPES_HPP
#define UNTITLED1_HTTP_COMMON_TYPES_HPP
#include <boost/beast/http.hpp>
#include <unordered_map>
#include <string>
#include <optional>

namespace http = boost::beast::http;

using HttpRequest  = http::request<http::string_body>;


using HttpResponse = http::response<http::string_body>;

using Headers = http::fields;

using PathParams = std::unordered_map<std::string, std::string>;

#endif //UNTITLED1_HTTP_COMMON_TYPES_HPP