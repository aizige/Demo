#include "http_client.hpp"

#include <functional> // for std::function
#include <stdexcept>
#include <spdlog/spdlog.h>
#include <ada.h>
#include <boost/asio/detail/impl/scheduler.ipp>

#include "h2c_connection.hpp"
#include "h2_connection.hpp"
#include "iconnection.hpp"
#include "utils/decompression_manager.hpp"


// 构造函数，注意成员初始化顺序
HttpClient::HttpClient(boost::asio::io_context& ioc)
    : ioc_(ioc),
      manager_(std::make_shared<ConnectionManager>(ioc_)) {
}

// 实现接口中的 get 方法
boost::asio::awaitable<HttpResponse> HttpClient::get(std::string_view url, const Headers& headers) {
    auto response = co_await execute(http::verb::get, url, "", headers);
    co_return response;
}

// 实现接口中的 post 方法
// 注意：它不处理 content-type，这被认为是 header 的一部分
boost::asio::awaitable<HttpResponse> HttpClient::post(std::string_view url, std::string body, const Headers& headers) {
    // 调用者应该在 headers 中设置 Content-Type
    // 如果没有，beast 可能会有一个默认值，或者服务器可能会拒绝
    auto response = co_await execute(http::verb::post, url, std::move(body), headers);
    co_return response;
}

//  一个辅助函数来解析和组合 URL
std::string HttpClient::resolve_url(const std::string& base_url, const std::string& location) {
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

boost::asio::awaitable<HttpResponse> HttpClient::execute(http::verb method, std::string_view url, std::string body, const Headers& headers) {
    int redirects_left = follow_redirects_ ? max_redirects_ : 0;

    // --- **[关键]** 将请求参数保存起来，以便在循环中修改 ---
    std::string current_url(url);
    http::verb current_method = method;
    std::string current_body = std::move(body);
    Headers current_headers = headers;
    bool is_first_request = true; // [新增] 标志位，用于判断是否是第一次请求

    while (redirects_left-- >= 0) {
        ParsedUrl target = parse_url(current_url);
        SPDLOG_DEBUG("正在对 {} 发起请求", url);

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
        for (const auto& field : current_headers) {
            req.set(field.name(), field.value());
        }

        // --- **[新增]** H2C Upgrade 头部注入 ---
        // 只在第一次请求、明文HTTP、且是幂等方法时尝试升级
        if (is_first_request && target.scheme == "http" && (current_method == http::verb::get || current_method == http::verb::head)) {
            req.set(http::field::connection, "Upgrade, HTTP2-Settings");
            req.set(http::field::upgrade, "h2c");

            // HTTP2-Settings header: base64url 编码的空 SETTINGS 帧 payload
            // 对于客户端请求，一个空的 settings payload "AAAAAA==" 是常见且安全的
            req.set(http::field::http2_settings, "AAAAAA==");
        }
        is_first_request = false; // 后续重定向不再是第一次请求

        // 设置 body
        if (!current_body.empty()) {
            if (req.find(http::field::content_type) == req.end()) {
                req.set(http::field::content_type, "application/octet-stream");
            }
            req.body() = current_body;
            req.prepare_payload();
        }

        // 2. 执行一次请求
        auto [res, conn] = co_await execute_internal(req, target);

        // --- **[新增]** H2C Upgrade 响应处理 ---
        if (res.result() == http::status::switching_protocols && res.count(http::field::upgrade) && boost::beast::iequals(res.at(http::field::upgrade), "h2c")) {
            spdlog::info("H2C Upgrade successful for {}. Replacing connection.", current_url);


            // a. 从 Http1Connection 中“窃取” socket
            auto socket_opt = co_await conn->release_socket();
            if (!socket_opt || !socket_opt->is_open()) {
                throw std::runtime_error("Failed to release socket for H2C upgrade.");
            }

            // b. 创建一个新的 H2cConnection
            auto h2c_stream = std::make_shared<Http2cConnection::StreamType>(std::move(*socket_opt));
            auto h2c_conn = std::make_shared<Http2cConnection>(h2c_stream, conn->get_pool_key());
            // c. 启动 H2C 会话（发送 preface settings）
            h2c_conn->start();

            // d. 在连接池中用新连接替换旧连接
            manager_->release_connection(conn);

            // e. 在【新的 H2C 连接】上【重新发送】原始请求
            //    我们直接 co_return，因为升级后不会再有重定向
            co_return co_await h2c_conn->execute(req);
        }


        // 3. 检查是否是重定向状态码
        auto status_code = res.result_int();
        if (status_code >= 300 && status_code < 400) {
            auto loc_it = res.find(http::field::location);
            if (loc_it == res.end()) {
                // 重定向响应没有 Location 头，这是一个服务器错误，我们直接返回
                co_return res;
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
            std::string decompressed_body;
            if (boost::beast::iequals(it->value(), "gzip")) {
                // **直接调用线程安全的静态方法**
                decompressed_body = utils::compression::DecompressionManager::gzip_decompress(res.body());
                res.body() = std::move(decompressed_body);

                // 4. **非常重要**：移除或更新头部
                //    因为 body 已经变了，原始的 Content-Length 不再有效
                //    同时，内容也不再是 gzip 编码了
                res.erase(http::field::content_encoding);
                res.prepare_payload();
            } else if (boost::beast::iequals(it->value(), "deflate")) {
                // 切换解压器到 DEFLATE 模式
                decompressed_body = utils::compression::DecompressionManager::deflate_decompress(res.body());
                res.body() = std::move(decompressed_body);
                res.erase(http::field::content_encoding);
                res.prepare_payload();
            }
        }
        co_return res;
    }

    // 不可达，但为了编译器满意
    throw std::runtime_error("Too many redirects.");
}

// 创建一个辅助函数来检查错误码，让代码更干净
bool is_retryable_network_error(const boost::system::error_code& ec) {
    return ec == boost::beast::http::error::end_of_stream || // 当尝试写入一个对方已关闭接收的连接时
        ec == boost::asio::error::eof || // 当你尝试读取一个对方已关闭发送的连接时
        ec == boost::asio::error::connection_reset || //对一个已关闭的端口发送数据
        ec == boost::asio::error::broken_pipe; // 当尝试写入一个对方已关闭接收的连接时
}

// 统一的内部执行函数，负责连接管理
boost::asio::awaitable<HttpClient::InternalResponse> HttpClient::execute_internal(HttpRequest& request, const ParsedUrl& target) {
    // --- 使用 for 循环来实现重试逻辑 ---
    for (int attempt = 1; attempt <= 2; ++attempt) {
        SPDLOG_DEBUG("去连接池获取连接");
        PooledConnection pooled_conn ;
        std::shared_ptr<IConnection> conn;
        try {
            // 1. 获取连接及其来源信息
            pooled_conn = co_await manager_->get_connection(target.scheme, target.host, target.port);
            conn = pooled_conn.connection;

            if (!conn) throw std::runtime_error("Failed to acquire a connection.");

            // 3. 执行请求
            HttpResponse response = co_await conn->execute(request);
            manager_->release_connection(conn);
            // **成功，立即返回，跳出循环**
            co_return std::make_pair(std::move(response), conn);
        } catch (const boost::system::system_error& e) {
            // 4.  在 catch 块中，我们只做决策，不 co_await
            SPDLOG_WARN("连接过期了 {}", e.code().value());
            // 如果不满足重试条件，则立即重新抛出异常，终止循环
            if (attempt > 1 || !pooled_conn.is_reused || !is_retryable_network_error(e.code())) {
                throw; // 向上抛出
            }

            // 如果满足重试条件，记录日志，然后什么也不做，让循环自然进入下一次迭代
            SPDLOG_WARN("Stale connection [{}] detected. Retrying request (attempt {}/2)...",
                        conn->id(),
                        attempt);

            // **注意**：这里没有 co_await！catch 块正常结束。
        }
    }
    throw std::runtime_error("HttpClient: All retry attempts failed.");
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

    SPDLOG_DEBUG("Parsed URL successfully: {}", url->get_href());


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
        } catch (const std::exception& e) {
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
