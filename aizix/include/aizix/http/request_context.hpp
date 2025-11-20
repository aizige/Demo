#ifndef AIZIX_REQUEST_CONTEXT_HPP
#define AIZIX_REQUEST_CONTEXT_HPP

#include <aizix/http/http_common_types.hpp>
#include <aizix/utils/param_parser.hpp>


class RequestContext {
public:

    RequestContext(HttpRequest req, PathParams params);

    // --- 请求数据访问 ---


    const HttpRequest& request() const;


    std::optional<std::string_view> pathParam(std::string_view key) const;
    std::optional<std::string_view> queryParam(std::string_view key) const;
    std::vector<std::string_view> queryParamList(std::string_view key) const;
    const std::unordered_map<std::string_view, std::vector<std::string_view>>& queryParamAll() const;

    // --- 带类型转换的参数访问 ---

    template <typename T>
    std::optional<T> path_param_as(std::string_view key) const {
        auto sv_opt = pathParam(key);
        if (!sv_opt) {
            return std::nullopt;
        }
        return param_parser::tryParse<T>(*sv_opt);
    }
    template <typename T>
    std::optional<T> query_param_as(std::string_view key) const {
        auto sv_opt = queryParam(key);
        if (!sv_opt) {
            return std::nullopt;
        }
        return param_parser::tryParse<T>(*sv_opt);
    }


    // --- 响应构建 ---
    HttpResponse& response();

    void string(http::status status, std::string_view body, std::string_view content_type = "text/plain; charset=utf-8");
    //void json(http::status status, const nlohmann::json& j);
    void json(http::status status, std::string_view body);

    // --- 压缩响应body ---
    boost::asio::awaitable<void> compressIfAcceptable(const boost::asio::any_io_executor& work_executor);

private:

    void parseQueryIfNeeded() const;

    static void  urlDecode(std::string_view sv, std::string &buffer);


    // --- 成员变量 ---

    HttpRequest request_;   // 持有请求对象的值
    HttpResponse response_; // 持有响应对象的值
    PathParams path_params_; // 持有路径参数的值


    mutable std::string decodeBuffer_; // 所有解码后的 key 和 value 拼接在一起
    mutable std::unordered_map<std::string_view, std::vector<std::string_view>> queryParams_;
    mutable bool queryParamsParsed_ = false;
};

#endif // AIZIX_REQUEST_CONTEXT_HPP