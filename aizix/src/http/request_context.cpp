#include <aizix/http/request_context.hpp>
#include <aizix/version.hpp>
#include <aizix/utils/compression_manager.hpp>
#include <aizix/utils/param_parser.hpp>

#include <iostream>
#include <spdlog/spdlog.h>
#include <boost/url.hpp>
#include <cctype>
#include <cstdlib>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>



// --- 构造函数实现 ---

RequestContext::RequestContext(HttpRequest req, PathParams params)
    : request_(std::move(req)),
      path_params_(std::move(params)) {
    // 在构造时，可以为 response 对象设置一些通用的默认值
    // response_.version(request_.version());
    response_.keep_alive(request_.keep_alive());
    response_.set(http::field::server, aizix::framework::name + "/" + aizix::framework::version);
}

// --- 请求数据访问实现 ---

const HttpRequest& RequestContext::request() const {
    return request_;
}

std::optional<std::string_view> RequestContext::pathParam(std::string_view key) const {
    if (auto it = path_params_.find(key); it != path_params_.end()) {
        return it->second;
    }
    return std::nullopt;
}


std::optional<std::string_view> RequestContext::queryParam(std::string_view key) const {
    parseQueryIfNeeded(); // 确保已解析
    auto it = queryParams_.find(key);
    if (it != queryParams_.end() && !it->second.empty()) {
        return it->second.front(); // 返回第一个值
    }
    return std::nullopt;
}


std::vector<std::string_view> RequestContext::queryParamList(std::string_view key) const {
    parseQueryIfNeeded(); // 确保已解析

    auto it = queryParams_.find(key);
    if (it != queryParams_.end()) {
        return it->second; // 返回所有值
    }
    return {}; // 空 vector
}

const std::unordered_map<std::string_view, std::vector<std::string_view>>& RequestContext::queryParamAll() const {
    parseQueryIfNeeded(); // 确保已解析

    // 返回所有参数键值对
    return queryParams_;
}

// --- 响应构建实现 ---

HttpResponse& RequestContext::response() {
    return response_;
}

void RequestContext::string(const http::status status, const std::string_view body, const std::string_view content_type) {
    response_.result(status);
    response_.set(http::field::content_type, content_type);
    response_.body() = body;
}

/*void RequestContext::json(http::status status, const nlohmann::json& j) {
    response_.result(status);
    response_.set(http::field::content_type, "application/json");
    response_.body() = j.dump(); // nlohmann::json::dump() 返回 std::string
    compressIfAcceptable();

}*/

void RequestContext::json(http::status status, std::string_view json) {
    response_.result(status);
    response_.set(http::field::content_type, "application/json;charset=UTF-8");
    response_.body() = json;
}


// --- 私有辅助函数实现 ---

void RequestContext::parseQueryIfNeeded() const {
    if (queryParamsParsed_) return;

    // 标记为已解析，即使没有查询参数也只执行一次
    queryParamsParsed_ = true;

    std::string_view target = request_.target();
    auto pos = target.find('?');
    if (pos == std::string_view::npos) {
        return; // 没有查询字符串
    }

    std::string_view query_str = target.substr(pos + 1);
    if (query_str.empty()) {
        return; // 查询字符串为空
    }

    // 清空 buffer，为本次解析做准备
    decodeBuffer_.clear();
    // 可以预分配一些空间以提高性能
    decodeBuffer_.reserve(query_str.length());

    // 注意：这里如果直接 .value() 可能会在解析失败时BOOST_ASSERT_IS_ON或std::terminate()导致程序崩溃
    //boost::urls::params_view params = boost::urls::parse_query(query_str).value();
    auto result = boost::urls::parse_query(query_str);

    if (!result.has_value()) {
        spdlog::warn("Query parse failed: {}", query_str);
        return;
    }
    boost::urls::params_view params = result.value(); // 安全
    for (const auto& param : params) {
        // param.key 和 param.value 已经是解码后的 std::string
        // 我们需要将它们的内容拷贝到我们自己的稳定 buffer 中

        // 1. 将解码后的 Key 存入 buffer
        const size_t key_start_pos = decodeBuffer_.length();
        decodeBuffer_.append(param.key); // 直接追加已解码的 key
        std::string_view key_sv(decodeBuffer_.data() + key_start_pos, param.key.length());

        // 2. 将解码后的 Value 存入 buffer
        const size_t value_start_pos = decodeBuffer_.length();
        if (param.has_value) {
            decodeBuffer_.append(param.value); // 直接追加已解码的 value
        }
        std::string_view value_sv(decodeBuffer_.data() + value_start_pos, param.has_value ? param.value.length() : 0);

        // 3. 将指向 buffer 的稳定 string_view 存入 map
        queryParams_[key_sv].push_back(value_sv);
    }
}


void RequestContext::urlDecode(std::string_view sv, std::string& buffer) {
    for (size_t i = 0; i < sv.size(); ++i) {
        if (sv[i] == '%' && i + 2 < sv.size()) {
            const char hex[3] = {sv[i + 1], sv[i + 2], '\0'};
            if (isxdigit(hex[0]) && isxdigit(hex[1])) {
                buffer += static_cast<char>(std::strtol(hex, nullptr, 16));
                i += 2;
            } else {
                buffer += '%'; // 非法编码，保留原始字符
            }
        } else if (sv[i] == '+') {
            buffer += ' ';
        } else {
            buffer += sv[i];
        }
    }
}

bool contains_ci(std::string_view haystack, std::string_view needle) {
    return std::ranges::search(haystack, needle, [](const char a, const char b) {
                                   return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                               }
    ).begin() != haystack.end();
}

bool is_compressible_content_type(std::string_view content_type) {
    // 常见可压缩类型（可扩展）
    constexpr std::string_view compressible_keywords[] = {
        "text/",
        "json",
        "xml",
        "javascript",
        "x-www-form-urlencoded",
        "csv",
        "yaml",
        "log"
    };

    for (auto keyword : compressible_keywords) {
        if (contains_ci(content_type, keyword)) {
            return true;
        }
    }

    return false;
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
boost::asio::awaitable<void> RequestContext::compressIfAcceptable(const boost::asio::any_io_executor& work_executor) {
    // 1. 检查客户端是否支持压缩
    std::string client_accepts_encoding;
    if (request_.count(http::field::accept_encoding)) {
        std::string_view value = request_.at(http::field::accept_encoding);
        if (value.find("gzip") != std::string_view::npos) {
            client_accepts_encoding = "gzip";
        } else if (value.find("deflate") != std::string_view::npos) {
            client_accepts_encoding = "deflate";
        } else {
            co_return; // 客户端不支持，直接返回
        }
    } else {
        co_return;
    }
    //  检查 Content-Type
    if (response_.count(http::field::content_type)) {
        const std::string_view content_type = response_.at(http::field::content_type);
        // 只压缩已知的文本类型
        if (!is_compressible_content_type(content_type)) {
            co_return; // 不是可压缩的类型，直接返回
        }
    }


    // 2. 检查响应是否已经被压缩过了（防止重复压缩）
    if (response_.count(http::field::content_encoding)) {
        co_return;
    }

    // 3. 检查响应体大小是否值得压缩
    constexpr size_t min_compress_size = 1024 * 5; // 最小压缩大小 5KB
    constexpr size_t min_async_compression_size = 1024 * 10; // 最小异步压缩大小 10KB
    if (response_.body().size() < min_compress_size) {
        co_return; // body 太小，不值得压缩
    }

    // 4. 执行压缩
    // 注意：这里我们直接修改内部的 response_ 对象
    try {
        std::string compressed_body;
        if (response_.body().size() >= min_async_compression_size) {
            auto [ex_ptr, result] = co_await boost::asio::co_spawn(work_executor,
                                                                   [body = response_.body(), encoding = client_accepts_encoding]() -> boost::asio::awaitable<std::string> {
                                                                       co_return (encoding == "gzip") ? utils::compression::compression_manager::gzip_compress(body) : utils::compression::compression_manager::deflate_compress(body);
                                                                   },
                                                                   boost::asio::as_tuple(boost::asio::use_awaitable)
            );
            if (ex_ptr) {
                try {
                    std::rethrow_exception(ex_ptr); // 重新抛出异常
                } catch (const std::exception& e) {
                    SPDLOG_WARN("异步压缩失败: {}", e.what());
                    co_return; // 失败则直接返回，不修改 response
                }
            }
            compressed_body = result;
        } else {
            compressed_body = client_accepts_encoding == "gzip" ? utils::compression::compression_manager::gzip_compress(response_.body()) : utils::compression::compression_manager::deflate_compress(response_.body());
        }
        // 5. 更新响应对象
        // 只有在压缩成功且有效时才更新
        if (compressed_body.size() < response_.body().size()) {
            response_.body() = std::move(compressed_body);
            response_.set(http::field::content_encoding, client_accepts_encoding);
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("压缩失败: {}", e.what());
        co_return;
    }
}
