#include "http_client.hpp"

#include <functional> // for std::function
#include <stdexcept>
#include <spdlog/spdlog.h>
#include <ada.h>
#include <boost/asio/detail/impl/scheduler.ipp>
#include <boost/system/result.hpp>
#include <boost/url/parse.hpp>
#include <boost/url/url.hpp>

#include "h2_connection.hpp"
#include "iconnection.hpp"
#include "error/aizix_error.hpp"
#include "utils/compression_manager.hpp"
#include "utils/finally.hpp"


/**
 * @brief HttpClient 的构造函数。
 * @param manager 一个 ConnectionManager 的共享指针，HttpClient 将依赖它来获取和管理连接。
 */
HttpClient::HttpClient(std::shared_ptr<ConnectionManager> manager)
    : manager_(std::move(manager)),
      max_redirects_(manager_->max_redirects) {
}

// 实现接口中的 get 方法
boost::asio::awaitable<HttpResponse> HttpClient::get(const std::string_view url, const Headers& headers) {
    auto response = co_await execute(http::verb::get, url, "", headers);
    co_return response;
}

// 实现接口中的 post 方法
// 注意：它不处理 content-type，这被认为是 header 的一部分
boost::asio::awaitable<HttpResponse> HttpClient::post(const std::string_view url, const std::string& body, const Headers& headers) {
    // 调用者应该在 headers 中设置 Content-Type
    // 如果没有，beast 可能会有一个默认值，或者服务器可能会拒绝
    auto response = co_await execute(http::verb::post, url, body, headers);
    co_return response;
}

//  一个辅助函数来解析和组合 URL
std::string HttpClient::resolve_url(const std::string& base_url, const std::string& location) {
    // 1. 将字符串解析为 boost::url_view 对象。
    // url_view 是非拥有式的视图，它不分配内存，效率很高。
    boost::system::result<boost::urls::url_view> base_view_res = boost::urls::parse_uri(base_url);
    if (!base_view_res) {
        SPDLOG_WARN("Failed to parse base_url '{}': {}", base_url, base_view_res.error().message());
        return location; // base_url 无效，无法解析
    }

    // 同样，将 location 也解析为 url_view
    boost::system::result<boost::urls::url_view> ref_view_res = boost::urls::parse_uri_reference(location);
    if (!ref_view_res) {
        SPDLOG_WARN("Failed to parse location '{}': {}", location, ref_view_res.error().message());
        return location; // location 本身格式就有问题
    }

    // 2. 创建一个用于接收结果的 `url` 对象。
    // 这个对象将作为第三个参数（输出参数）传递。
    boost::urls::url resolved_url;

    // 3. 调用你找到的三参数 `resolve` 函数。
    // 它会将 `base_view` 和 `ref_view` 解析的结果写入到 `resolved_url` 中。
    const boost::system::result<void> resolve_result = boost::urls::resolve(*base_view_res, *ref_view_res, resolved_url);

    // 4. 检查操作是否成功。
    if (!resolve_result) {
        SPDLOG_WARN("Failed to resolve location '{}' against base '{}': {}",
                    location, base_url, resolve_result.error().message());
        return location; // 解析失败，返回原始 location
    }

    // 5. 如果成功，结果就在 `resolved_url` 对象中。将其转换为字符串返回。
    return resolved_url.c_str();
}



/**
 * @brief 所有HTTP请求的统一入口点，实现了重定向处理和零拷贝优化。
 *
 * 该协程负责编排整个HTTP请求的生命周期，包括：
 * - 构建HTTP请求对象。
 * - 采用“写时复制”(Copy-on-Write)策略在重定向时高效地修改请求参数。
 * - 自动处理多达 `max_redirects_` 次的HTTP重定向。
 * - 在请求结束后自动解压缩响应体。
 * - 确保在重定向的每一步中，获取到的连接都被正确释放，防止资源泄漏。
 *
 * @param method HTTP 方法。
 * @param url 请求的 URL。
 * @param body 请求体 (const 引用)。
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
            req.set(http::field::user_agent, aizix::framework::name + "/" += aizix::framework::version);

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
            // 在重定向循环中，我们需要手动管理上一个连接的释放
            if (result_pair) {
                // 如果这不是第一次循环，说明我们有一个来自上一次重定向的连接需要释放
                manager_->release_connection(result_pair->second);
                result_pair.reset(); // 清空 optional
            }

            result_pair.emplace(co_await execute_internal(req, target));

            HttpResponse& res = result_pair->first;

            // --- 3. 处理重定向 ---
            if (auto status_code = res.result_int(); status_code >= 301 && status_code <= 308) {
                auto loc_it = res.find(http::field::location);
                if (loc_it == res.end()) {
                    // 重定向响应没有 Location 头，这是一个服务器错误，我们直接返回
                    co_return std::move(res); // 无 Location 头，无法重定向
                }


                // 检查是否还有重试次数
                if (redirects_left < 0) {
                    SPDLOG_WARN("Too many redirects");
                    co_return std::move(res); // 顶层 guard 会释放最后一个连接
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
            if (auto it = res.find(http::field::content_encoding); it != res.end()) {
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
    return ec == http::error::end_of_stream || // 在 keep-alive 连接上读取，但对方已关闭。
        ec == boost::asio::error::eof || // 当你尝试读取一个对方已关闭发送的连接时
        ec == boost::asio::error::connection_reset || // 连接被对端强制重置 (RST包)。
        ec == boost::asio::error::connection_aborted || // 连接在本机中止（通常也是因为对端问题）。
        ec == boost::asio::error::broken_pipe || // 尝试写入一个已关闭读端的socket。
        ec == aizix_error::h2::receive_timeout || // 等待H2响应超时，网络不好的时候好像会出现这种问题
        ec == aizix_error::network::connection_timeout || // 连接超时
        ec == aizix_error::network::connection_error || // 连接超时
        ec == boost::asio::error::timed_out; // Asio 标准超时错误
}


/**
 * @brief [私有] 负责单次请求的执行，并包含对“陈旧连接”的自动重试逻辑。
 *
 * 此协程的核心职责是：获取一个连接，用它执行请求，并在遇到特定的、
 * 可恢复的网络错误时，自动进行有限次数的重试。
 *
 * @param request 要发送的 const HttpRequest 引用。
 * @param target 已解析的目标URL信息。
 * @return a pair containing the HttpResponse and the IConnection used.
 * @throws boost::system::system_error 如果发生不可重试的网络错误，或重试耗尽。
 * @throws std::runtime_error 如果无法获取连接。
 */
boost::asio::awaitable<HttpClient::InternalResponse> HttpClient::execute_internal(const HttpRequest& request, const ParsedUrl& target) const {
    // 初始化重试计数器（最多尝试3次）
    constexpr int MAX_ATTEMPTS = 3;

    int attempt = 1; // 尝试次数从 1 开始

    // 使用 while 循环代替 for，将逻辑控制全部移入循环体
    while (true) {
        std::shared_ptr<IConnection> conn;

        try {
            // --- 成功路径 ---
            const auto [connection, is_reused] = co_await manager_->get_connection(target.scheme, target.host, target.port);
            conn = connection;

            if (!conn) {
                const std::string key = std::string(target.scheme) + "//" + std::string(target.host) + ":" + std::to_string(target.port);
                conn = co_await manager_->create_new_connection(key, target.scheme, target.host, target.port);
                if (!conn) { continue; }
            }

            auto conn_guard = Finally([this, conn = conn]() {
                manager_->release_connection(conn);
            });

            HttpResponse response = co_await conn->execute(request);

            conn_guard.disarm();
            co_return std::make_pair(std::move(response), conn);
        } catch (const boost::system::system_error& e) {
            // --- 失败/重试路径 ---

            // 1. 判断是否还有重试机会
            if (attempt >= MAX_ATTEMPTS) {
                // 达到最大尝试次数，抛出最终错误
                SPDLOG_ERROR("HttpClient: 所有重试尝试失败。最终错误: {}", e.code().message());
                throw std::runtime_error("HttpClient: All retry attempts failed. Final error: " + e.code().message());
            }

            // 2. 判断错误是否可重试
            if (is_retryable_network_error(e.code())) {
                // 情况A: 可重试错误 (例如复用连接陈旧)
                SPDLOG_WARN("对重用连接 [{}] 的请求失败，出现可重试错误 ({})。正在重试...（尝试 {}/{}）",
                            conn ? conn->id() : "N/A", e.code().message(), attempt + 1, MAX_ATTEMPTS);

                // 显式递增尝试次数，继续循环
                attempt++;
            } else {
                // 情况B: 不可重试错误（例如权限错误、逻辑错误），直接抛出
                SPDLOG_ERROR("HttpClient: 出现不可重试错误，立即终止。错误: {}", e.code().message());
                throw; // 重新抛出当前的异常
            }
        } catch (const std::exception& e) {
            // 捕获其他运行时异常（例如 acquire null connection）
            SPDLOG_ERROR("HttpClient: 出现非系统错误异常，终止。错误: {}", e.what());
            throw;
        }
    }
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


    // 4. 获取端口，并处理默认值
    if (const std::string port(url->get_port()); port.empty()) {
        // 如果端口为空字符串，说明是默认端口
        // **直接调用 scheme_default_port() 获取默认端口**
        result.port = url->scheme_default_port();
    } else {
        // 否则，转换端口号
        try {
            result.port = std::stoi(port);
        } catch (const std::exception& e) {
            throw std::runtime_error("Invalid port in URL: '" + port + "' " + e.what());
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
