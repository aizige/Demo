//
// Created by Aiziboy on 2025/7/18.
//

#ifndef ICONNECTION_HPP
#define ICONNECTION_HPP

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "http/http_common_types.hpp"

class IConnection {
public:
    virtual ~IConnection() = default;

    // 核心功能：异步执行一个请求并返回一个响应
    // 这是所有连接子类必须实现的纯虚函数
    virtual boost::asio::awaitable<HttpResponse> execute(HttpRequest& request) = 0;

    // 检查连接是否仍然可用且可以被复用
    virtual bool is_usable() const = 0;

    // 主动关闭连接
    virtual void close() = 0;

    // 获取连接的唯一ID，用于连接池管理
    virtual const std::string& id() const = 0;

    virtual const std::string& get_pool_key() const = 0;

    // [新增] 暴露底层的 socket 以便进行健康检查
    virtual boost::asio::ip::tcp::socket& lowest_layer_socket() = 0;

};


#endif //CONNECTION_HPP