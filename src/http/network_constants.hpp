//
// Created by Aiziboy on 2025/10/14.
//

#ifndef UNTITLED1_NETWORK_CONSTANTS_HPP
#define UNTITLED1_NETWORK_CONSTANTS_HPP
#include <boost/asio/ssl/context.hpp>
#include <spdlog/spdlog.h>


namespace network {
    namespace alpn {
        // 使用 inline constexpr 可以确保在多个 .cpp 文件中包含此头文件时
        // 不会发生“多重定义”链接错误，并且只在二进制文件中保留一份副本 (C++17+)。

        // 同时支持H1.1 H2
        inline constexpr unsigned char PROTOS_H2_PREFERRED[] = {
            2, 'h', '2',
            8, 'h', 't', 't', 'p', '/', '1', '.', '1'

        };

        // 只支持 HTTP/1.1
        inline constexpr unsigned char PROTOS_H1_ONLY[] = {
            8, 'h', 't', 't', 'p', '/', '1', '.', '1'
        };

    } // namespace alpn

    // --- TLS/SSL 相关常量 ---
    namespace ssl {

        /**
         * @brief 的现代加密套件列表 (Mozilla Intermediate a, TLS 1.2 & 1.3)。
         *
         * @note 这个列表是从 Mozilla Intermediate compatibility 推荐中提取的，@link https://wiki.mozilla.org/Security/Server_Side_TLS#:~:text=yet%20widely%20supported-,Intermediate%20compatibility%20(recommended),-For%20services%20that
         * 这个列表优先选择 TLS 1.3 的套件，然后是支持前向保密 (ECDHE/DHE) 且
         * 使用强加密算法 (AES-GCM, ChaCha20-Poly1305) 的 TLS 1.2 套件。
         * 它排除了所有已知的弱或过时的算法 (RSA key exchange, CBC mode, RC4, 3DES)。
         */
        inline constexpr auto CIPHER_SUITES =
            "TLS_AES_128_GCM_SHA256:"
            "TLS_AES_256_GCM_SHA384:"
            "TLS_CHACHA20_POLY1305_SHA256:"
            "ECDHE-ECDSA-AES128-GCM-SHA256:"
            "ECDHE-RSA-AES128-GCM-SHA256:"
            "ECDHE-ECDSA-AES256-GCM-SHA384:"
            "ECDHE-RSA-AES256-GCM-SHA384:"
            "ECDHE-ECDSA-CHACHA20-POLY1305:"
            "ECDHE-RSA-CHACHA20-POLY1305:"
            "DHE-RSA-AES128-GCM-SHA256:"
            "DHE-RSA-AES256-GCM-SHA384:"
            "DHE-RSA-CHACHA20-POLY1305";

        /**
         * @brief 现代 SSL/TLS 上下文选项。
         *
         * 这个组合禁用了所有已知的不安全协议版本 (SSLv2, SSLv3, TLSv1.0, TLSv1.1)，
         * 只允许使用安全的 TLSv1.2 和 TLSv1.3。
         * 同时，它也开启了 OpenSSL 的默认安全变通方案。
         */
        inline constexpr boost::asio::ssl::context::options CONTEXT_OPTIONS =
            // 使用 Boost.Asio 推荐的默认变通方法
            boost::asio::ssl::context::default_workarounds |
            // 禁用已被认为不安全的旧协议
            boost::asio::ssl::context::no_sslv2 |
            boost::asio::ssl::context::no_sslv3 |
            boost::asio::ssl::context::no_tlsv1 |
            boost::asio::ssl::context::no_tlsv1_1 |
            // 确保每次都使用新的 Diffie-Hellman 密钥，增强前向保密性
            boost::asio::ssl::context::single_dh_use;

    /**
     * @brief  根据配置动态设置 SSL_CTX 的最小和最大 TLS 协议版本。
     *
     * @param raw_ctx 指向 OpenSSL SSL_CTX 对象的裸指针。
     * @param enabled_versions 一个字符串向量，包含期望启用的 TLS 版本，
     *        例如 {"TLSv1.3", "TLSv1.2"}。如果为空，则使用默认值。
     *
     * @note 默认行为是同时启用 TLS 1.2 和 TLS 1.3。
     *       如果只想启用 TLS 1.3，请传入 {"TLSv1.3"}。
     *       如果只想启用 TLS 1.2，请传入 {"TLSv1.2"}。
     */
    inline void configure_tls_versions(SSL_CTX* raw_ctx, const std::vector<std::string>& enabled_versions) {
        // --- 1. 定义默认值 ---
        // 默认情况下，仅支持 TLS 1.3 。
        int min_version = TLS1_3_VERSION;
        int max_version = TLS1_3_VERSION;

        // --- 2. 如果配置非空，则根据配置调整版本范围 ---
        if (!enabled_versions.empty()) {
            // 使用 a simple bool flag 来跟踪版本是否存在
            bool tls1_2_enabled = false;
            bool tls1_3_enabled = false;

            for (const auto& version_str : enabled_versions) {
                if (version_str == "TLSv1.2") {
                    tls1_2_enabled = true;
                } else if (version_str == "TLSv1.3") {
                    tls1_3_enabled = true;
                }
            }

            if (tls1_3_enabled && tls1_2_enabled) {
                // 两者都启用，使用范围 (1.2 -> 1.3)
                min_version = TLS1_2_VERSION;
                max_version = TLS1_3_VERSION;
            } else if (tls1_3_enabled) {
                // 只启用 TLS 1.3
                min_version = TLS1_3_VERSION;
                max_version = TLS1_3_VERSION;
            } else if (tls1_2_enabled) {
                // 只启用 TLS 1.2
                min_version = TLS1_2_VERSION;
                max_version = TLS1_2_VERSION;
            } else {
                // 配置中包含了未知的版本字符串，或者配置为空列表后又被错误逻辑覆盖
                // 在这种情况下，为了安全，可以坚持使用最安全的默认值。
                // 或者抛出一个配置错误。这里坚持默认值。
                min_version = TLS1_3_VERSION;
                max_version = TLS1_3_VERSION;
                SPDLOG_WARN("识别到未知的 Tls version 字符串，仅支持：TLSv1.2 和 TLSv1.3 ");
            }
        }

        // --- 3. 应用最终的设置 ---
        // SSL_CTX_set_min_proto_version 返回 1 表示成功，0 表示失败
        if (SSL_CTX_set_min_proto_version(raw_ctx, min_version) != 1) {
            // 处理错误
            SPDLOG_ERROR("set min tls version failed");
        }
        if (SSL_CTX_set_max_proto_version(raw_ctx, max_version) != 1) {
            // 处理错误
            SPDLOG_ERROR("set max tls version failed");
            throw std::runtime_error("set max tls version failed");
        }
    }

    } // namespace tls

} // namespace network
#endif //UNTITLED1_NETWORK_CONSTANTS_HPP
