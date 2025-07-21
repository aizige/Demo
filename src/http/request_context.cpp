#include "request_context.hpp"
#include <nlohmann/json.hpp> // 在 .cpp 文件中包含完整的 json.hpp

// --- 构造函数实现 ---

RequestContext::RequestContext(HttpRequest req, PathParams params)
    : request_(std::move(req)),
      path_params_(std::move(params))
{
    // 构造时不做任何事情，查询参数的解析被推迟到第一次访问时
}

// --- 请求数据访问实现 ---

const HttpRequest& RequestContext::request() const {
    return request_;
}

std::optional<std::string_view> RequestContext::path_param(const std::string& key) const {
    if (auto it = path_params_.find(key); it != path_params_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<std::string_view> RequestContext::query_param(const std::string& key) const {
    parse_query_if_needed(); // 确保已解析
    if (auto it = query_params_.find(key); it != query_params_.end()) {
        return it->second;
    }
    return std::nullopt;
}

const std::unordered_map<std::string_view, std::string_view>& RequestContext::query_params() const {
    parse_query_if_needed(); // 确保已解析
    return query_params_;
}

// --- 响应构建实现 ---

HttpResponse& RequestContext::response() {
    return response_;
}

void RequestContext::string(http::status status, std::string_view body, std::string_view content_type) {
    response_.result(status);
    response_.set(http::field::content_type, content_type);
    response_.set(http::field::server, "Aiziboyserver/1.0");
    response_.body() = body;
    response_.prepare_payload();
}

void RequestContext::json(http::status status, const nlohmann::json& j) {
    response_.result(status);
    response_.set(http::field::content_type, "application/json");
    response_.set(http::field::server, "Aiziboyserver/1.0");
    response_.body() = j.dump(); // nlohmann::json::dump() 返回 std::string
    response_.prepare_payload();
}

void RequestContext::json(http::status status, std::string_view json) {
    response_.result(status);
    response_.set(http::field::content_type, "application/json");
    response_.set(http::field::server, "Aiziboyserver/1.0");
    response_.body() = json;
    response_.prepare_payload();
}

// --- 私有辅助函数实现 ---

void RequestContext::parse_query_if_needed() const {
    if (query_params_parsed_) {
        return;
    }

    std::string_view target = request_.target();
    auto pos = target.find('?');
    if (pos != std::string_view::npos) {
        std::string_view q = target.substr(pos + 1);
        size_t start = 0;
        while (start < q.size()) {
            size_t end = q.find('&', start);
            if (end == std::string_view::npos) {
                end = q.size();
            }

            auto kv = q.substr(start, end - start);
            auto eq = kv.find('=');

            if (eq != std::string_view::npos) {
                // key 和 value 都是 request_.target() 的视图，无拷贝
                query_params_[kv.substr(0, eq)] = kv.substr(eq + 1);
            } else {
                // 处理没有值的参数，如 /path?flag
                query_params_[kv] = ""; // 值是一个空的 string_view
            }

            start = end + 1;
        }
    }

    query_params_parsed_ = true;
}