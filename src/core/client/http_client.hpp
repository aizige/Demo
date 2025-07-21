//
// Created by Aiziboy on 2025/7/18.
//
#ifndef HTTP_CLIENT_HPP
#define HTTP_CLIENT_HPP

#include "ihttp_client.hpp"
#include <spdlog/spdlog.h>


#include "connection_manager.hpp"
#include "http/http_common_types.hpp"
#include "utils/decompressor.hpp"

class HttpClient : public IHttpClient {
public:
    explicit HttpClient(boost::asio::io_context& ioc);

    boost::asio::awaitable<HttpResponse> get(std::string_view url,
                                             const Headers& headers = {}) override;

    boost::asio::awaitable<HttpResponse> post(std::string_view url,
                                              std::string body,
                                              const Headers& headers = {}) override;

    boost::asio::awaitable<HttpResponse> execute(http::verb method,
                                                 std::string_view url,
                                                 std::string body = "",
                                                 const Headers& headers = {}) override;

private:
    // 内部辅助函数，用于解析 URL
    // 返回 scheme, host, port, target
    struct ParsedUrl {
        std::string scheme; // 是带:格式的。例如: https:,而非: https
        std::string host;
        uint16_t port;
        std::string target;
    };

    ParsedUrl parse_url(std::string_view url);
    std::string resolve_url(const std::string& base_url, const std::string& location);
    // 统一的内部执行函数
    boost::asio::awaitable<HttpResponse> execute_internal(HttpRequest& request,
                                                          const ParsedUrl& target);
    boost::asio::io_context& ioc_;
    // HttpClient 持有一个 ConnectionManager 的共享指针
    // 使用 shared_ptr 是因为它可能被多个并发的请求共享
    std::shared_ptr<ConnectionManager> manager_;


    //  将解压器作为成员变量
    Decompressor decompressor_{Decompressor::Format::GZIP};


    bool follow_redirects_ = true;
    int max_redirects_ = 20;
};
#endif // HTTP_CLIENT_HPP
