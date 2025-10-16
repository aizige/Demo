//
// Created by Aiziboy on 2025/10/15.
//

#include "ConfigLoader.hpp"
#include <stdexcept>
#include <iostream>

#include "utils/toml.hpp"

// --- 主加载函数 ---

Config ConfigLoader::load(const std::string& filepath) {
    toml::table root_tbl;
    try {
        root_tbl = toml::parse_file(filepath);
    } catch (const toml::parse_error& err) {
        std::cerr << "Error parsing config file '" << filepath << "':\n" << err << std::endl;
        throw std::runtime_error("Failed to parse configuration file.");
    }

    Config config;
    config.app = parse_app(root_tbl);
    config.server = parse_server(root_tbl);
    config.client = parse_client(root_tbl);
    config.database = parse_database(root_tbl);
    config.logging = parse_logging(root_tbl);

    return config;
}

// --- 私有帮助函数实现 ---

AppConfig ConfigLoader::parse_app(const toml::table& root_tb) {
    AppConfig appConfig;
    if (const auto table = root_tb["app"].as_table()) {
        appConfig.name = table->get("name")->value<std::string>();
        appConfig.version = table->get("version")->value<std::string>();
        appConfig.profiles = table->get("profiles")->value<std::string>();
    }
    return appConfig;
}

LoggingConfig ConfigLoader::parse_logging(const toml::table& root_tb) {
    LoggingConfig logConfig;
    if (const auto table = root_tb["logging"].as_table()) {
        logConfig.level = table->get("level")->value<std::string>();
        logConfig.output = table->get("output")->value<std::string>();
        logConfig.file_path = table->get("file_path")->value<std::string>();
        logConfig.max_size_mb = table->get("max_size_mb")->value<uint16_t>();
        logConfig.max_files = table->get("max_files")->value<uint16_t>();
    }
    return logConfig;
}

ServerConfig ConfigLoader::parse_server(const toml::table& root_tb) {
    ServerConfig serverConfig;
    if (auto server_tbl = root_tb["server"].as_table()) {
        serverConfig.port = server_tbl->get("port")->value<std::int16_t>();
        serverConfig.http2_enabled = server_tbl->get("http2_enabled")->value<bool>();
        serverConfig.io_threads = server_tbl->get("io_threads")->value<std::int16_t>();
        serverConfig.threads = server_tbl->get("threads")->value<std::int16_t>();
        serverConfig.timeout_ms = server_tbl->get("timeout_ms")->value<std::int32_t>();
        serverConfig.keep_alive_timeout_ms = server_tbl->get("keep_alive_timeout_ms")->value<std::int32_t>();
        serverConfig.max_request_size_kb = server_tbl->get("max_request_size_kb")->value<std::int32_t>();


        if (auto ssl_tbl = server_tbl->get("ssl")->as_table()) {
            ServerSslConfig serverSSLConfig;
            serverSSLConfig.enabled = ssl_tbl->get("enabled")->value<bool>();
            serverSSLConfig.certificate_file = ssl_tbl->get("certificate_file")->value<std::string>();
            serverSSLConfig.certificate_key_file = ssl_tbl->get("certificate_key_file")->value<std::string>();

            if (auto versions = ssl_tbl->get("tls_versions")->as_array()) {
                serverSSLConfig.tls_versions.clear();
                for (const auto& elem : *versions) {
                    if (auto str = elem.value<std::string>()) {
                        serverSSLConfig.tls_versions.push_back(*str);
                    }
                }
            }
             if (auto hosts = ssl_tbl->get("required_hosts_list")->as_array()) {
                serverSSLConfig.required_hosts_list.clear();
                for (const auto& elem : *hosts) {
                    if (auto str = elem.value<std::string>()) {
                        serverSSLConfig.required_hosts_list.push_back(*str);
                    }
                }
            }
            serverConfig.ssl = serverSSLConfig;
        }
    }
    return serverConfig;
}

ClientConfig ConfigLoader::parse_client(const toml::table& root_tb) {
    ClientConfig cfg;
    if (const auto client_tbl = root_tb["client"].as_table()) {
        // 使用 value_or 填充每个字段
        cfg.http2_enabled = client_tbl->get("http2_enabled")->value_or(cfg.http2_enabled.value());
        cfg.ssl_verify = client_tbl->get("ssl_verify")->value_or(cfg.ssl_verify.value());
        cfg.max_redirects_ = client_tbl->get("max_redirects_")->value_or(cfg.max_redirects_.value());
        cfg.connect_timeout_ms = client_tbl->get("connect_timeout_ms")->value_or(cfg.connect_timeout_ms.value());
        cfg.idle_connection_timeout_ms = client_tbl->get("idle_close_threshold_ms")->value_or(cfg.idle_connection_timeout_ms.value());
        cfg.ping_interval_ms = client_tbl->get("ping_interval_ms")->value_or(cfg.ping_interval_ms.value());
        cfg.host_connection_pool_size = client_tbl->get("host_connection_pool_size")->value_or(cfg.host_connection_pool_size.value());
        cfg.max_connections_per_host = client_tbl->get("MAX_CONNECTIONS_PER_HOST")->value_or(cfg.max_connections_per_host.value());
        cfg.http2_request_send_timeout_ms = client_tbl->get("http2_request_send_timeout_ms")->value_or(cfg.http2_request_send_timeout_ms.value());
        cfg.http2_response_recv_timeout_ms = client_tbl->get("http2_response_recv_timeout_ms")->value_or(cfg.http2_response_recv_timeout_ms.value());
        cfg.http2_max_streams_per_connection = client_tbl->get("http2_max_streams_per_connection")->value_or(cfg.http2_max_streams_per_connection.value());
    }
    return cfg;
}


DatabaseConfig ConfigLoader::parse_database(const toml::table& root_tbl) {
    DatabaseConfig cfg;
    auto db_tbl_ptr = root_tbl["database"].as_table();
    if (!db_tbl_ptr) {
        return cfg; // No [database] section
    }
    const auto& db_tbl = *db_tbl_ptr;

    // --- 核心逻辑：处理 PostgreSQL 单表或表数组 ---
    if (const auto pg_node = db_tbl.get("postgresql")) {
        // 尝试作为数组解析
        if (const auto pg_array = pg_node->as_array()) {
            for (const auto& elem : *pg_array) {
                if (const auto tbl = elem.as_table()) {
                    auto postgres_from_table = parse_postgres_from_table(*tbl);
                    cfg.postgresql[postgres_from_table.name.value()] = postgres_from_table;
                }
            }
        }
        // 尝试作为单个表解析
        else if (const auto pg_table = pg_node->as_table()) {
            const auto postgres_config = parse_postgres_from_table(*pg_table);
            cfg.postgresql[postgres_config.name.value()] = postgres_config;
        }
    }

    // --- 解析 Redis 和 ClickHouse ---
    if (const auto ck_node = db_tbl.get("clickhouse")) {
        // 尝试作为数组解析
        if (const auto ck_array = ck_node->as_array()) {
            for (const auto& elem : *ck_array) {
                if (const auto tbl = elem.as_table()) {
                    auto postgres_from_table = parse_clickhouse(*tbl);
                    cfg.clickhouse[postgres_from_table.name.value()] = postgres_from_table;
                }
            }
        }
        // 尝试作为单个表解析
        else if (const auto ck_table = ck_node->as_table()) {
            const auto postgres_config = parse_clickhouse(*ck_table);
            cfg.clickhouse[postgres_config.name.value()] = postgres_config;
        }
    }

    if (const auto rd_node = db_tbl.get("redis")) {
        // 尝试作为数组解析
        if (const auto rd_array = rd_node->as_array()) {
            for (const auto& elem : *rd_array) {
                if (const auto tbl = elem.as_table()) {
                    auto redis_from_table = parse_clickhouse(*tbl);
                    cfg.clickhouse[redis_from_table.name.value()] = redis_from_table;
                }
            }
        }
        // 尝试作为单个表解析
        else if (const auto rd_table = rd_node->as_table()) {
            const auto redis_config = parse_clickhouse(*rd_table);
            cfg.clickhouse[redis_config.name.value()] = redis_config;
        }
    }

    return cfg;
}

PostgresConfig ConfigLoader::parse_postgres_from_table(const toml::table& pg_tb) {
    PostgresConfig postgresConfig;
    postgresConfig.name = pg_tb.get("name")->value_or(postgresConfig.name.value());
    postgresConfig.host = pg_tb.get("host")->value_or(postgresConfig.host.value());
    postgresConfig.port = pg_tb.get("port")->value_or(postgresConfig.port.value());
    postgresConfig.user = pg_tb.get("user")->value_or(postgresConfig.user.value());
    postgresConfig.password = pg_tb.get("password")->value_or(postgresConfig.password.value());
    postgresConfig.database = pg_tb.get("database")->value_or(postgresConfig.database.value());

    if (const auto ssl_tbl = pg_tb.get("ssl")->as_table()) {
        PostgresSslConfig SSlConfig;
        SSlConfig.ssl_enabled = ssl_tbl->get("ssl_enabled")->value_or(SSlConfig.ssl_enabled.value());
        SSlConfig.ssl_mode = ssl_tbl->get("ssl_mode")->value_or(SSlConfig.ssl_mode.value());
        SSlConfig.sslcert = ssl_tbl->get("sslcert")->value<std::string>();
        SSlConfig.sslkey = ssl_tbl->get("sslkey")->value<std::string>();
        SSlConfig.sslrootcert = ssl_tbl->get("sslrootcert")->value<std::string>();
        postgresConfig.ssl = SSlConfig;
    }

    if (auto pool_tbl = pg_tb.get("pool")->as_table()) {
        PostgresPoolConfig pool;
        pool.max_connections = pool_tbl->get("max_connections")->value_or(pool.max_connections.value());
        pool.min_connections = pool_tbl->get("min_connections")->value_or(pool.min_connections.value());
        pool.idle_timeout_ms = pool_tbl->get("idle_timeout_ms")->value_or(pool.idle_timeout_ms.value());
        pool.connection_timeout_ms = pool_tbl->get("connection_timeout_ms")->value_or(pool.connection_timeout_ms.value());
        postgresConfig.pool = pool;
    }
    return postgresConfig;
}

RedisConfig ConfigLoader::parse_redis(const toml::table& rd_tb) {

    RedisConfig redisConfig;

    redisConfig.host = rd_tb.get("host")->value_or(redisConfig.host.value());
    redisConfig.port = rd_tb.get("port")->value_or(redisConfig.port.value());
    redisConfig.database = rd_tb.get("database")->value_or(redisConfig.database.value());
    redisConfig.password = rd_tb.get("password")->value_or(redisConfig.password.value());


    if (const auto ssl_tbl = rd_tb.get("ssl")->as_table()) {
        RedisSslConfig ssl;
        ssl.ssl_enabled = ssl_tbl->get("ssl_enabled")->value_or(ssl.ssl_enabled.value());
        ssl.ca_file = ssl_tbl->get("ca_file")->value_or(ssl.ca_file.value());
        ssl.client_cert_file = ssl_tbl->get("client_cert_file")->value<std::string>();
        ssl.client_key_file = ssl_tbl->get("client_key_file")->value<std::string>();
        redisConfig.ssl = ssl;
    }

    if (const auto pool_tbl = rd_tb.get("pool")->as_table()) {
        RedisPoolConfig pool_cfg;
        pool_cfg.pool_size = pool_tbl->get("pool_size")->value_or(pool_cfg.pool_size.value());
        pool_cfg.max_wait_ms = pool_tbl->get("max_wait_ms")->value_or(pool_cfg.max_wait_ms.value());
        pool_cfg.idle_timeout_ms = pool_tbl->get("idle_timeout_ms")->value_or(pool_cfg.idle_timeout_ms.value());
        pool_cfg.health_check_interval_ms = pool_tbl->get("connection_timeout_ms")->value_or(pool_cfg.health_check_interval_ms.value());
        redisConfig.pool = pool_cfg;
    }

    return redisConfig;
}

ClickhouseConfig ConfigLoader::parse_clickhouse(const toml::table& ck_tb) {
    // ClickHouse 也不是必需的
    ClickhouseConfig clickhouseConfig;

    clickhouseConfig.host = ck_tb.get("host")->value_or(clickhouseConfig.host.value());
    clickhouseConfig.port = ck_tb.get("port")->value_or(clickhouseConfig.port.value());
    clickhouseConfig.database = ck_tb.get("database")->value_or(clickhouseConfig.database.value());
    clickhouseConfig.user = ck_tb.get("user")->value_or(clickhouseConfig.user.value());
    clickhouseConfig.password = ck_tb.get("password")->value_or(clickhouseConfig.password.value());


    if (const auto ssl_tbl = ck_tb.get("ssl")->as_table()) {
        ClickhouseSslConfig ssl;
        ssl.enabled = ssl_tbl->get("enabled")->value_or(ssl.enabled.value());
        ssl.client_cert_file = ssl_tbl->get("client_cert_file")->value_or(ssl.client_cert_file.value());
        ssl.ca_cert_file = ssl_tbl->get("ca_cert_file")->value<std::string>();
        ssl.client_key_file = ssl_tbl->get("client_key_file")->value<std::string>();
        ssl.verify_peer = ssl_tbl->get("verify_peer")->value_or(ssl.verify_peer.value());
        ssl.verify_hostname = ssl_tbl->get("verify_hostname")->value_or(ssl.verify_hostname.value());
        clickhouseConfig.ssl = ssl;
    }

    if (const auto pool_tbl = ck_tb.get("pool")->as_table()) {
        ClickhousePoolConfig pool_cfg;
        pool_cfg.max_connections = pool_tbl->get("max_connections")->value_or(pool_cfg.max_connections.value());
        pool_cfg.min_connections = pool_tbl->get("min_connections")->value_or(pool_cfg.min_connections.value());
        pool_cfg.idle_timeout_ms = pool_tbl->get("idle_timeout_ms")->value_or(pool_cfg.idle_timeout_ms.value());
        pool_cfg.connection_timeout_ms = pool_tbl->get("connection_timeout_ms")->value_or(pool_cfg.connection_timeout_ms.value());
        clickhouseConfig.pool = pool_cfg;
    }

    return clickhouseConfig;
}