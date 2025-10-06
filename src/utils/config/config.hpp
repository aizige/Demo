//
// Created by Aiziboy on 2025/10/1.
//

#ifndef UNTITLED1_CONFIG_HPP
#define UNTITLED1_CONFIG_HPP

#include <string>
#include <vector>
#include <optional>

struct AppConfig {
    std::string name;
    std::string version;
};

struct SSLConfig {
    bool enabled = false;
    std::vector<std::string> tls_version;
    std::string certificate_file;
    std::string certificate_private_key_file;
};

struct ServerConfig {
    std::string host = "0.0.0.0";
    int port = 8080;
    int threads = 0;
    bool H2 = false;
    bool H2C = false;
    int timeout_ms = 5000;
    int max_request_size_kb = 1024;
    SSLConfig ssl;
};

struct DatabasePoolConfig {
    int max_connections = 32;
    int min_connections = 4;
    int idle_timeout_ms = 30000;
    int connection_timeout_ms = 5000;
    std::string validation_query = "SELECT 1";
};

struct DatabaseConfig {
    std::string type;
    std::string host;
    int port;
    std::string user;
    std::string password;
    std::string database;
    bool ssl_enabled = false;
    std::string sslmode;
    std::string sslcert;
    std::string sslkey;
    std::string sslrootcert;
    DatabasePoolConfig pool;
};

struct AppConfigRoot {
    AppConfig app;
    ServerConfig server;
    DatabaseConfig database;
};

#endif //UNTITLED1_CONFIG_HPP