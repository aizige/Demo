//
// Created by Aiziboy on 2025/7/18.
//
#ifndef HTTP_CLIENT_HPP
#define HTTP_CLIENT_HPP

#include "ihttp_client.hpp"
#include <spdlog/spdlog.h>

#include <utility>

#include "connection_manager.hpp"
#include "http/http_common_types.hpp"


class HttpClient final : public IHttpClient {
public:
    /**
     * @brief 构造函数。
     * @param manager 一个 ConnectionManager 的共享指针，HttpClient 将使用它来获取和管理连接。
     */
    explicit HttpClient(std::shared_ptr<ConnectionManager> manager);

    boost::asio::awaitable<HttpResponse> get(std::string_view url, const Headers& headers) override;
    boost::asio::awaitable<HttpResponse> post(std::string_view url, const std::string& body, const Headers& headers) override;
    boost::asio::awaitable<HttpResponse> execute(http::verb method, std::string_view url, const std::string& body, const Headers& headers) override;

private:
    using InternalResponse = std::pair<HttpResponse, std::shared_ptr<IConnection>>;
    // 内部辅助函数，用于解析 URL
    // 返回 scheme, host, port, target
    struct ParsedUrl {
        std::string scheme; // 是带:格式的。(例如: https:,而非: https) 以Ada的scheme格式为准
        std::string host;
        uint16_t port;
        std::string target;
    };

    static ParsedUrl parse_url(std::string_view url_strv);
    static std::string resolve_url(const std::string& base_url, const std::string& location);
    // 统一的内部执行函数
    [[nodiscard]] boost::asio::awaitable<InternalResponse> execute_internal(const HttpRequest& request, const ParsedUrl& target) const;


    /// @brief 指向连接管理器的共享指针。
    std::shared_ptr<ConnectionManager> manager_;


    /// @brief 允许的最大重定向次数，防止无限循环。TODO：应来自配置文件
    /// @note  值小于等于0则关闭自动处理 HTTP 重定向 (3xx 状态码)
    int max_redirects_ = 3;
};
#endif // HTTP_CLIENT_HPP
