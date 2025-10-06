//
// Created by Aiziboy on 2025/10/1.
//

#include "config_loader.hpp"
#include "utils/toml.hpp"

config_loader::config_loader(const std::string& path) {
    auto tbl = toml::parse_file(path);
    AppConfigRoot cfg;

    // [app]
    if (auto app = tbl["app"].as_table()) {
        cfg.app.name = app->get("name")->value_or("UnnamedApp");
        cfg.app.version = app->get("version")->value_or("0.0.0");
    }

    // [server]
    if (auto server = tbl["server"].as_table()) {
        cfg.server.host = server->get("host")->value_or("0.0.0.0");
        cfg.server.port = server->get("port")->value_or(8080);
        cfg.server.threads = server->get("threads")->value_or(0);
        cfg.server.H2 = server->get("H2")->value_or(false);
        cfg.server.H2C = server->get("H2C")->value_or(false);
        cfg.server.timeout_ms = server->get("timeout_ms")->value_or(5000);
        cfg.server.max_request_size_kb = server->get("max_request_size_kb")->value_or(1024);

        // [server.ssl]
        if (auto ssl = server->get("ssl")->as_table()) {
            cfg.server.ssl.enabled = ssl->get("enabled")->value_or(false);
            if (auto tls = ssl->get("tls_version")->as_array()) {
                for (const auto& v : *tls) {
                    if (auto s = v.value<std::string>())
                        cfg.server.ssl.tls_version.push_back(*s);
                }
            }
            cfg.server.ssl.certificate_file = ssl->get("certificate_file")->value_or("");
            cfg.server.ssl.certificate_private_key_file = ssl->get("certificate_private_key_file")->value_or("");
        }
    }

    // [database]
    if (auto db = tbl["database"].as_table()) {
        cfg.database.type = db->get("type")->value_or("postgresql");
        cfg.database.host = db->get("host")->value_or("localhost");
        cfg.database.port = db->get("port")->value_or(5432);
        cfg.database.user = db->get("user")->value_or("user");
        cfg.database.password = db->get("password")->value_or("pass");
        cfg.database.database = db->get("database")->value_or("default");
        cfg.database.ssl_enabled = db->get("ssl_enabled")->value_or(false);
        cfg.database.sslmode = db->get("sslmode")->value_or("require");
        cfg.database.sslcert = db->get("sslcert")->value_or("");
        cfg.database.sslkey = db->get("sslkey")->value_or("");
        cfg.database.sslrootcert = db->get("sslrootcert")->value_or("");

        // [postgresql.pool] or [database.pool]
        if (auto pool = tbl["postgresql.pool"].as_table()) {
            cfg.database.pool.max_connections = pool->get("max_connections")->value_or(32);
            cfg.database.pool.min_connections = pool->get("min_connections")->value_or(4);
            cfg.database.pool.idle_timeout_ms = pool->get("idle_timeout_ms")->value_or(30000);
            cfg.database.pool.connection_timeout_ms = pool->get("connection_timeout_ms")->value_or(5000);
            cfg.database.pool.validation_query = pool->get("validation_query")->value_or("SELECT 1");
        }
    }

    return;
}

const AppConfigRoot& config_loader::get() const {
}

void config_loader::reload() {
}
