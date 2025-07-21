//
// Created by Aiziboy on 2025/7/18.
//

#ifndef IHTTPCLIENT_HPP
#define IHTTPCLIENT_HPP



#include <boost/asio/awaitable.hpp>
#include <string_view>

#include "http/http_common_types.hpp"

class IHttpClient {
public:
    virtual ~IHttpClient() = default;

    // 简洁的 GET 方法

    virtual boost::asio::awaitable<HttpResponse> get(std::string_view url, const Headers& headers) = 0;

    // 简洁的 POST 方法

    virtual boost::asio::awaitable<HttpResponse> post(std::string_view url, std::string body, const Headers& headers) = 0;

    // 更通用的 execute 方法，允许用户自定义所有请求细节
    virtual boost::asio::awaitable<HttpResponse> execute(http::verb method, std::string_view url, std::string body, const Headers& headers) = 0;
};


#endif //IHTTPCLIENT_HPP
