#include "http_client.hpp"

#include <functional> // for std::function
#include <stdexcept>
#include <spdlog/spdlog.h>
#include <ada.h>
#include <boost/asio/detail/impl/scheduler.ipp>

#include "h2_connection.hpp"
#include "iconnection.hpp"
#include "error/my_error.hpp"
#include "utils/compression_manager.hpp"
#include "utils/finally.hpp"


// 构造函数，注意成员初始化顺序
HttpClient::HttpClient(std::shared_ptr<ConnectionManager> manager)
    : manager_(std::move(manager))
     // manager_(std::make_shared<ConnectionManager>(ioc_))

{
}

// 实现接口中的 get 方法
boost::asio::awaitable<HttpResponse> HttpClient::get(std::string_view url, const Headers &headers) {
    auto response = co_await execute(http::verb::get, url, "", headers);
    co_return response;
}

// 实现接口中的 post 方法
// 注意：它不处理 content-type，这被认为是 header 的一部分
boost::asio::awaitable<HttpResponse> HttpClient::post(std::string_view url, std::string body, const Headers &headers) {
    // 调用者应该在 headers 中设置 Content-Type
    // 如果没有，beast 可能会有一个默认值，或者服务器可能会拒绝
    auto response = co_await execute(http::verb::post, url, std::move(body), headers);
    co_return response;
}

//  一个辅助函数来解析和组合 URL
std::string HttpClient::resolve_url(const std::string &base_url, const std::string &location) {
    if (location.find("://") != std::string::npos) {
        // Location 是一个绝对 URL，直接使用
        return location;
    }

    // Location 是一个相对 URL
    // (一个完整的实现需要正确处理 ../ 等情况，这里简化)
    auto parsed_base = parse_url(base_url);
    if (location.starts_with('/')) {
        // 根相对路径
        return parsed_base.scheme + "//" + parsed_base.host + ":" + std::to_string(parsed_base.port) + location;
    } else {
        // 相对路径
        auto last_slash = parsed_base.target.rfind('/');
        std::string base_path = (last_slash == std::string::npos) ? "/" : parsed_base.target.substr(0, last_slash + 1);
        return parsed_base.scheme + "//" + parsed_base.host + ":" + std::to_string(parsed_base.port) + base_path + location;
    }
}

// 实现接口中的 execute 方法，这是所有请求的最终入口
boost::asio::awaitable<HttpResponse> HttpClient::execute(http::verb method, std::string_view url, std::string body, const Headers &headers) {
    int redirects_left = follow_redirects_ ? max_redirects_ : 0;


    // --- **[关键]** 将请求参数保存起来，以便在循环中修改 ---
    std::string current_url(url);
    http::verb current_method = method;
    std::string current_body = std::move(body);
    Headers current_headers = headers;

    // [!!! 关键改动：创建一个 optional<pair> 来持有结果和连接 !!!]
    // 这样可以确保连接和响应的生命周期被绑定在一起
    std::optional<InternalResponse> result_pair;

    // 使用 Finally guard 确保只要 result_pair 有值，连接就会被释放
    auto connection_guard = Finally([&] {
        if (result_pair && result_pair->second) { // result_pair->second 就是连接
            manager_->release_connection(result_pair->second);
        }
    });

    try {
        while (redirects_left-- >= 0) {
            ParsedUrl target = parse_url(current_url);
            SPDLOG_DEBUG("正在对 {} 发起请求", current_url);

            // 1. 创建 Request 对象
            HttpRequest req{current_method, target.target, 11};
            req.set(http::field::host, target.host);
            req.set(http::field::user_agent, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");

            // 设置通用头 (Accept, etc.)
            if (current_headers.find(http::field::accept) == current_headers.end()) {
                req.set(http::field::accept, "*/*");
            }
            if (current_headers.find(http::field::accept_encoding) == current_headers.end()) {
                req.set(http::field::accept_encoding, "gzip, deflate");
            }
            if (current_headers.find(http::field::accept_language) == current_headers.end()) {
                req.set(http::field::accept_language, "en-US,en;q=0.9");
            }
            if (current_headers.find(http::field::connection) == current_headers.end()) {
                req.set(http::field::connection, "keep-alive");
            }

            // 合并用户头
            for (const auto &field: current_headers) {
                req.set(field.name(), field.value());
            }

            // 设置 body
            if (!current_body.empty()) {
                if (req.find(http::field::content_type) == req.end()) {
                    req.set(http::field::content_type, "application/octet-stream");
                }
                req.body() = current_body;
                req.prepare_payload();
            }

            // 2. 执行一次请求
            // connection_guard 会在函数最终退出时处理一切。
            // 如果有上一次循环的连接，它的 shared_ptr 在 result_pair 被重新赋值时会自动析构。
            result_pair.emplace(co_await execute_internal(req, target));


            HttpResponse& res = result_pair->first;

            // 3. 检查是否是重定向状态码
            auto status_code = res.result_int();
            if (status_code >= 300 && status_code < 400) {
                auto loc_it = res.find(http::field::location);
                if (loc_it == res.end()) {
                    // 重定向响应没有 Location 头，这是一个服务器错误，我们直接返回
                    co_return std::move(res);
                }
                std::string new_location(loc_it->value());

                // 检查是否还有重试次数
                if (redirects_left < 0) {
                    throw std::runtime_error("Too many redirects");
                }

                SPDLOG_DEBUG("Redirecting from {} to {}", current_url, new_location);
                current_url = resolve_url(current_url, new_location);

                // --- **[关键]** 根据不同的重定向码，更新请求参数 ---
                if (status_code == 301 || status_code == 302 || status_code == 303) {
                    // 对于这些状态码，非 GET/HEAD 请求通常会变成 GET
                    if (current_method != http::verb::get && current_method != http::verb::head) {
                        current_method = http::verb::get;
                        current_body.clear();
                        // 清除与 body 相关的头部
                        current_headers.erase(http::field::content_length);
                        current_headers.erase(http::field::content_type);
                        current_headers.erase(http::field::transfer_encoding);
                    }
                }
                // 对于 307 和 308 (以及其他未明确处理的)，我们保持方法和 body 不变

                // 继续下一次循环
                continue;
            }


            // 4. 如果不是重定向，则返回最终的响应
            // 解压Body数据
            auto it = res.find(http::field::content_encoding);
            if (it != res.end()) {
                SPDLOG_DEBUG("正在解压Body");
                std::string decompressed_body;
                if (boost::beast::iequals(it->value(), "gzip")) {
                    // **直接调用线程安全的静态方法**
                    decompressed_body = utils::compression::compression_manager::gzip_decompress(res.body());
                    res.body() = std::move(decompressed_body);

                    // 4. **非常重要**：移除或更新头部
                    //    因为 body 已经变了，原始的 Content-Length 不再有效
                    //    同时，内容也不再是 gzip 编码了
                    res.erase(http::field::content_encoding);
                    res.prepare_payload();
                } else if (boost::beast::iequals(it->value(), "deflate")) {
                    // 切换解压器到 DEFLATE 模式
                    decompressed_body = utils::compression::compression_manager::deflate_decompress(res.body());
                    res.body() = std::move(decompressed_body);
                    res.erase(http::field::content_encoding);
                    res.prepare_payload();
                }
                SPDLOG_DEBUG("正在解压Body 完毕");
            }
            // 我们需要返回 HttpResponse，但要确保连接在之后被释放。
            // 因为 co_return 会销毁局部变量，connection_guard 会被触发。
            co_return std::move(res);
        }
    } catch (const std::exception &e) {

        throw;
    }
    // 不可达，但为了编译器满意
    throw std::runtime_error("Too many redirects.");
}

// 一个辅助函数来检查错误码，让代码更干净
bool is_retryable_network_error(const boost::system::error_code &ec) {
    return ec == boost::beast::http::error::end_of_stream || // 当尝试写入一个对方已关闭接收的连接时
           ec == boost::asio::error::eof || // 当你尝试读取一个对方已关闭发送的连接时
           ec == boost::asio::error::connection_reset || //对一个已关闭的端口发送数据
           ec == boost::asio::error::connection_aborted || // 连接已关闭或已收到 GOAWAY的连接
           ec == boost::asio::error::broken_pipe || // 当尝试写入一个对方已关闭接收的连接时
           ec    == my_error::h2::receive_timeout; // 等待H2响应超时，网络不好的时候好像会出现这种问题
}

// 统一的内部执行函数，负责连接管理
// in HttpClient.cpp
//旧的v1.0与execute v1.0一起的


boost::asio::awaitable<HttpClient::InternalResponse> HttpClient::execute_internal(HttpRequest& request, const ParsedUrl& target) {

    // 初始化重试计数器（最多尝试两次）
    int attempt = 1;

    // 使用 while 循环代替 for，显式递增 attempt，避免编译器警告
    while (true) {
        if (attempt > 2) {
            throw std::runtime_error("HttpClient: All retry attempts failed after stale connection.");
        }

        PooledConnection pooled_conn;
        std::shared_ptr<IConnection> conn;

        try {
            // 🔹 第一步：从连接池获取连接（可能是复用连接）
            pooled_conn = co_await manager_->get_connection(target.scheme, target.host, target.port);
            conn = pooled_conn.connection;

            // 🔹 如果连接获取失败，抛出异常
            if (!conn) {
                throw std::runtime_error("Failed to acquire a connection.");
            }

            // 🔹 第三步：执行请求（可能抛出 system_error）
            HttpResponse response = co_await conn->execute(request);

            // ✅ 请求成功，立即返回响应和连接
            co_return std::make_pair(std::move(response), conn);

        } catch (const boost::system::system_error& e) {


            // 🔹 判断是否满足重试条件：
            //   - 还有重试次数
            //   - 当前连接是复用的（可能是 stale）
            //   - 错误码属于可重试的网络错误
            bool should_retry = (
                attempt < 2 &&
                pooled_conn.is_reused &&
                is_retryable_network_error(e.code())
            );

            // ❌ 不满足重试条件，记录日志并向上抛出异常
            if (!should_retry) {
                SPDLOG_ERROR("Request failed and is not retryable (attempt {}): {}", attempt, e.what());
                throw;
                // 注意：如果 conn 在这里被获取了，但因为异常没有被返回，它的 shared_ptr
                // 会在这里被销毁，引用计数减一。如果这是唯一的引用，对象会被析构。
                // 这一切都是自动且正确的。
            }

            // ✅ 满足重试条件，记录日志，继续下一轮尝试
            SPDLOG_DEBUG("已过期的连接 [{}] 正在重试 (尝试次数 {}/2)...",
                        conn ? conn->id() : "N/A",
                        attempt + 1);


        }

        // 🔹 显式递增重试计数器，进入下一轮尝试
        ++attempt;
    }
}


// 简单的 URL(Ada-url) 解析器实现
HttpClient::ParsedUrl HttpClient::parse_url(std::string_view url_strv) {
    std::string url_string(url_strv);

    // 1. 使用 ada::parse 解析 URL
    /**
     * has_value()：确保对象内部有有效值，是访问 ada::url_aggregator 成员（比如 is_valid）之前必须检查的第一步。
     * is_valid：在确定对象有效后，进一步判断 URL 是否满足Url有效性规则。
     * 如果未先检查 has_value() 或者 而直接调用 is_valid，当解析失败时程序可能崩溃（因为在无效的 tl::expected 上调用其成员是未定义行为）。
     */
    auto url = ada::parse<ada::url_aggregator>(url_string);

    // 如果解析失败，则补全协议并重试
    if (!url) {
        SPDLOG_WARN("Parsing failed for URL: {}, attempting with protocol...", url_strv);
        if (url_string.find("http://") != 0 && url_string.find("https://") != 0) {
            url_string = "http://" + url_string;
        }
        SPDLOG_DEBUG("Re-parsing URL: {}", url_string);

        // 再次尝试解析
        url = ada::parse<ada::url_aggregator>(url_string);
        if (!url) {
            throw std::runtime_error("Parsing failed for URL: " + url_string);
        }
    }

    // 2. 检查解析是否成功
    if (!url->is_valid) {
        SPDLOG_ERROR("Invalid URL format: {}", url_string);
        throw std::runtime_error("Invalid URL format: " + std::string{url_string});
    }

    SPDLOG_DEBUG("解析URL成功: {}", url->get_href());


    ParsedUrl result;
    // 3. 从解析结果中提取信息
    result.scheme = url->get_protocol();
    result.host = url->get_host();

    SPDLOG_DEBUG("scheme = {}", result.scheme);

    // 4. [关键] 获取端口，并处理默认值
    std::string port(url->get_port());
    if (port.empty()) {
        // 如果端口为空字符串，说明是默认端口
        // **直接调用 scheme_default_port() 获取默认端口**
        result.port = url->scheme_default_port();
    } else {
        // 否则，转换端口号
        try {
            result.port = std::stoi(port);
        } catch (const std::exception &e) {
            throw std::runtime_error("Invalid port in URL: " + port);
        }
    }
    // 5. 获取路径和查询字符串
    std::string pathname(url->get_pathname());
    std::string search(url->get_search());
    result.target = pathname + search;
    if (result.target.empty()) {
        result.target = "/";
    }

    return result;
}
