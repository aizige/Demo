//
// Created by Aiziboy on 2025/10/1.
//

#ifndef UNTITLED1_CONFIG_HPP
#define UNTITLED1_CONFIG_HPP

#include <string>
#include <vector>
#include <optional>
#include <cstdint> // For specific integer types like uint16_t, uint32_t
#include <unordered_map>

// ------------------------------------------------
// [logging]
// ------------------------------------------------
struct LoggingConfig {
    ///  日志级别，如 "debug", "info", "warn", "error"
    std::optional<std::string> level = "debug";
    /// 日志输出位置，如 "console", "file", "all"
    std::optional<std::string> output = "console";
    /// 指定日志文件文件路径
    std::optional<std::string> file_path;
    /// 日志轮转配置 : 防止日志文件无限增大。单个日志文件的最大大小（MB）
    std::optional<uint16_t> max_size_mb;
    /// 日志轮转配置 : 防止日志文件无限增大。日志文件轮转数量
    std::optional<uint16_t> max_files;
};

// ------------------------------------------------
// [app]
// ------------------------------------------------
struct AppConfig {
    /// 应用名称
    std::optional<std::string> name = "MyWebFrameworkApp";
    /// 应用版本
    std::optional<std::string> version = "1.0.0";
    /// 指定配置文件，这样能动态加载指定配置文件，例如这里填写dev，程序会在同目录下加载config-dev.toml这个配置文件
    std::optional<std::string> profiles = "dev";
};

// ------------------------------------------------
// [server] & [server.ssl]
// ------------------------------------------------
struct ServerSslConfig {
    /// 是否启用HTTPS。默认false
    std::optional<bool> enabled = false;
    /// Tls版本范围。tls_version数组最大size为2。
    /// @note TLS版本范围示例：{"TLSv1.3","TLSv1.2"}这将支持TLSv1.2、TLSv1.3。{"TLSv1.3"}：将只支持TLSv1.3
    std::vector<std::string> tls_versions = {"TLSv1.3", "TLSv1.2"};
    /// SSL证书文件的路径。
    std::optional<std::string> certificate_file;
    /// SSL私钥文件的路径
    std::optional<std::string> certificate_key_file;
    /// 填写证书所对应的域名及IP列表，就能帮你检查证书文件，并验证其 SAN (Subject Alternative Name) 列表
    std::vector<std::string> required_hosts_list;
};

struct ServerConfig {
    /// server监听断开
    std::optional<uint16_t> port;
    /// 是否开启HTTP2。默认false
    std::optional<bool> http2_enabled = false;
    /// I/O的线程数
    std::optional<uint16_t> io_threads;
    /// 工作线程用于CPU密集计算
    std::optional<uint16_t> threads;
    /// 新连接的第一个请求超时时间
    std::optional<uint32_t> timeout_ms = 5000;
    std::optional<uint32_t> keep_alive_timeout_ms = 5000;
    /// 请求体最大大小
    std::optional<uint32_t> max_request_size_kb = 1024;
    /// http server ssl配置
    std::optional<ServerSslConfig> ssl;
};

// ------------------------------------------------
// [client]
// ------------------------------------------------
struct ClientConfig {
    /// 是否开启HTTP2。默认false
    std::optional<bool> http2_enabled = false;
    /// 是否验证证书
    std::optional<bool> ssl_verify = true;
    /// 最大重定向次数.默认3，(0则关闭重定向)
    std::optional<uint16_t> max_redirects_ = 3;
    /// 连接建立超时	TCP + TLS 握手阶段的最大等待时间（如 1500ms）
    std::optional<uint32_t> connect_timeout_ms = 1500;
    /// 空闲连接关闭时间 控制连接在无活动时多久关闭（如5分钟 300,000ms ）
    std::optional<uint32_t> idle_connection_timeout_ms = 3000000;
    /// PING 保活间隔	定期发送 PING 帧检测连接是否挂起（如 20000ms）
    std::optional<uint32_t> ping_interval_ms = 20000;
    /// 单个个Host的最大连接池大小
    std::optional<uint32_t> host_connection_pool_size = 100;
    /// 每个Host的最大并发连接数
    std::optional<uint32_t> max_connections_per_host = 100;
    /// 请求发送阶段的超时（防止actor未唤醒）
    std::optional<uint32_t> http2_request_send_timeout_ms = 300;
    /// 请求响应阶段的超时（防止连接挂起）
    std::optional<uint32_t> http2_response_recv_timeout_ms = 3000;
    /// HTTP2初始最大并发流数	控制每个连接允许的最大并发请求数（如 100），这个一般是设置服务器 SETTINGS 帧中获取的值
    /// @note 当自定义最大并发流数大于服务器支持的最大并发流时，重置为服务器的最大并发流数
    std::optional<uint32_t> http2_max_streams_per_connection = 100;
};

// ------------------------------------------------
// [database]
// ------------------------------------------------

// --- PostgreSQL ---
struct PostgresSslConfig {
    /// 是否启用SSL
    std::optional<bool> ssl_enabled = false;
    /// SSL模式。可选: disable, require, verify-ca, verify-full
    std::optional<std::string> ssl_mode = "require";

    std::optional<std::string> sslcert;
    std::optional<std::string> sslkey;
    std::optional<std::string> sslrootcert;
};

struct PostgresPoolConfig {
    std::optional<uint16_t> max_connections = 32;
    std::optional<uint16_t> min_connections = 4;
    std::optional<uint16_t> idle_timeout_ms = 30000;
    std::optional<uint16_t> connection_timeout_ms = 5000;
};

struct PostgresConfig {
    std::optional<std::string> name = "default";
    std::optional<std::string> host = "localhost";
    std::optional<uint16_t> port = 5432;
    std::optional<std::string> user;
    std::optional<std::string> password;
    std::optional<std::string> database;
    std::optional<PostgresSslConfig> ssl;
    std::optional<PostgresPoolConfig> pool;
};

// --- Redis ---
struct RedisSslConfig {
    std::optional<bool> ssl_enabled = false;
    std::optional<std::string> ca_file;
    std::optional<std::string> client_cert_file;
    std::optional<std::string> client_key_file;
};

struct RedisPoolConfig {
    std::optional<uint16_t> pool_size = 10;
    std::optional<uint32_t> max_wait_ms = 3000;
    std::optional<uint32_t> idle_timeout_ms = 300000;
    std::optional<uint32_t> health_check_interval_ms = 60000;
};

struct RedisConfig {
    std::optional<std::string> name = "default";
    std::optional<std::string> host = "127.0.0.1";
    std::optional<uint16_t> port = 6379;
    std::optional<uint16_t> database = 0;
    std::optional<std::string> password;
    std::optional<uint32_t> timeout_ms = 100;
    std::optional<uint32_t> keep_alive_ms = 5000;
    std::optional<RedisPoolConfig> pool;
    std::optional<RedisSslConfig> ssl;
};

// --- Clickhouse ---
struct ClickhouseSslConfig {
    std::optional<bool> enabled = false;
    std::optional<std::string> client_cert_file;
    std::optional<std::string> client_key_file;
    std::optional<std::string> ca_cert_file;
    std::optional<bool> verify_peer = true;
    std::optional<bool> verify_hostname = true;
};

struct ClickhousePoolConfig {
    std::optional<uint16_t> max_connections = 16;
    std::optional<uint16_t> min_connections = 2;
    std::optional<uint16_t> idle_timeout_ms = 30000;
    std::optional<uint16_t> connection_timeout_ms = 5000;
};

struct ClickhouseConfig {
    std::optional<std::string> name = "default";
    std::optional<std::string> host = "127.0.0.1";
    std::optional<uint16_t> port = 9440;
    std::optional<std::string> database = "default";
    std::optional<std::string> user = "default";
    std::optional<std::string> password;
    std::optional<ClickhouseSslConfig> ssl;
    std::optional<ClickhousePoolConfig> pool;
};


// --- 主数据库配置 ---
struct DatabaseConfig {
    std::unordered_map<std::string, PostgresConfig> postgresql;
    std::unordered_map<std::string, RedisConfig> redis;
    std::unordered_map<std::string, ClickhouseConfig> clickhouse;
};


// ------------------------------------------------
// 根配置结构体
// ------------------------------------------------
struct Config {
    AppConfig app;
    ServerConfig server;
    ClientConfig client;
    DatabaseConfig database;
    LoggingConfig logging;

    /**
     * @brief 从指定的 TOML 文件加载配置。
     * @param file_path 配置文件路径。
     * @return 一个填充了配置的 Config 对象。
     * @throws toml::parse_error 如果文件解析失败。
     * @throws std::runtime_error 如果配置内容不合法（如缺少 name）。
     */
    static Config load(const std::string& file_path);
};
#endif //UNTITLED1_CONFIG_HPP
