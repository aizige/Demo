#ifndef SSL_CONTEXT_HPP
#define SSL_CONTEXT_HPP

#include <fstream>
#include <boost/asio/ssl.hpp> // Asio 的 SSL/TLS 功能
#include <openssl/ssl.h>      // 直接使用 OpenSSL 的底层 API 来设置 ALPN 回调
#include <string>


/**
 * @class SslContextManager
 * @brief 负责创建和管理服务器的 SSL/TLS 上下文 (SSL Context)。
 *
 * 负责 SSL/TLS 配置，是一个网络相关的工具
 *
 * SSL 上下文是一个包含了所有 TLS 相关配置（如证书、私钥、加密套件、协议版本等）
 * 的核心对象。在服务器启动时，我们创建一个全局的 SSL 上下文，之后所有新的
 * SSL 连接都会从这个上下文中派生，共享这些配置。
 *
 * 这个类将复杂的 SSL 配置细节封装起来，为 `Server` 类提供一个干净的接口。
 */
class SslContextManager {
public:
    /**
     * @brief 加载证书和私钥，并配置 SSL 上下文。
     * @param cert_file PEM 格式的证书链文件路径。
     * @param key_file PEM 格式的私钥文件路径。
     */
    void load(const std::string& cert_file, const std::string& key_file) {
        // 启动前检查文件是否存在，如果不存在则抛出异常，使服务器启动失败
        if (!std::ifstream(cert_file))
            throw std::runtime_error("未找到证书: " + cert_file);
        if (!std::ifstream(key_file))
            throw std::runtime_error("未找到证书密钥: " + key_file);

        // 创建一个用于服务器端的 SSL 上下文对象
        ctx_ = std::make_unique<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);

        // --- 设置安全选项 ---
        // 这是增强服务器安全性的重要步骤
        ctx_->set_options(
            // 使用 Boost.Asio 推荐的默认变通方法
            boost::asio::ssl::context::default_workarounds |
            // 禁用已被认为不安全的旧协议
            boost::asio::ssl::context::no_sslv2 |
            boost::asio::ssl::context::no_sslv3 |
            boost::asio::ssl::context::no_tlsv1 |
            boost::asio::ssl::context::no_tlsv1_1 | // 强制使用 TLS 1.2 或更高版本
            // 确保每次都使用新的 Diffie-Hellman 密钥，增强前向保密性
            boost::asio::ssl::context::single_dh_use
        );

        // --- 加载证书和私钥 ---
        ctx_->use_certificate_chain_file(cert_file);
        ctx_->use_private_key_file(key_file, boost::asio::ssl::context::pem);

        // 🔐 [可选但推荐] 加入额外的 TLS 安全选项
        // 禁止 TLS 会话重协商，这可以防止一种潜在的 DoS 攻击
        SSL_CTX_set_options(ctx_->native_handle(), SSL_OP_NO_RENEGOTIATION);

        // --- 设置 ALPN (Application-Layer Protocol Negotiation) 回调函数 ---
        // ALPN 是 TLS 握手期间的一个扩展，允许客户端和服务器协商接下来要使用的应用层协议
        // (例如，是使用 HTTP/2 还是 HTTP/1.1)。
        // 我们需要直接调用 OpenSSL 的底层 API 来设置这个回调。
        // `native_handle()` 返回底层的 `SSL_CTX*` 指针。
        SSL_CTX_set_alpn_select_cb(ctx_->native_handle(), alpn_select_callback, nullptr);
        SPDLOG_DEBUG("✅ ALPN selection callback set for server.");
    }

    /**
     * @brief 获取已配置好的 SSL 上下文对象的引用。
     * @return boost::asio::ssl::context&
     * @throws std::logic_error 如果上下文尚未通过 `load()` 方法加载。
     */
    boost::asio::ssl::context& context() {
        if (!ctx_) throw std::logic_error("SSL context not loaded");
        return *ctx_;
    }

private:
    // 使用 unique_ptr 来管理 SSL 上下文的生命周期，确保资源被自动释放
    std::unique_ptr<boost::asio::ssl::context> ctx_;

    /**
     * @brief 静态回调函数，用于在 TLS 握手期间选择一个应用层协议。
     * @param ssl OpenSSL 的 SSL 对象指针。
     * @param out 用于存放服务器选择的协议的指针。
     * @param outlen 用于存放服务器选择的协议的长度。
     * @param in 客户端提供的协议列表。
     * @param inlen 客户端协议列表的总长度。
     * @param arg 用户自定义参数（在此未使用）。
     * @return `SSL_TLSEXT_ERR_OK` 表示成功选择了一个协议，
     *         `SSL_TLSEXT_ERR_NOACK` 表示没有找到共同支持的协议。
     */
    static int alpn_select_callback(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                                    const unsigned char *in, unsigned int inlen, void *arg) {
        // 定义服务器支持的协议列表。
        // 格式是：[1字节长度][协议名称][1字节长度][协议名称]...
        // 这里的顺序很重要，表示服务器的偏好。我们优先选择 'h2' (HTTP/2)。
        const unsigned char supported_protos[] = {
            2, 'h', '2',                               // 2字节长的 "h2"
            8, 'h', 't', 't', 'p', '/', '1', '.', '1'  // 8字节长的 "http/1.1"
        };

        // 使用 OpenSSL 提供的辅助函数来从客户端的列表 (`in`) 和服务器的列表 (`supported_protos`) 中选择第一个共同支持的协议。
        if (SSL_select_next_proto((unsigned char **)out, outlen, supported_protos, sizeof(supported_protos), in, inlen) == OPENSSL_NPN_NEGOTIATED) {
            // OPENSSL_NPN_NEGOTIATED 表示协商成功
            SPDLOG_INFO("ALPN 协商结果: {}", std::string(reinterpret_cast<const char*>(*out), *outlen));

            return SSL_TLSEXT_ERR_OK; // 返回成功
        }

        // 如果没有找到共同的协议，打印警告并返回错误
        SPDLOG_WARN("ALPN: Could not find a common protocol.");
        return SSL_TLSEXT_ERR_NOACK;
    }
};

#endif // SSL_CONTEXT_HPP