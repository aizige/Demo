//
// Created by Aiziboy on 25-8-5.
//

#include "WebSocketClient.hpp"

#include <ada.h>
#include <boost/asio/redirect_error.hpp>
#include <spdlog/spdlog.h>

WebSocketClient::WebSocketClient(boost::asio::io_context& ioc)
    : ioc_(ioc),
      // 初始化一个专门用于 WebSocket 的 ssl::context
      ssl_ctx_(boost::asio::ssl::context::tls_client),
      resolver_(ioc) {
    // 配置这个专用的 ssl_ctx_
    ssl_ctx_.set_options(
        boost::asio::ssl::context::default_workarounds |
        boost::asio::ssl::context::no_sslv2 |
        boost::asio::ssl::context::no_sslv3 |
        boost::asio::ssl::context::no_tlsv1 |
        boost::asio::ssl::context::no_tlsv1_1
    );

    // **[关键]** 只宣告支持 http/1.1，为 WebSocket Upgrade 做准备
    const unsigned char ws_protos[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    if (SSL_CTX_set_alpn_protos(ssl_ctx_.native_handle(), ws_protos, sizeof(ws_protos)) != 0) {
        // nghttp2 recommends checking for error
        throw std::runtime_error("Could not set ALPN protos for WebSocket");
    }

    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(boost::asio::ssl::verify_peer);
}

std::string base64_encode(const unsigned char* input, int length) {
    BIO* bmem = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL); // 不换行
    b64 = BIO_push(b64, bmem);
    BIO_write(b64, input, length);
    BIO_flush(b64);
    BUF_MEM* bptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string encoded(bptr->data, bptr->length);
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

boost::asio::awaitable<std::shared_ptr<WebSocketConnection>> WebSocketClient::connect(std::string_view url, std::shared_ptr<IWebSocketClientHandler> handler, const Headers& headers) {
    auto target = parse_url(url);
    bool use_ssl = (target.scheme == "wss:");
    if (!use_ssl && target.scheme != "ws:") {
        throw std::invalid_argument("无效的 WebSocket URL scheme");
    }

    using PlainStream = boost::beast::tcp_stream;
    using SslStream = boost::beast::ssl_stream<PlainStream>;
    using TransportStream = std::variant<std::shared_ptr<PlainStream>, std::shared_ptr<SslStream>>;
    using results_type = boost::asio::ip::tcp::resolver::results_type;

    TransportStream transport;

    // b. DNS 解析
    boost::system::error_code ec;
    results_type endpoints = co_await resolver_.async_resolve(
        boost::asio::ip::tcp::v4(),
        target.host,
        std::to_string(target.port),
        boost::asio::redirect_error(boost::asio::use_awaitable, ec)

    );
    if (ec) throw boost::system::system_error(ec, "async_resolve failed");


    if (use_ssl) {
        auto stream = std::make_shared<SslStream>(ioc_, ssl_ctx_);
        // 2) TCP 连接（迭代器重载 + redirect_error）
        ec.clear();
        co_await stream->next_layer().async_connect(
            endpoints.begin(),
            endpoints.end(),
            boost::asio::redirect_error(boost::asio::use_awaitable, ec)
        );
        if (ec) throw boost::system::system_error(ec, "tcp async_connect failed");

        std::string host_str(target.host);
        if (!SSL_set_tlsext_host_name(stream->native_handle(), host_str.c_str())) {
            throw boost::system::system_error(static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category());
        }

        // 3) TLS 握手（redirect_error）
        ec.clear();
        co_await stream->async_handshake(boost::asio::ssl::stream_base::client, boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec) throw boost::system::system_error(ec, "tls async_handshake failed");

        transport = std::move(stream);
    } else {
        auto stream = std::make_shared<PlainStream>(ioc_);
        // 2) TCP 连接（迭代器重载 + redirect_error）
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
    HttpRequest upgrade_req{http::verb::get, target.target.empty() ? "/" : target.target, 11};
    upgrade_req.set(http::field::host, target.host);
    upgrade_req.set(http::field::user_agent, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    upgrade_req.set(http::field::accept, "*/*");
    upgrade_req.set(http::field::accept_encoding, "gzip, deflate");
    upgrade_req.set(http::field::accept_language, "en-US,en;q=0.9");

    // WebSocket 专用头部
    upgrade_req.set(http::field::connection, "Upgrade");
    upgrade_req.set(http::field::upgrade, "websocket");
    upgrade_req.set(http::field::sec_websocket_version, "13");
    upgrade_req.set(http::field::sec_websocket_key, generate_websocket_key());
    // [可选但推荐] 声明支持 permessage-deflate 压缩扩展
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


    // 4. 启动它的 actor_loop
    ws_conn->run();

    // 6. 握手成功，返回可用的连接
    co_return ws_conn;
}

WebSocketClient::ParsedUrl WebSocketClient::parse_url(std::string_view url_strv) {
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
