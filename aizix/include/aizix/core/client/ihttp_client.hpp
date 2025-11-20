//
// Created by Aiziboy on 2025/7/18.
//
#ifndef AIZIX_IHTTP_CLIENT_HPP
#define AIZIX_IHTTP_CLIENT_HPP

#include <string>
#include <string_view>
#include <boost/asio/awaitable.hpp>

#include <aizix/http/http_common_types.hpp>

class IHttpClient {
public:
    virtual ~IHttpClient() = default;

    // --- 使用重载代替默认参数 ---

    // 1. 提供简洁的、常用的重载版本
    //    这些可以是非虚的，因为它们的实现是固定的
    boost::asio::awaitable<HttpResponse> get(const std::string_view url) {
        // 委托给带有 Headers 的版本
        return get(url, {}); 
    }

    boost::asio::awaitable<HttpResponse> post(const std::string_view url, const std::string& body) {
        // 委托给带有 Headers 的版本
        return post(url, body, {});
    }

    // 2. 将带有所有参数的版本作为核心虚函数接口
    virtual boost::asio::awaitable<HttpResponse> get(std::string_view url, const Headers& headers) = 0;
    virtual boost::asio::awaitable<HttpResponse> post(std::string_view url, const std::string& body, const Headers& headers) = 0;

    // 3. 对于 execute，也提供多个重载
    //    最核心的、带所有参数的版本是纯虚的
    virtual boost::asio::awaitable<HttpResponse> execute(http::verb method, std::string_view url, const std::string& body, const Headers& headers) = 0;

    //    提供一个不带 body 和 headers 的便捷版本
    boost::asio::awaitable<HttpResponse> execute(const http::verb method, const std::string_view url) {
        return execute(method, url, "", {});
    }

    //    提供一个只带 body 的便捷版本
    boost::asio::awaitable<HttpResponse> execute(const http::verb method, const std::string_view url, const std::string& body) {
        return execute(method, url, body, {});
    }
};

#endif // AIZIX_IHTTP_CLIENT_HPP