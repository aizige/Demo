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


/**
 * @brief HttpClient 的构造函数。
 * @param manager 一个 ConnectionManager 的共享指针，HttpClient 将依赖它来获取和管理连接。
 */
HttpClient::HttpClient(std::shared_ptr<ConnectionManager> manager)
    : manager_(std::move(manager))
// manager_(std::make_shared<ConnectionManager>(ioc_))

{
}

// 实现接口中的 get 方法
boost::asio::awaitable<HttpResponse> HttpClient::get(std::string_view url, const Headers& headers) {
    auto response = co_await execute(http::verb::get, url, "", headers);
    co_return response;
}

// 实现接口中的 post 方法
// 注意：它不处理 content-type，这被认为是 header 的一部分
boost::asio::awaitable<HttpResponse> HttpClient::post(std::string_view url, const std::string& body, const Headers& headers) {
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

/**
 * @brief 所有HTTP请求的统一入口点。
 *
 * 这个协程负责编排整个HTTP请求的生命周期，包括：
 * - 构建HTTP请求对象。
 * - 自动处理多达 `max_redirects_` 次的HTTP重定向。
 * - 在请求结束后自动解压缩响应体。
 * - 通过RAII guard确保连接在使用后被安全地释放回连接池。
 *
 * @return 最终的 HttpResponse 对象。
 * @throws std::runtime_error 如果重定向次数过多或发生其他严重错误。
 * @throws boost::system::system_error 如果发生不可重试的网络错误。
 */
/*boost::asio::awaitable<HttpResponse> HttpClient::execute(http::verb method, std::string_view url, std::string body, const Headers &headers) {
    int redirects_left = follow_redirects_ ? max_redirects_ : 0;

    // 将请求参数保存起来，以便在循环中修改
    std::string current_url(url);
    http::verb current_method = method;
    std::string current_body = std::move(body);
    Headers current_headers = headers;

    // 创建一个 optional<pair> 来持有结果和连接
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
                req.set(http::field::accept, "#1#*");
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
}*/

/**
 * @brief 所有HTTP请求的统一入口点，实现了重定向处理和零拷贝优化。
 *
 * 该协程采用“写时复制”(Copy-on-Write)策略：
 * - 在不需要重定向的“快乐路径”上，它通过 string_view 和 const 指针
 *   零拷贝地使用传入的 body 和 headers。
 * - 只有在发生重定向且需要修改请求参数（如方法、body、headers）时，
 *   它才会创建这些参数的本地副本。
 *
 * 这种设计在保证API对调用者安全（使用 const&）的同时，实现了极致的性能。
 *
 * @param method HTTP 方法。
 * @param url 请求的 URL。
 * @param body 请求体 (const 引用，函数不会修改调用者的数据)。
 * @param headers 自定义的 HTTP 头部 (const 引用)。
 * @return 最终的 HttpResponse 对象。
 * @throws std::runtime_error 如果重定向次数过多或发生其他严重错误。
 * @throws boost::system::system_error 如果发生不可重试的网络错误。
 */
boost::asio::awaitable<HttpResponse> HttpClient::execute(http::verb method, std::string_view url, const std::string& body, const Headers& headers) {
    int redirects_left = max_redirects_;

    try {
        // --- 状态管理：使用string_view/指针和 optional 实现“写时复制” ---
        // 将请求参数保存起来，以便在循环中修改

        // URL 状态
        std::string_view current_url_sv = url;
        std::optional<std::string> modified_url; // 仅在重定向时分配

        // 请求方法 (可直接修改)
        http::verb current_method = method;

        // Body 状态
        std::string_view current_body_sv(body); // 初始指向原始 body (零拷贝)
        // 注意：我们不需要 optional<string> 来存 body 副本，
        // 因为 POST->GET 转换后 body 总是空的。

        // Headers 状态
        const Headers* current_headers_ptr = &headers; // 初始指向原始 headers
        std::optional<Headers> modified_headers; // 仅在需要修改时分配


        // 创建一个 optional<pair> 来持有结果和连接
        // 这样可以确保连接和响应的生命周期被绑定在一起
        std::optional<InternalResponse> result_pair;

        // 使用 Finally guard 确保只要 result_pair 有值，连接就会被释放
        auto connection_guard = Finally([&] {
            if (result_pair && result_pair->second) {
                // result_pair->second 就是连接
                manager_->release_connection(result_pair->second);
            }
        });


        while (redirects_left-- >= 0) {
            // --- 1. 构建请求 (基于当前状态，尽可能零拷贝) ---
            ParsedUrl target = parse_url(current_url_sv);


            // 1. 创建 Request 对象
            // 拷贝只在 body 非空时发生在这里。
            HttpRequest req{current_method, target.target, 11};
            req.set(http::field::host, target.host);
            req.set(http::field::user_agent, "MyFramework/1.0");

            // 设置通用头 (Accept, etc.)(如果用户没有提供)
            if (current_headers_ptr->find(http::field::accept) == current_headers_ptr->end()) {
                req.set(http::field::accept, "*/*");
            }
            if (current_headers_ptr->find(http::field::accept_encoding) == current_headers_ptr->end()) {
                req.set(http::field::accept_encoding, "gzip,deflate");
            }
            if (current_headers_ptr->find(http::field::accept_language) == current_headers_ptr->end()) {
                req.set(http::field::accept_language, "en-US,en;q=0.9");
            }
            if (current_headers_ptr->find(http::field::connection) == current_headers_ptr->end()) {
                req.set(http::field::connection, "keep-alive");
            }

            // 合并用户提供的头部
            for (const auto& field : *current_headers_ptr) {
                req.set(field.name(), field.value());
            }

            // 设置 body
            if (!current_body_sv.empty()) {
                if (req.find(http::field::content_type) == req.end()) {
                    req.set(http::field::content_type, "application/octet-stream");
                }
                req.body() = current_body_sv; // 从 view 拷贝到 string
                req.prepare_payload();
            }

            // 2. 执行一次请求
            // connection_guard 会在函数最终退出时处理一切。
            // 如果有上一次循环的连接，它的 shared_ptr 在 result_pair 被重新赋值时会自动析构。
            result_pair.emplace(co_await execute_internal(req, target));

            HttpResponse& res = result_pair->first;

            // --- 3. 处理重定向 ---
            auto status_code = res.result_int();
            if (status_code >= 301 && status_code <= 308) {
                auto loc_it = res.find(http::field::location);
                if (loc_it == res.end()) {
                    // 重定向响应没有 Location 头，这是一个服务器错误，我们直接返回
                    co_return std::move(res);
                }


                // 检查是否还有重试次数
                if (redirects_left < 0) {
                    SPDLOG_WARN("Too many redirects");
                    co_return std::move(res);
                }

                std::string new_location(loc_it->value());

                SPDLOG_DEBUG("Redirecting from {} to {}", current_url_sv, new_location);
                // [!!! 写时复制逻辑 !!!]

                // a. URL 总是需要更新，所以为其分配副本
                modified_url = resolve_url(std::string(current_url_sv), new_location);
                current_url_sv = *modified_url;

                // ---  根据 RFC 7231，更新方法、Body 和 Headers ---
                if (status_code == 301 || status_code == 302 || status_code == 303) {
                    // 对于这些状态码，非 GET/HEAD 请求通常会变成 GET，并清空 body
                    if (current_method != http::verb::get && current_method != http::verb::head) {
                        current_method = http::verb::get;
                        current_body_sv = ""; // 清空Body

                        // 写时复制：仅在第一次需要修改时创建 Headers 副本
                        if (!modified_headers) {
                            modified_headers.emplace(headers); // 第一次修改，从原始 headers 拷贝
                        }
                        modified_headers->erase(http::field::content_length);
                        modified_headers->erase(http::field::content_type);
                        modified_headers->erase(http::field::transfer_encoding);

                        // 指针指向修改后的副本
                        current_headers_ptr = &(*modified_headers);
                    }
                }
                // 对于 307 和 308 (以及其他未明确处理的)，保持方法和 body 不变

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
    } catch (const std::exception&) {
        throw;
    }
    // 不可达，但为了编译器满意
    throw std::runtime_error("Too many redirects.");
}

/**
 * @brief [私有] 检查给定的网络错误码是否属于“可重试”类型。
 *
 * 可重试的错误通常是由于复用一个已被服务器关闭的“陈旧连接”(stale connection)
 * 导致的。对于新创建的连接，这些错误通常表示更严重的问题。
 */
bool is_retryable_network_error(const boost::system::error_code& ec) {
    return ec == boost::beast::http::error::end_of_stream || // 当尝试写入一个对方已关闭接收的连接时
        ec == boost::asio::error::eof || // 当你尝试读取一个对方已关闭发送的连接时
        ec == boost::asio::error::connection_reset || //对一个已关闭的端口发送数据
        ec == boost::asio::error::connection_aborted || // 连接已关闭或已收到 GOAWAY的连接
        ec == boost::asio::error::broken_pipe || // 当尝试写入一个对方已关闭接收的连接时
        ec == my_error::h2::receive_timeout; // 等待H2响应超时，网络不好的时候好像会出现这种问题
}


/**
 * @brief [私有] 负责单次请求的执行，并包含对陈旧连接的自动重试逻辑。
 *
 * @param request 要发送的 const HttpRequest 引用。
 * @param target 已解析的目标URL信息。
 * @return a pair containing the HttpResponse and the IConnection used.
 */
boost::asio::awaitable<HttpClient::InternalResponse> HttpClient::execute_internal(const HttpRequest& request, const ParsedUrl& target) const {
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


            if (!conn->supports_multiplexing() && conn->get_active_streams() > 0) {
                // 这个 H1.1 连接正在被别人使用！
                SPDLOG_WARN("[{}] 正在被他人使用，重新获取", conn->id());
                manager_->release_connection(conn);
                // 立即重试，获取另一个连接
                //++attempt;
                continue; // 跳到下一次循环
            }

            // 🔹 第三步：执行请求（可能抛出 system_error）
            HttpResponse response = co_await conn->execute(request);

            // ✅ 请求成功，立即返回响应和连接
            co_return std::make_pair(std::move(response), conn);
        } catch (const boost::system::system_error& e) {
            // --- 错误处理与重试逻辑 ---

            // [!!! 在决定重试前，先释放坏连接 !!!]
            // 如果 conn 是有效的（即我们成功获取了连接但执行失败），
            // 我们需要通知 ConnectionManager 这个连接可能已经失效了。
            if (conn) {
                // 我们不需要手动调用 close()，只需要 release_connection。
                // ConnectionManager 的 release_connection 会检查 is_usable()，
                // 发现它（可能）已经被 execute() 内部标记为 false，然后丢弃它。
                manager_->release_connection(conn);
            }


            // 🔹 判断是否满足重试条件：
            //   - 错误码属于可重试的网络错误
            bool should_retry = (
                attempt < 2 && //   - 还有重试次数
                pooled_conn.is_reused); // 必须是复用连接才重试（新连接失败通常是配置问题）
              // is_retryable_network_error(e.code()) // 对于复用的连接不再检查错误码

            // ❌ 不满足重试条件，记录日志并向上抛出异常
            if (!should_retry) {
                SPDLOG_ERROR("Request failed and is not retryable (attempt {}): {}", attempt, e.what());
                throw;
                // 注意：如果 conn 在这里被获取了，但因为异常没有被返回，它的 shared_ptr
                // 会在这里被销毁，引用计数减一。如果这是唯一的引用，对象会被析构。
                // 这一切都是自动且正确的。
            }

            // ✅ 满足重试条件，记录日志，继续下一轮尝试
            SPDLOG_WARN("已过期的连接 [{}] 正在重试 (尝试次数 {}/2)...",
                        conn ? conn->id() : "N/A",
                        attempt + 1);
        }

        // 🔹 显式递增重试计数器，进入下一轮尝试
        ++attempt;
    } // end while

    // 如果循环结束还没有成功返回，说明所有尝试都失败了。
    throw std::runtime_error("HttpClient: All retry attempts failed.");
}


/**
 * @brief [私有] 使用 ada-url 库安全地解析URL字符串。
 *
 * 包含对缺少协议头的URL的自动补全逻辑。
 * @throws std::runtime_error 如果URL格式无效。
 */
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


    ParsedUrl result;
    // 3. 从解析结果中提取信息
    result.scheme = url->get_protocol();
    result.host = url->get_hostname();


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
    SPDLOG_DEBUG("解析URL成功: scheme = {}, host = {}, port = {}, target = {}", result.scheme, result.host, result.port, result.target);
    return result;
}
