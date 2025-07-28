#include "request_context.hpp"
#include <nlohmann/json.hpp> // 在 .cpp 文件中包含完整的 json.hpp

#include "utils/decompression_manager.hpp"

// --- 构造函数实现 ---

RequestContext::RequestContext(HttpRequest req, PathParams params)
    : request_(std::move(req)),
      path_params_(std::move(params)) {
    // 构造时不做任何事情，查询参数的解析被推迟到第一次访问时
}

// --- 请求数据访问实现 ---

const HttpRequest &RequestContext::request() const {
    return request_;
}

std::optional<std::string_view> RequestContext::path_param(const std::string &key) const {
    if (auto it = path_params_.find(key); it != path_params_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<std::string_view> RequestContext::query_param(const std::string &key) const {
    parse_query_if_needed(); // 确保已解析
    if (auto it = query_params_.find(key); it != query_params_.end()) {
        return it->second;
    }
    return std::nullopt;
}

const std::unordered_map<std::string_view, std::string_view> &RequestContext::query_params() const {
    parse_query_if_needed(); // 确保已解析
    return query_params_;
}

// --- 响应构建实现 ---

HttpResponse &RequestContext::response() {
    return response_;
}

void RequestContext::string(http::status status, std::string_view body, std::string_view content_type) {
    response_.result(status);
    response_.set(http::field::content_type, content_type);
    response_.set(http::field::server, "Aiziboyserver/1.0");
    response_.body() = body;
    compress_if_acceptable();
    response_.prepare_payload();
}

void RequestContext::json(http::status status, const nlohmann::json &j) {
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

/**
 * @brief 如果客户端支持，则对响应体进行压缩。
 *
 * 这个方法会检查请求中的 "Accept-Encoding" 头。如果客户端支持
 * gzip 或 deflate，并且响应体足够大，它会自动压缩响应体，
 * 并设置相应的 "Content-Encoding" 头。
 *
 *
 * 高度可压缩的数据 (强烈建议压缩):
 *
 * 文本: HTML, CSS, JavaScript, JSON, XML, CSV, 日志文件。这些文件包含大量重复的单词、标签和结构，压缩率极高。一个 1MB 的 JSON 文件压缩后可能只有 100-200KB。
 * 对于这类数据，即使只有几百字节，压缩也往往是划算的。
 * 低度可压缩或已压缩的数据 (绝对不要压缩):
 *
 * 图片: JPEG, PNG, GIF, WebP。这些格式本身就是高度压缩的。再次对它们进行 GZIP 压缩，不仅几乎没有效果，有时甚至会因为 GZIP 的头部开销而使文件变大，纯属浪费 CPU。
 * 视频/音频: MP4, MP3, AAC。同理，这些也是压缩格式。
 * 二进制文件: .zip, .gz, .pdf, .docx 等。这些文件格式内部已经包含了压缩。
 */
void RequestContext::compress_if_acceptable() {
    // 1. 检查客户端是否支持压缩
    std::string client_accepts_gzip;
    if (request_.count(http::field::accept_encoding)) {
        std::string_view value = request_.at(http::field::accept_encoding);
        if (value.find("gzip") != std::string_view::npos) {
            client_accepts_gzip = "gzip";
        } else if (value.find("deflate") != std::string_view::npos) {
            client_accepts_gzip = "deflate";
        } else {
            return; // 客户端不支持，直接返回
        }
    }

    // [新] 检查 Content-Type
    if (response_.count(http::field::content_type)) {
        std::string_view content_type = response_.at(http::field::content_type);
        // 只压缩已知的文本类型
        if (!(content_type.starts_with("text/") ||
              content_type.find("json") != std::string_view::npos ||
              content_type.find("xml") != std::string_view::npos ||
              content_type.find("javascript") != std::string_view::npos)) {
            return; // 不是可压缩的类型，直接返回
        }
    }

    // 2. 检查响应体是否值得压缩
    constexpr size_t MIN_COMPRESSION_SIZE = 1024; // 1KB
    if (response_.body().size() < MIN_COMPRESSION_SIZE) {
        return; // body 太小，不值得压缩
    }

    // 3. 检查响应是否已经被压缩过了（防止重复压缩）
    if (response_.count(http::field::content_encoding)) {
        return;
    }

    // 4. 执行压缩
    // 注意：这里我们直接修改内部的 response_ 对象

    std::string compressed_body = client_accepts_gzip == "gzip"?utils::compression::DecompressionManager::gzip_compress(response_.body()) : utils::compression::DecompressionManager::deflate_compress(response_.body());

    // 5. 更新响应对象
    response_.body() = std::move(compressed_body);
    response_.set(http::field::content_encoding, client_accepts_gzip);

    // 6. prepare_payload() 会在 dispatch 中被调用，它会更新 Content-Length
}
