#ifndef REQUEST_CONTEXT_HPP
#define REQUEST_CONTEXT_HPP

#include <nlohmann/json_fwd.hpp>
#include "http_common_types.hpp" //



/**
 * @class RequestContext
 * @brief 封装了单次 HTTP 请求的所有上下文信息，包括请求、响应、路径参数和查询参数。
 *
 * 这是框架的核心上下文对象，被传递给所有的请求处理函数 (Handler)。
 * 它旨在提供一个安全、高效且易于使用的 API，来访问请求数据并构建响应。
 * RequestContext 拥有其管理的所有数据，保证了生命周期的安全。
 */
class RequestContext {
public:
    /**
     * @brief 构造函数，通过移动语义获取请求和路径参数的所有权。
     * @param req 解析完成的 HttpRequest 对象。
     * @param params 从路由匹配中获得的 PathParams。
     */
    RequestContext(HttpRequest req, PathParams params);

    // --- 请求数据访问 ---

    /**
     * @brief 获取对底层 HttpRequest 对象的 const 引用。
     */
    const HttpRequest& request() const;

    /**
     * @brief 获取指定的路径参数。
     * @param key 参数名。
     * @return std::optional<std::string_view>，如果参数存在则包含其值。
     */
    std::optional<std::string_view> path_param(const std::string& key) const;

    /**
     * @brief 获取指定的查询参数。
     * @param key 参数名。
     * @return std::optional<std::string_view>，如果参数存在则包含其值。
     */
    std::optional<std::string_view> query_param(const std::string& key) const;

    /**
     * @brief 获取所有查询参数的只读 map。
     * @return 对查询参数 map 的 const 引用。
     */
    const std::unordered_map<std::string_view, std::string_view>& query_params() const;

    // --- 响应构建 ---

    /**
     * @brief 获取对底层 HttpResponse 对象的引用，以便进行高级操作。
     */
    HttpResponse& response();

    /**
     * @brief 设置一个纯文本或 HTML 响应。
     * @param status HTTP 状态码。
     * @param body 响应体内容。
     * @param content_type 响应内容的 MIME 类型。
     */
    void string(boost::beast::http::status status, std::string_view body, std::string_view content_type = "text/plain");

    /**
     * @brief 设置一个 JSON 响应。
     * @param status HTTP 状态码。
     * @param j nlohmann::json 对象。
     */
    void json(boost::beast::http::status status, const nlohmann::json& j);
    void json(boost::beast::http::status status, std::string_view body);

private:
    /**
     * @brief 内部辅助函数，如果需要，则解析查询字符串。
     * 该函数是 const 的，因为它只修改 mutable 成员。
     */
    void parse_query_if_needed() const;

    // --- 成员变量 ---

    HttpRequest request_;   // 持有请求对象的值
    HttpResponse response_; // 持有响应对象的值
    PathParams path_params_; // 持有路径参数的值

    // 查询参数被设计为懒加载，以优化性能。
    // 使用 mutable 关键字，允许在 const 方法中修改它们。
    mutable std::unordered_map<std::string_view, std::string_view> query_params_;
    mutable bool query_params_parsed_ = false;
};

#endif // REQUEST_CONTEXT_HPP