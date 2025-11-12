//
// Created by Aiziboy on 2025/10/1.
//

#ifndef UNTITLED1_CONFIG_HPP
#define UNTITLED1_CONFIG_HPP

#include <string>
#include <vector>
#include <optional>
#include <cstdint> // For specific integer types like uint16_t, uint32_t
#include <thread>
#include <unordered_map>

// ------------------------------------------------
// [logging]
// ------------------------------------------------
struct LoggingConfig {
    ///  日志级别，如 "debug", "info", "warn", "error"
    std::string level = "debug";
    /// 日志输出位置，如 "console", "file", "all"
    std::string output_type = "console";
    /// 指定日志文件文件路径
    std::string file_path = "/var/logs/aizix/log.txt";
    /// 日志轮转配置 : 防止日志文件无限增大。单个日志文件的最大大小（MB）
    uint16_t max_size_mb = 5;
    /// 日志轮转配置 : 防止日志文件无限增大。日志文件轮转数量
    uint16_t max_files = 50;
};

// ------------------------------------------------
// [app]
// ------------------------------------------------
struct AppConfig {
    /// 应用名称
    std::string name = "Aizix";


};

// ------------------------------------------------
// [server] & [server.ssl]
// ------------------------------------------------
struct ServerSslConfig {
    /// 是否启用HTTPS。默认false
    bool enabled = false;
    /// SSL证书文件的路径。
    std::optional<std::string> cert;
    /// SSL私钥文件的路径
    std::optional<std::string> cert_private_key;
    /// Tls版本范围。tls_version数组最大size为2。默认值：TLSv1.3
    /// @note TLS版本范围示例：{"TLSv1.3","TLSv1.2"}(value顺序不重要)这将支持TLSv1.2、TLSv1.3。{"TLSv1.3"}：将只支持TLSv1.3
    std::vector<std::string> tls_versions = {"TLSv1.3", "TLSv1.3"};
};

struct ServerConfig {
    /// server监听断开. 默认值：8080
    uint16_t port = 8080;
    /// IP. 默认值：0.0.0.0
    std::string ip_v4 = "0.0.0.0";
    /// 是否开启HTTP2。默认false
    bool http2_enabled = false;
    /// io线程自动计算的成数 ( 例如 0.5 (50%) )就是IO线程占所有核心的一半，默认值0.4
    float io_thread_ratio = 0.4;
    /// 专门用于I/O的线程数. 默认值：总核心数 * io_thread_ratio，当只有一个核心的时候此值默认为1
    uint16_t io_threads = 0;
    /// 专门用于执行CPU耗时计算的线程数. 默认值：总核心数减I/O的线程数，当只有一个核心的时候此值默认为1
    uint16_t worker_threads = 0;
    /// keep alive超时时间. 默认值：180000ms(3分钟)
    uint32_t keep_alive_ms = 180000;
    /// 请求体最大大小. 默认值：1048576 bytes(1MB)
    uint32_t max_request_size_bytes = 1024 * 1024;
    /// http server ssl配置
    std::optional<ServerSslConfig> ssl;
};

// ------------------------------------------------
// [client]
// ------------------------------------------------
struct ClientConfig {
    /// 是否开启HTTP2。默认false
    bool http2_enabled = false;
    /// 是否验证证书，默认值：false
    bool ssl_verify = false;
    /// 最大重定向次数.默认3，(0则关闭重定向)
    uint8_t max_redirects = 3;
    /// 连接建立超时TCP + TLS 握手阶段的最大等待时间，默认值：1500ms
    uint32_t connect_timeout_ms = 1500;
    /// 连接池维护间隔，默认值：5,000 ms (5秒)
    /// @note 必须是三个值中最小的。这个值决定了你的维护操作的“精度”或“反应时间”。<br>
    /// <br>举个例子：如果一个连接在 T=0 时变为空闲，你的 ping 阈值是15秒。如果巡逻间隔是5秒，那么巡逻队会在 T=5, T=10, T=15 时过来检查。在 T=15 这次检查时，它会发现 15 - 0 >= 15，于是触发 PING。实际的 PING 时间在15秒到20秒之间，这是完全可以接受的。<br>
    /// 如果巡逻间隔太长，比如10秒，那么 PING 可能会在15秒到25秒之间才被触发，精度就变差了。<br>
    /// 如果巡逻间隔太短，比如100毫秒，那么会频繁唤醒协程，即使什么都不需要做，也会带来微小的CPU开销。5秒是一个非常经典的折中值，既保证了足够的响应速度，又不会给系统带来负担。<br>
    uint32_t maintenance_interval_ms = 5000;
    /// 空闲连接关闭时间 控制连接在无活动时多久关闭，默认值：300,000ms(5分钟)
    /// @note 必须留出安全边际小于服务器的keep alive时间！ 客户端和服务器之间可能存在网络延迟、时钟不完全同步等问题。<br>
    ///       如果客户端在第59.9秒时认为连接是好的，并发起一个请求，但服务器恰好在第60秒关闭了连接，请求就会失败（通常是 "Connection reset by peer" 错误）。
    uint32_t idle_timeout_for_close_ms = 3000000;
    /// PING 这个值决定了客户端在连接空闲多久后，会主动发送一个 PING帧或者head请求来确认连接是否还活着。默认值：15,000 ms (15秒)
    /// @note 必须远小于 idle_timeout_for_close_ms_ 。PING 的目的是为了“保活”和“探测”，它必须在连接被判定为“太老该关闭”之前发生。15秒远小于45秒，逻辑上是正确的。<br>
    ///       可以对抗网络中间设备（NAT/防火墙）。很多网络设备会对空闲的TCP连接进行超时清理，这个超时时间通常在30-300秒之间。每15秒发送一次 PING，对于绝大多数网络环境来说，都足以刷新这些设备的超时计时器，让连接“看起来”一直很活跃，从而避免被意外断开。
    uint32_t idle_timeout_for_ping_ms = 15000;
    /// 单个个Host的最大连接池大小，默认值：100
    uint32_t host_connection_pool_size = 100;
    /// 每个Host的最大并发连接数，默认值：100
    uint32_t max_connections_per_host = 100;
    /// HTTP2初始最大并发流数	控制每个连接允许的最大并发请求数（默认值 100）
    /// @note 当自定义最大并发流数大于服务器支持的最大并发流时，重置为服务器的最大并发流数
    uint32_t http2_max_concurrent_streams = 100;
};

// ------------------------------------------------
// [database]
// ------------------------------------------------

// --- PostgreSQL ---
struct PostgresSslConfig {
    /// 是否启用SSL
    bool ssl_enabled = false;
    /// SSL模式。默认值：disable
    /// @note 可选: disable, require, verify-ca, verify-full
    std::string ssl_mode = "disable";
    /// SSL证书文件的路径。
    std::optional<std::string> cert;
    /// SSL私钥文件的路径
    std::optional<std::string> cert_private_key;
};

struct PostgresPoolConfig {
    /// 最大连接数. 默认值：16
    uint16_t max_connections = 16;
    /// 最小连接数. 默认值：1
    uint16_t min_connections = 1;
    /// 空闲连接超时时间. 默认值：30000ms
    uint16_t idle_timeout_ms = 30000;

};

struct PostgresConfig {
    /// 数据源配置名称。默认：default
    /// @note 当有多个数据源的时候此值不能重复
    std::string name = "default";
    std::string host;
    uint16_t port = 5432;
    std::string user;
    std::string password;
    std::string database = "postgres";
    /// 获取连接超时时间. 默认值：5000ms
    uint16_t connection_timeout_ms = 5000;

    std::optional<PostgresSslConfig> ssl;
    std::optional<PostgresPoolConfig> pool;
};

// --- Redis ---
struct RedisSslConfig {
    /// 是否启用SSL
    bool ssl_enabled = false;
    /// SSL证书文件的路径。
    std::optional<std::string> cert;
    /// SSL私钥文件的路径
    std::optional<std::string> cert_private_key;
};

struct RedisPoolConfig {
    /// 最大连接数. 默认值：16
    uint16_t max_connections = 16;
    /// 最小连接数. 默认值：1
    uint16_t min_connections = 1;
    /// 空闲连接超时时间. 默认值：30000ms
    uint16_t idle_timeout_ms = 30000;

};

struct RedisConfig {
    /// 数据源配置名称。默认：default
    /// @note 当有多个数据源的时候此值不能重复
    std::string name = "default";
    std::string host;
    uint16_t port = 6379;
    uint16_t database = 0;
    std::string password;
    /// 获取连接超时时间. 默认值：5000ms
    uint16_t connection_timeout_ms = 5000;

    std::optional<RedisPoolConfig> pool;
    std::optional<RedisSslConfig> ssl;
};

// --- Clickhouse ---
struct ClickhouseSslConfig {
    /// 是否启用SSL
    bool ssl_enabled = false;
    /// SSL证书文件的路径。
    std::optional<std::string> cert;
    /// SSL私钥文件的路径
    std::optional<std::string> cert_private_key;
};

struct ClickhousePoolConfig {
    /// 最大连接数. 默认值：16
    uint16_t max_connections = 16;
    /// 最小连接数. 默认值：1
    uint16_t min_connections = 1;
    /// 空闲连接超时时间. 默认值：30000ms
    uint16_t idle_timeout_ms = 30000;

};

struct ClickhouseConfig {
    /// 数据源配置名称。默认：default
    /// @note 当有多个数据源的时候此值不能重复
    std::string name = "default";
    std::string host;
    uint16_t port = 8123;
    std::string database = "default";
    std::string user;
    std::string password;
    /// 获取连接超时时间. 默认值：5000ms
    uint16_t connection_timeout_ms = 5000;

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
struct AizixConfig {
    AppConfig app;
    ServerConfig server;
    ClientConfig client;
    DatabaseConfig database;
    LoggingConfig logging;

    /**

    @brief 返回配置对象的字符串表示（格式化的JSON）。

    @return std::string 包含配置信息的字符串。
    */
    std::string toString() const;
};
#endif //UNTITLED1_CONFIG_HPP
