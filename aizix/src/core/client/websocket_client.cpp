//
// Created by Aiziboy on 25-8-5.
//

#include <aizix/core/client/websocket_client.hpp>
#include <aizix/http/network_constants.hpp>

#include <aizix/lib/ada.h>
#include <boost/asio/redirect_error.hpp>
#include <spdlog/spdlog.h>

#include <aizix/App.hpp>


WebSocketClient::WebSocketClient(aizix::App& app)
    : app_(app),
      main_ioc_(app.get_main_ioc()), // Resolver 绑定在 Main Context
      // 初始化一个专门用于 WebSocket 的 ssl::context
      ssl_ctx_(boost::asio::ssl::context::tls_client),
      resolver_(main_ioc_) {
    // 配置这个专用的 ssl_ctx_
    ssl_ctx_.set_options(network::ssl::CONTEXT_OPTIONS);

    //只宣告支持 http/1.1，为 WebSocket Upgrade 做准备
    if (SSL_CTX_set_alpn_protos(ssl_ctx_.native_handle(), network::alpn::PROTOS_H1_ONLY, sizeof(network::alpn::PROTOS_H1_ONLY)) != 0) {
        throw std::runtime_error("Failed to set ALPN protocols for WebSocket client.");
    }

    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(app_.config().client.ssl_verify ? boost::asio::ssl::verify_peer : boost::asio::ssl::verify_none);
}

std::string base64_encode(const unsigned char* input, const int length) {
    BIO* bio_st = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL); // 不换行
    b64 = BIO_push(b64, bio_st);
    BIO_write(b64, input, length);
    BIO_flush(b64);
    BUF_MEM* buf_mem_st;
    BIO_get_mem_ptr(b64, &buf_mem_st);
    std::string encoded(buf_mem_st->data, buf_mem_st->length);
    BIO_free_all(b64);
    return encoded;
}

std::string generate_websocket_key() {
    unsigned char key[16];
    if (RAND_bytes(key, sizeof(key)) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return base64_encode(key, sizeof(key));
}

boost::asio::awaitable<std::shared_ptr<WebSocketConnection>> WebSocketClient::connect(std::string_view url, std::shared_ptr<IWebsocketClientHandler> handler, const Headers& headers) {
    auto [scheme, host, port, target] = parse_url(url);
    bool use_ssl = (scheme == "wss:");
    if (!use_ssl && scheme != "ws:") {
        throw std::invalid_argument("无效的 WebSocket URL scheme");
    }

    using PlainStream = boost::beast::tcp_stream;
    using SslStream = boost::beast::ssl_stream<PlainStream>;
    using TransportStream = std::variant<std::shared_ptr<PlainStream>, std::shared_ptr<SslStream>>;
    using results_type = boost::asio::ip::tcp::resolver::results_type;

    // SPDLOG_DEBUG("scheme={},host={},port={},target={}", scheme, host, port, target);
    TransportStream transport;
    // b. DNS 解析(在 Main Context 执行)
    boost::system::error_code ec;
    results_type endpoints = co_await resolver_.async_resolve(
        boost::asio::ip::tcp::v4(),
        host,
        std::to_string(port),
        boost::asio::redirect_error(boost::asio::use_awaitable, ec)

    );
    if (ec) {
        throw boost::system::system_error(ec, "async_resolve failed");
    }


    //  获取一个 Worker IO Context
    auto& worker_ioc = app_.get_ioc();

    if (use_ssl) {
        // Stream 绑定到 Worker IO Context
        // 之后的 SSL 握手、WebSocket 读写都在 Worker IO Context 线程并行执行
        auto stream = std::make_shared<SslStream>(worker_ioc, ssl_ctx_);

        // TCP 连接（迭代器重载 + redirect_error）
        ec.clear();
        co_await stream->next_layer().async_connect(
            endpoints.begin(),
            endpoints.end(),
            boost::asio::redirect_error(boost::asio::use_awaitable, ec)
        );
        if (ec) throw boost::system::system_error(ec, "tcp async_connect failed");

        // SNI 设置
        std::string host_str(host);
        if (!SSL_set_tlsext_host_name(stream->native_handle(), host_str.c_str())) {
            throw boost::system::system_error(static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category());
        }

        //  TLS 握手 (在 Worker IO Context 线程执行)
        ec.clear();
        co_await stream->async_handshake(boost::asio::ssl::stream_base::client, boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec) throw boost::system::system_error(ec, "tls async_handshake failed");

        transport = std::move(stream);
    } else {
        // Plain Stream 绑定到 Worker IO Context 线程
        auto stream = std::make_shared<PlainStream>(main_ioc_);

        // TCP 连接（迭代器重载 + redirect_error）
        ec.clear();
        co_await stream->async_connect(
            endpoints.begin(),
            endpoints.end(),
            boost::asio::redirect_error(boost::asio::use_awaitable, ec)
        );
        if (ec) throw boost::system::system_error(ec, "tcp async_connect failed");

        transport = std::move(stream);
    }


    // 1.2. 将 TransportStream 包装成 WsVariantStream
    WebSocketConnection::WsVariantStream ws_transport;
    std::visit([&ws_transport](auto& underlying_stream_ptr) {
        using UnderlyingStream = std::remove_reference_t<decltype(*underlying_stream_ptr)>;
        using WsStream = websocket::stream<UnderlyingStream>;
        ws_transport = std::make_shared<WsStream>(std::move(*underlying_stream_ptr));
    }, transport);

    // 2. 构建初始的 HTTP Upgrade 请求
    HttpRequest upgrade_req{http::verb::get, target.empty() ? "/" : target, 11};
    upgrade_req.set(http::field::host, host);
    upgrade_req.set(http::field::user_agent, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    upgrade_req.set(http::field::accept, "*/*");
    upgrade_req.set(http::field::accept_encoding, "gzip, deflate");
    upgrade_req.set(http::field::accept_language, "en-US,en;q=0.9");

    // WebSocket 专用头部
    upgrade_req.set(http::field::connection, "Upgrade");
    upgrade_req.set(http::field::upgrade, "websocket");
    upgrade_req.set(http::field::sec_websocket_version, "13");
    upgrade_req.set(http::field::sec_websocket_key, generate_websocket_key());
    // 声明支持 permessage-deflate 压缩扩展
    upgrade_req.set(http::field::sec_websocket_extensions, "permessage-deflate; client_max_window_bits");

    // 添加用户自定义的头部
    for (const auto& field : headers) {
        upgrade_req.set(field.name(), field.value());
    }

    // 3. 创建 WebSocketConnection 实例
    static std::atomic<uint64_t> conn_counter = 0;
    std::string conn_id = "ws-conn-" + std::to_string(++conn_counter);
    auto ws_conn = std::make_shared<WebSocketConnection>(
        std::move(ws_transport), std::move(upgrade_req), std::move(handler), std::move(conn_id)
    );


    // 4. 启动 WebSocketConnection 的 actor_loop
    // ws_conn->run() 会在其内部 stream 所属的 executor (即 Worker IO Context 线程) 上运行 Actor 循环
    ws_conn->run();

    // 5. 握手成功，返回可用的连接
    co_return ws_conn;
}

/**
 * @brief 为 WebSocketClient 安全、高效地解析 URL 字符串。
 *
 * 与 HttpClient 的解析器不同，此版本有以下特点：
 * - **不自动补全协议**: 强制要求用户提供包含 `ws://` 或 `wss://` 的完整 URL。
 *   这是因为 WebSocket 没有像 HTTP 那样普遍接受的默认协议，强制明确可以避免连接错误。
 * - **协议验证**: 检查解析出的 scheme 是否确实是 "ws" 或 "wss"。
 *
 * @param url_strview 要解析的 WebSocket URL 字符串视图。
 * @return ParsedUrl 结构体。
 * @throws std::runtime_error 如果 URL 格式无效或协议不是 ws/wss。
 */
WebSocketClient::ParsedUrl WebSocketClient::parse_url(const std::string_view url_strview) {
    // 1. 直接解析 URL
    auto url = ada::parse<ada::url_aggregator>(url_strview);


    // 2. 如果初步解析失败，通常是因为缺少协议头，尝试补全并重试
    if (!url) {
        throw std::runtime_error("Failed to parse WebSocket URL: " + std::string(url_strview));
    }


    // 3. 从解析结果中提取所需信息
    ParsedUrl result;
    // get_protocol() 和 get_hostname() 返回 string_view，
    // 在这里隐式转换为 string 并拷贝给成员变量。这是必要的拷贝，将数据从临时解析器中保存下来。
    result.scheme = url->get_protocol();
    result.host = url->get_hostname();
    SPDLOG_DEBUG("scheme = {}", result.scheme);

    // 确保协议是 "ws:" 或 "wss:"
    // ada-url 返回的 get_protocol() 会包含冒号 ":"
    if (result.scheme != "ws:" && result.scheme != "wss:") {
        throw std::runtime_error("Invalid WebSocket protocol: '" + result.scheme + "'. Must be 'ws:' or 'wss:'。");
    }

    // 5. 解析端口
    const std::string_view port = url->get_port();
    if (port.empty()) {
        // 如果端口字段为空，则根据 scheme 获取标准默认端口 (如 ws ---> 80, wss ---> 443)
        result.port = url->scheme_default_port();
    } else {
        // 否则，转换端口号（使用 std::from_chars 进行严格、无异常、高性能的解析）
        uint16_t parsed_port;
        auto [ptr, ec] = std::from_chars(port.data(), port.data() + port.size(), parsed_port);
        // 确保整个字符串都被解析了 (ptr == end)，并且没有发生错误 (ec == OK)
        if (ec != std::errc{} || ptr != port.data() + port.size()) {
            throw std::runtime_error("Invalid port in WebSocket URL: '" + std::string(port) + "'");
        }
        result.port = parsed_port;
    }

    // 6. 高效地组合 target (path + query)
    const std::string_view pathname = url->get_pathname();
    const std::string_view search = url->get_search();

    if (pathname.empty() && search.empty()) {
        result.target = "/";
    } else {
        // 预分配内存，然后通过 append(string_view) 追加，避免因 '+' 操作符而产生不必要的临时 std::string
        result.target.reserve(pathname.length() + search.length());
        result.target.append(pathname);
        result.target.append(search);
    }

    SPDLOG_DEBUG("Parsed WebSocket URL successfully: scheme={}, host={}, port={}, target={}", result.scheme, result.host, result.port, result.target);
    return result;
}
