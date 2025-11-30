//
// Created by Aiziboy on 2025/10/15.
//

#ifndef AIZIX_CONFIG_LOADER_HPP
#define AIZIX_CONFIG_LOADER_HPP

#include <string>

#include <aizix/utils/config/AizixConfig.hpp>
#include <aizix/lib/toml.hpp>


class ConfigLoader {
public:
    /**
     * @brief 从指定的 TOML 文件路径加载配置。
     *
     * @param filepath 配置文件的路径。
     * @return Config 填充了配置数据的结构体。
     * @throws std::runtime_error 如果文件不存在或解析失败。
     */
    static AizixConfig load(const std::string& filepath);

private:
    // 为了保持接口干净，所有解析函数都作为私有帮助函数
    static AppConfig parse_app(const toml::table& app_tb);
    static ServerConfig parse_server(const toml::table& server_tb);
    static ClientConfig parse_client(const toml::table& client_tb);
    static DatabaseConfig parse_database(const toml::table& db_tb);
    static LoggingConfig parse_logging(const toml::table& log_tb);

    // 数据库子解析器
    static PostgresConfig parse_postgres_from_table(const toml::table& pg_tb);
    static RedisConfig parse_redis(const toml::table& rd_tb);
    static ClickhouseConfig parse_clickhouse(const toml::table& ck_tb);
};


#endif //AIZIX_CONFIG_LOADER_HPP