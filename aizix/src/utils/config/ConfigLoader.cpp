//
// Created by Aiziboy on 2025/10/15.
//

#include <aizix/utils/config/ConfigLoader.hpp>
#include <aizix/lib/toml.hpp>

#include <stdexcept>
#include <iostream>
#include <ranges>
#include <thread>
#include <filesystem>
#include <spdlog/spdlog.h>



void finalize_config(AizixConfig& config) {
    // 自动配置线程数
    // 如果用户在 toml 文件中没有设置，或者显式设置为 0，则进行自动计算
    if (config.server.io_threads == 0 || config.server.worker_threads == 0) {
        /// 这里的逻辑显示，在核心数极少（1或2）的情况下，为了保证 io 和 worker 至少各有1个线程，我们不得不用满所有核心。这是完全合理的，因为在这么低配的环境下，性能本身就不是首要考虑，保证程序能运行才是。当核心数 > 2 时，保留一个核心的策略就能完美生效。
        uint16_t total_cores = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 1;// 避免除以0

        // 引入“可分配核心数”的概念，为系统保留一个核心。
        // 如果总核心数大于1，就保留一个；否则，只能用这一个。
        const uint16_t allocatable_cores = (total_cores > 1) ? (total_cores - 1) : 1;

        uint16_t final_io_threads = config.server.io_threads;
        uint16_t final_worker_threads = config.server.worker_threads;

        // --- 第一步：计算 IO 线程数 ---
        // 只有当用户没有配置（即为0）时，我们才计算它
        if (final_io_threads == 0) {
            // 1. 使用浮点数进行计算，以支持百分比这样的比例
            const float desired_io_threads = static_cast<float>(allocatable_cores) * config.server.io_thread_ratio;
            //std::cerr << "desired_io_threads ==== '" << desired_io_threads << "':\n" << std::endl;
            // 2.直接转换为整数，实现向下取整 (截断)
            const auto calculated_io = static_cast<uint16_t>(desired_io_threads);
            //std::cerr << "calculated_io ==== '" << calculated_io << "':\n" << std::endl;

            // 3. 专门用于I/O的线程数.
            final_io_threads = allocatable_cores > 1 ? calculated_io : 1;
            if (final_io_threads == 0) final_io_threads = 1; // 默认值：总核心数 ÷ 2，当只有一个核心的时候此值默认为1
        }

        // --- 第二步：计算 Worker 线程数 ---
        // 同样，只有当用户没有配置时才计算
        if (final_worker_threads == 0) {
            /// 专门用于执行CPU耗时计算的线程数. 如果总核心数大于已确定的IO线程数，则用剩余的，否则至少保证1个
            final_worker_threads = allocatable_cores > final_io_threads ? allocatable_cores - final_io_threads : 1;

            // 再次确保 worker 线程至少为 1
            if (final_worker_threads == 0) final_worker_threads = 1;
            // final_worker_threads = std::max(static_cast<uint16_t>(1), final_worker_threads); 等效if (final_worker_threads == 0) final_worker_threads = 1; 但是这样写有点反直觉
        }

        // --- 第三步：最终赋值 ---
        // 将计算出的最终值赋回 config 对象
        config.server.io_threads = final_io_threads;
        config.server.worker_threads = final_worker_threads;
    }

    // 添加一个警告，用于检查用户手动配置是否合理
    if (const uint16_t total_cores = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 1; config.server.io_threads + config.server.worker_threads >= total_cores) {
        // 使用你的日志系统来打印一个警告
         SPDLOG_WARN("线程配置警告：io_threads ({}) 和 worker_threads ({}) 的总和为 {}，等于或大于可用核心总数 ({})。这可能会导致系统在高负载下不稳定。",
                     config.server.io_threads, config.server.worker_threads,
                     config.server.io_threads + config.server.worker_threads, total_cores);
    }


    // HTTP2协议检测
    if (config.server.http2_enabled && !config.server.ssl->enabled) {
        // 当[server.ssl]不存在或者没开启ssl的时候抛出异常
        if (!config.server.ssl || !config.server.ssl->enabled) {
            throw std::runtime_error("server config 缺少必填字段 'server.ssl.enabled'.");
        }
    }

    // ssl配置检测
    if (config.server.ssl && config.server.ssl->enabled) {
        //std::cerr << std::filesystem::current_path().string() << std::endl;
        if (const auto certFilepath = config.server.ssl->cert) {
            if (!std::ifstream(certFilepath.value())) {

                throw std::runtime_error("未找到server证书文件: " + certFilepath.value());
            }
        } else {
            throw std::runtime_error("server.ssl config  is missing required field 'ssl.cert'.");
        }

        if (const auto certPrivateKeyFilepath = config.server.ssl->cert_private_key) {
            if (!std::ifstream(certPrivateKeyFilepath.value())) {
                throw std::runtime_error("未找到server证书密钥文件: " + certPrivateKeyFilepath.value());
            }
        } else {
            throw std::runtime_error("server.ssl config  is missing required field 'ssl.cert_private_key'.");
        }

        for (const auto& version_str : config.server.ssl->tls_versions) {
            if (version_str != "TLSv1.3" && version_str != "TLSv1.2") {
                throw std::runtime_error("server.ssl.protocols config field has an incorrect value  '" + version_str + "' is not a valid TLS version. Only TLSv1.3 or TLSv1.2 is supported appears");
            }
        }

    }

    // 数据库ssl配置检测
    for (const auto& clickhouseConfig : config.database.clickhouse | std::views::values) {
        if (clickhouseConfig.ssl && clickhouseConfig.ssl->ssl_enabled) {
            if (const auto certFilepath = clickhouseConfig.ssl->cert) {
                if (!std::ifstream(certFilepath.value())) {
                    throw std::runtime_error("未找到clickhouse证书文件: " + certFilepath.value());
                }
            } else {
                throw std::runtime_error("Clickhouse config '" + clickhouseConfig.name + "' is missing required field 'ssl.cert'.");
            }

            if (const auto certPrivateKeyFilepath = clickhouseConfig.ssl->cert_private_key) {
                if (!std::ifstream(certPrivateKeyFilepath.value())) {
                    throw std::runtime_error("未找到clickhouse证书密钥文件: " + certPrivateKeyFilepath.value());
                }
            } else {
                throw std::runtime_error("Clickhouse config '" + clickhouseConfig.name + "' is missing required field 'ssl.cert_private_key'.");
            }
        }
    }

    for (const auto& postgres_config : config.database.postgresql | std::views::values) {
        if (postgres_config.ssl && postgres_config.ssl->ssl_enabled) {
            if (const auto certFilepath = postgres_config.ssl->cert) {
                if (!std::ifstream(certFilepath.value())) {
                    throw std::runtime_error("未找到postgresql证书文件: " + certFilepath.value());
                }
            } else {
                throw std::runtime_error("PostgreSQL config '" + postgres_config.name + "' is missing required field 'ssl.cert'.");
            }

            if (const auto certPrivateKeyFilepath = postgres_config.ssl->cert_private_key) {
                if (!std::ifstream(certPrivateKeyFilepath.value())) {
                    throw std::runtime_error("未找到postgresql证书密钥文件: " + certPrivateKeyFilepath.value());
                }
            } else {
                throw std::runtime_error("PostgreSQL config '" + postgres_config.name + "' is missing required field 'ssl.cert_private_key'.");
            }
        }
    }

    for (const auto& redis_config : config.database.redis | std::views::values) {
        if (redis_config.ssl && redis_config.ssl->ssl_enabled) {
            if (const auto certFilepath = redis_config.ssl->cert) {
                if (!std::ifstream(certFilepath.value())) {
                    throw std::runtime_error("未找到redis证书文件: " + certFilepath.value());
                }
            } else {
                throw std::runtime_error("Redis config '" + redis_config.name + "' is missing required field 'ssl.cert'.");
            }

            if (const auto certPrivateKeyFilepath = redis_config.ssl->cert_private_key) {
                if (!std::ifstream(certPrivateKeyFilepath.value())) {
                    throw std::runtime_error("未找到redis证书密钥文件: " + certPrivateKeyFilepath.value());
                }
            } else {
                throw std::runtime_error("Redis config '" + redis_config.name + "' is missing required field 'ssl.cert_private_key'.");
            }
        }
    }
}

// --- 主加载函数 ---
AizixConfig ConfigLoader::load(const std::string& filepath) {
    try {
        toml::table root_tbl = toml::parse_file(filepath);

        if (const auto value_op = root_tbl["active_profile"].value<std::string>()) {
            if (*value_op != "dev" && *value_op != "prod" && *value_op != "test") {
                throw std::runtime_error("无法识别的配置文件类型[ " + *value_op + " ]");
            }
            const std::filesystem::path path = filepath;
            path.stem() = path.stem().string() + "-" + *value_op;
            const std::filesystem::path new_path = path.parent_path() / (path.stem().string() + "-" + *value_op + path.extension().string());
            // 提取部分
            // std::cout << "父目录: " << path.parent_path() << '\n'; // /home/user/project/
            // std::cout << "文件名（无后缀）: " << path.stem() << '\n'; // config
            // std::cout << "后缀名: " << path.extension() << '\n'; // .toml
            // std::cout << "完整文件名: " << path.filename() << '\n'; // config.toml
            root_tbl = toml::parse_file(new_path.string());
        }

        AizixConfig config;
        config.app = parse_app(root_tbl);
        config.server = parse_server(root_tbl);
        config.client = parse_client(root_tbl);
        config.database = parse_database(root_tbl);
        config.logging = parse_logging(root_tbl);

        finalize_config(config);
        return config;
    } catch (const toml::parse_error& err) {
        std::cerr << "Error parsing config file '" << filepath << "':\n" << err << std::endl;
        throw std::runtime_error(err.what());
    }
}

// --- 私有帮助函数实现 ---

AppConfig ConfigLoader::parse_app(const toml::table& app_tb) {
    AppConfig appConfig;
    if (const auto table = app_tb["app"].as_table()) {
        appConfig.name = (*table)["name"].value_or(appConfig.name);
    }
    return appConfig;
}

LoggingConfig ConfigLoader::parse_logging(const toml::table& log_tb) {
    LoggingConfig logConfig;
    if (const auto table = log_tb["logging"].as_table()) {
        logConfig.level = (*table)["level"].value_or(logConfig.level);
        logConfig.output_type = (*table)["output_type"].value_or(logConfig.output_type);
        logConfig.file_path = (*table)["file_path"].value_or(logConfig.file_path);
        logConfig.max_size_mb = (*table)["max_size_mb"].value_or(logConfig.max_size_mb);
        logConfig.max_files = (*table)["max_files"].value_or(logConfig.max_files);
    }
    return logConfig;
}

ServerConfig ConfigLoader::parse_server(const toml::table& server_tb) {
    ServerConfig serverConfig;
    if (auto server_tbl = server_tb["server"].as_table()) {
        serverConfig.port = (*server_tbl)["port"].value_or(serverConfig.port);
        serverConfig.ip_v4 = (*server_tbl)["ip_v4"].value_or(serverConfig.ip_v4);
        serverConfig.http2_enabled = (*server_tbl)["http2_enabled"].value_or(serverConfig.http2_enabled);
        serverConfig.io_thread_ratio = (*server_tbl)["io_thread_ratio"].value_or(serverConfig.io_thread_ratio);
        serverConfig.io_threads = (*server_tbl)["io_threads"].value_or(serverConfig.io_threads);
        serverConfig.worker_threads = (*server_tbl)["worker_threads"].value_or(serverConfig.worker_threads);
        serverConfig.keep_alive_ms = (*server_tbl)["keep_alive_ms"].value_or(serverConfig.keep_alive_ms);
        serverConfig.max_request_size_bytes = (*server_tbl)["max_request_size_kb"].value_or(serverConfig.max_request_size_bytes);


        if (const auto ssl_tbl = server_tbl->get("ssl")->as_table()) {
            ServerSslConfig serverSSLConfig;
            serverSSLConfig.enabled = (*ssl_tbl)["enabled"].value_or(serverSSLConfig.enabled);

            serverSSLConfig.cert = (*ssl_tbl)["cert"].value<std::string>();
            serverSSLConfig.cert_private_key = (*ssl_tbl)["cert_private_key"].value<std::string>();

            if (const auto versions = ssl_tbl->get("tls_versions")->as_array()) {
                serverSSLConfig.tls_versions.clear();
                for (const auto& elem : *versions) {
                    if (auto str = elem.value<std::string>()) {
                        serverSSLConfig.tls_versions.push_back(*str);
                    }
                }
            }

            serverConfig.ssl = serverSSLConfig;
        }
    }
    return serverConfig;
}

ClientConfig ConfigLoader::parse_client(const toml::table& client_tb) {
    ClientConfig cfg;
    if (const auto client_tbl = client_tb["client"].as_table()) {
        // 使用 value_or 填充每个字段
        cfg.http2_enabled = (*client_tbl)["http2_enabled"].value_or(cfg.http2_enabled);
        cfg.ssl_verify = (*client_tbl)["ssl_verify"].value_or(cfg.ssl_verify);
        cfg.max_redirects = (*client_tbl)["max_redirects"].value_or(cfg.max_redirects);
        cfg.connect_timeout_ms = (*client_tbl)["connect_timeout_ms"].value_or(cfg.connect_timeout_ms);
        cfg.protocol_cache_ttl_ms = (*client_tbl)["protocol_cache_ttl_ms"].value_or(cfg.protocol_cache_ttl_ms);
        cfg.idle_timeout_for_close_ms = (*client_tbl)["idle_timeout_for_close_ms"].value_or(cfg.idle_timeout_for_close_ms);
        cfg.idle_timeout_for_ping_ms = (*client_tbl)["idle_timeout_for_ping_ms"].value_or(cfg.idle_timeout_for_ping_ms);
        cfg.max_h1_connections_per_host = (*client_tbl)["max_h1_connections_per_host"].value_or(cfg.max_h1_connections_per_host);
        cfg.max_h2_connections_per_host = (*client_tbl)["max_h2_connections_per_host"].value_or(cfg.max_h2_connections_per_host);
        cfg.http2_max_concurrent_streams = (*client_tbl)["http2_max_concurrent_streams"].value_or(cfg.http2_max_concurrent_streams);
    }
    return cfg;
}


DatabaseConfig ConfigLoader::parse_database(const toml::table& db_tb) {
    DatabaseConfig cfg;
    const auto db_tbl_ptr = db_tb["database"].as_table();
    if (!db_tbl_ptr) {
        return cfg; // No [database] section
    }
    const auto& db_tbl = *db_tbl_ptr;

    // --- 核心逻辑：处理 PostgreSQL 单表或表数组 ---
    if (const auto pg_node = db_tbl["postgresql"]) {
        // 尝试作为数组解析
        if (const auto pg_array = pg_node.as_array()) {
            for (const auto& elem : *pg_array) {
                if (const auto tbl = elem.as_table()) {
                    auto postgres_from_table = parse_postgres_from_table(*tbl);
                    cfg.postgresql[postgres_from_table.name] = postgres_from_table;
                }
            }
        }
        // 尝试作为单个表解析
        else if (const auto pg_table = pg_node.as_table()) {
            const auto postgres_config = parse_postgres_from_table(*pg_table);
            cfg.postgresql[postgres_config.name] = postgres_config;
        }
    }

    // --- 解析 Redis 和 ClickHouse ---
    if (const auto ck_node = db_tbl["clickhouse"]) {
        // 尝试作为数组解析
        if (const auto ck_array = ck_node.as_array()) {
            for (const auto& elem : *ck_array) {
                if (const auto tbl = elem.as_table()) {
                    auto postgres_from_table = parse_clickhouse(*tbl);
                    cfg.clickhouse[postgres_from_table.name] = postgres_from_table;
                }
            }
        }
        // 尝试作为单个表解析
        else if (const auto ck_table = ck_node.as_table()) {
            const auto postgres_config = parse_clickhouse(*ck_table);
            cfg.clickhouse[postgres_config.name] = postgres_config;
        }
    }

    if (const auto rd_node = db_tbl["redis"]) {
        // 尝试作为数组解析
        if (const auto rd_array = rd_node.as_array()) {
            for (const auto& elem : *rd_array) {
                if (const auto tbl = elem.as_table()) {
                    auto redis_from_table = parse_redis(*tbl);
                    cfg.redis[redis_from_table.name] = redis_from_table;
                }
            }
        }
        // 尝试作为单个表解析
        else if (const auto rd_table = rd_node.as_table()) {
            const auto redis_config = parse_redis(*rd_table);
            cfg.redis[redis_config.name] = redis_config;
        }
    }

    return cfg;
}

PostgresConfig ConfigLoader::parse_postgres_from_table(const toml::table& pg_tb) {
    PostgresConfig postgresConfig;
    // 在加载时强制检查 host、user、password字段，如果验证失败，ConfigLoader::load() 必须立即抛出异常，程序在启动阶段就失败。
    auto host_opt = pg_tb["host"].value<std::string>();
    auto user_opt = pg_tb["user"].value<std::string>();
    auto password_opt = pg_tb["password"].value<std::string>();
    // 如果 host 不存在，加载器立即失败！
    if (!host_opt) { throw std::runtime_error("PostgreSQL config '" + postgresConfig.name + "' is missing required field 'host'."); }
    // 如果 user 不存在，加载器立即失败！
    if (!user_opt) { throw std::runtime_error("PostgreSQL config '" + postgresConfig.name + "' is missing required field 'user'."); }
    // 如果 password 不存在，加载器立即失败！
    if (!password_opt) { throw std::runtime_error("PostgreSQL config '" + postgresConfig.name + "' is missing required field 'password'."); }

    postgresConfig.host = *host_opt;
    postgresConfig.user = *user_opt;
    postgresConfig.password = *password_opt;
    postgresConfig.name = pg_tb["name"].value_or(postgresConfig.name);
    postgresConfig.port = pg_tb["port"].value_or(postgresConfig.port);
    postgresConfig.database = pg_tb["database"].value_or(postgresConfig.database);
    postgresConfig.connection_timeout_ms = pg_tb["connection_timeout_ms"].value_or(postgresConfig.connection_timeout_ms);

    if (const auto ssl_tbl = pg_tb["ssl"].as_table()) {
        PostgresSslConfig SSlConfig;
        SSlConfig.ssl_enabled = (*ssl_tbl)["ssl_enabled"].value_or(SSlConfig.ssl_enabled);
        SSlConfig.ssl_mode = (*ssl_tbl)["ssl_mode"].value_or(SSlConfig.ssl_mode);
        SSlConfig.cert = (*ssl_tbl)["cert"].value<std::string>();
        SSlConfig.cert_private_key = (*ssl_tbl)["cert_private_key"].value<std::string>();
        postgresConfig.ssl = SSlConfig;
    }

    if (auto pool_tbl = pg_tb["pool"].as_table()) {
        PostgresPoolConfig pool;
        pool.max_connections = (*pool_tbl)["max_connections"].value_or(pool.max_connections);
        pool.min_connections = pool_tbl->get("min_connections")->value_or(pool.min_connections);
        pool.idle_timeout_ms = pool_tbl->get("idle_timeout_ms")->value_or(pool.idle_timeout_ms);
        postgresConfig.pool = pool;
    }
    return postgresConfig;
}

RedisConfig ConfigLoader::parse_redis(const toml::table& rd_tb) {
    RedisConfig redisConfig;
    // 在加载时强制检查 host、user、password字段，如果验证失败，ConfigLoader::load() 必须立即抛出异常，程序在启动阶段就失败。
    auto host_opt = rd_tb["host"].value<std::string>();
    auto password_opt = rd_tb["password"].value<std::string>();
    // 如果 password,host不存在，加载器立即失败！
    if (!host_opt) { throw std::runtime_error("Redis config '" + redisConfig.name + "' is missing required field 'host'."); }
    if (!password_opt) { throw std::runtime_error("Redis config '" + redisConfig.name + "' is missing required field 'password'."); }

    redisConfig.host = *host_opt;
    redisConfig.password = *password_opt;
    redisConfig.port = rd_tb["port"].value_or(redisConfig.port);
    redisConfig.database = rd_tb["database"].value_or(redisConfig.database);
    redisConfig.connection_timeout_ms = rd_tb["connection_timeout_ms"].value_or(redisConfig.connection_timeout_ms);


    if (const auto ssl_tbl = rd_tb["ssl"].as_table()) {
        RedisSslConfig ssl;
        ssl.ssl_enabled = (*ssl_tbl)["ssl_enabled"].value_or(ssl.ssl_enabled);
        ssl.cert = (*ssl_tbl)["cert"].value<std::string>();
        ssl.cert_private_key = (*ssl_tbl)["cert_private_key"].value<std::string>();
        redisConfig.ssl = ssl;
    }

    if (const auto pool_tbl = rd_tb.get("pool")->as_table()) {
        RedisPoolConfig pool_cfg;
        pool_cfg.max_connections = (*pool_tbl)["max_connections"].value_or(pool_cfg.max_connections);
        pool_cfg.min_connections = pool_tbl->get("min_connections")->value_or(pool_cfg.min_connections);
        pool_cfg.idle_timeout_ms = pool_tbl->get("idle_timeout_ms")->value_or(pool_cfg.idle_timeout_ms);
        redisConfig.pool = pool_cfg;
    }

    return redisConfig;
}

ClickhouseConfig ConfigLoader::parse_clickhouse(const toml::table& ck_tb) {

    ClickhouseConfig clickhouseConfig;

    // 在加载时强制检查 host、user、password字段，如果验证失败，ConfigLoader::load() 必须立即抛出异常，程序在启动阶段就失败。
    auto host_opt = ck_tb["host"].value<std::string>();
    auto user_opt = ck_tb["user"].value<std::string>();
    auto password_opt = ck_tb["password"].value<std::string>();
    // 如果 host 不存在，加载器立即失败！
    if (!host_opt) { throw std::runtime_error("Clickhouse config '" + clickhouseConfig.name + "' is missing required field 'host'."); }
    // 如果 user 不存在，加载器立即失败！
    if (!user_opt) { throw std::runtime_error("Clickhouse config '" + clickhouseConfig.name + "' is missing required field 'user'."); }
    // 如果 password 不存在，加载器立即失败！
    if (!password_opt) { throw std::runtime_error("Clickhouse config '" + clickhouseConfig.name + "' is missing required field 'password'."); }

    clickhouseConfig.host = *host_opt;
    clickhouseConfig.user = *user_opt;
    clickhouseConfig.password = *password_opt;
    clickhouseConfig.name = ck_tb["name"].value_or(clickhouseConfig.name);
    clickhouseConfig.port = ck_tb["port"].value_or(clickhouseConfig.port);
    clickhouseConfig.database = ck_tb["database"].value_or(clickhouseConfig.database);
    clickhouseConfig.connection_timeout_ms = ck_tb["connection_timeout_ms"].value_or(clickhouseConfig.connection_timeout_ms);

    if (const auto ssl_tbl = ck_tb.get("ssl")->as_table()) {
        ClickhouseSslConfig ssl;
        ssl.ssl_enabled = (*ssl_tbl)["ssl_enabled"].value_or(ssl.ssl_enabled);
        ssl.cert = (*ssl_tbl)["cert"].value<std::string>();
        ssl.cert_private_key = (*ssl_tbl)["cert_private_key"].value<std::string>();
        clickhouseConfig.ssl = ssl;
    }

    if (const auto pool_tbl = ck_tb.get("pool")->as_table()) {
        ClickhousePoolConfig pool_cfg;
        pool_cfg.max_connections = (*pool_tbl)["max_connections"].value_or(pool_cfg.max_connections);
        pool_cfg.min_connections = pool_tbl->get("min_connections")->value_or(pool_cfg.min_connections);
        pool_cfg.idle_timeout_ms = pool_tbl->get("idle_timeout_ms")->value_or(pool_cfg.idle_timeout_ms);
        clickhouseConfig.pool = pool_cfg;
    }

    return clickhouseConfig;
}
