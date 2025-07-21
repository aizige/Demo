//
// Created by Aiziboy on 2025/7/14.
//

#ifndef CERTCHECKER_HPP
#define CERTCHECKER_HPP

#include <openssl/x509.h>    // OpenSSL 核心库，用于处理 X.509 证书结构
#include <openssl/pem.h>     // 用于读取 PEM 格式的证书文件
#include <openssl/x509v3.h>  // 用于处理 X.509 v3 扩展，特别是 SAN (Subject Alternative Name)
#include <string>
#include <vector>
#include <iostream>
#include <arpa/inet.h>       // 用于将二进制的 IP 地址转换为点分十进制字符串 (inet_ntop)
#include <algorithm>         // C++标准库，用于 std::find

/**
 * @class CertChecker
 * @brief 一个静态工具类，用于在服务器启动时检查和验证 SSL/TLS 证书。
 *
 * 这个类的主要目的是提供一个便捷的方式来加载一个 PEM 格式的证书文件，
 * 读取其关键信息（如主题、有效期、SAN条目），并验证它是否包含了所有
 * 预期的域名或 IP 地址。这在部署前进行配置检查时非常有用，可以防止
 * 因证书配置错误导致客户端连接失败。
 */
class CertChecker {
public:
    /**
     * @brief 检查指定的证书文件，并验证其 SAN (Subject Alternative Name) 列表。
     * @param cert_path PEM 格式的证书文件的路径。
     * @param required_hosts一个包含所有必须出现在证书 SAN 列表中的域名或 IP 地址的向量。
     *
     * 该函数会执行以下操作：
     * 1. 读取并解析 PEM 证书文件。
     * 2. 打印证书的主题（Subject）信息。
     * 3. 打印证书的有效期（Not Before, Not After）。
     * 4. 提取并打印证书中的所有 SAN 条目（包括 DNS 名称和 IP 地址）。
     * 5. 检查 `required_hosts` 中的每一个条目是否存在于从证书中找到的 SAN 列表中。
     * 6. 对检查结果进行打印，成功找到的条目会以 "✅" 标记，缺失的条目会以 "⚠️" 标记并打印错误信息。
     */
    static void inspect(const std::string& cert_path, const std::vector<std::string>& required_hosts) {
        // --- 1. 打开并读取证书文件 ---
        FILE* fp = fopen(cert_path.c_str(), "r");
        if (!fp) {
            std::cerr << "❌ 无法打开证书文件: " << cert_path << std::endl;
            return;
        }

        // 从文件中读取 PEM 格式的 X.509 证书
        X509* cert = PEM_read_X509(fp, nullptr, nullptr, nullptr);
        fclose(fp); // 文件指针用完后立即关闭
        if (!cert) {
            std::cerr << "❌ 解析证书失败。" << std::endl;
            return;
        }

        // --- 2. 打印基本信息 ---
        std::cout << "🔍 证书主题 (Subject): ";
        // 使用 OpenSSL 的函数以单行格式打印证书主题
        X509_NAME_print_ex_fp(stdout, X509_get_subject_name(cert), 0, XN_FLAG_ONELINE);
        std::cout << std::endl;

        std::cout << "📅 证书有效期:\n";
        // 使用 BIO (OpenSSL 的 I/O 抽象) 来打印 ASN.1 格式的时间
        BIO* out = BIO_new_fp(stdout, BIO_NOCLOSE); // BIO_NOCLOSE 表示 BIO_free 时不关闭底层的文件指针 (stdout)
        ASN1_TIME_print(out, X509_get_notBefore(cert)); std::cout << " → ";
        ASN1_TIME_print(out, X509_get_notAfter(cert)); std::cout << std::endl;
        BIO_free(out); // 释放 BIO 对象


        // --- 3. 提取并解析 SAN (Subject Alternative Name) 扩展 ---
        // 这是证书验证中至关重要的一步，因为现代浏览器和客户端主要依赖 SAN 来验证域名
        STACK_OF(GENERAL_NAME)* san_names = (STACK_OF(GENERAL_NAME)*) X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr);
        if (!san_names) {
            std::cerr << "⚠️ 未找到 SAN 扩展。" << std::endl;
            X509_free(cert); // 别忘了释放证书对象
            return;
        }

        std::vector<std::string> found_hosts; // 用于存储在证书中找到的所有域名和 IP
        std::cout << "🌐 SAN (主题备用名称) 列表:" << std::endl;
        // sk_GENERAL_NAME_num 获取 SAN 列表中的条目数量
        for (int i = 0; i < sk_GENERAL_NAME_num(san_names); ++i) {
            // sk_GENERAL_NAME_value 获取指定索引的条目
            const GENERAL_NAME* name = sk_GENERAL_NAME_value(san_names, i);

            // 判断 SAN 条目的类型
            if (name->type == GEN_DNS) { // 如果是 DNS 名称
                // 从 ASN1_STRING 中获取数据并转换为 std::string
                auto entry = std::string(reinterpret_cast<const char*>(ASN1_STRING_get0_data(name->d.dNSName)));
                found_hosts.push_back(entry);
                std::cout << "   - [DNS] " << entry << std::endl;
            }
            else if (name->type == GEN_IPADD) { // 如果是 IP 地址
                const unsigned char* ip_data = ASN1_STRING_get0_data(name->d.iPAddress);
                int ip_len = ASN1_STRING_length(name->d.iPAddress);

                char buf[INET6_ADDRSTRLEN]; // 足够存储 IPv4 或 IPv6 地址的缓冲区
                const char* ip_str = nullptr;

                if (ip_len == 4) { // IPv4
                    ip_str = inet_ntop(AF_INET, ip_data, buf, sizeof(buf));
                } else if (ip_len == 16) { // IPv6
                    ip_str = inet_ntop(AF_INET6, ip_data, buf, sizeof(buf));
                }

                if (ip_str) {
                    found_hosts.push_back(ip_str);
                    std::cout << "   - [IP]  " << ip_str << std::endl;
                } else {
                    std::cerr << "⚠️ 解析 SAN 中的 IP 地址失败。" << std::endl;
                }
            }
        }


        // --- 4. 验证 SAN 列表是否包含所有必需的条目 ---
        std::cout << "🛡️ 验证必需的 SAN 条目:" << std::endl;
        for (const auto& host : required_hosts) {
            // 使用 std::find 在找到的列表中搜索必需的 host
            if (std::find(found_hosts.begin(), found_hosts.end(), host) == found_hosts.end()) {
                // 如果没有找到，打印警告信息
                std::cerr << "   - ⚠️ 缺失: " << host << std::endl;
            } else {
                // 如果找到了，打印成功信息
                std::cout << "   - ✅ 包含: " << host << std::endl;
            }
        }

        // --- 5. 清理和释放资源 ---
        // 释放由 X509_get_ext_d2i 分配的 SAN 列表内存
        sk_GENERAL_NAME_pop_free(san_names, GENERAL_NAME_free);
        // 释放证书对象
        X509_free(cert);
    }
};

#endif //CERTCHECKER_HPP