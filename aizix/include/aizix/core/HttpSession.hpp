//
// Created by Aiziboy on 2025/11/20.
//

#ifndef AIZIX_HTTP_SESSION_HPP
#define AIZIX_HTTP_SESSION_HPP
#include <aizix/http/router.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>     // Asio 的 SSL/TLS 功能
#include <aizix/http/request_context.hpp>




class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(tcp::socket socket, Router& router, boost::asio::any_io_executor work_exec, const size_t body_limit, const std::chrono::milliseconds timeout, const std::chrono::milliseconds keep_alive);
    ~HttpSession();

    // 启动会话
    boost::asio::awaitable<void> run();

    // 优雅关闭接口
    void stop() ;

private:
    void close_socket() ;
    boost::asio::ip::tcp::endpoint remote_endpoint() const;

    tcp::socket socket_;
    Router& router_;
    boost::asio::any_io_executor work_exec_;
    size_t body_limit_;
    std::chrono::milliseconds initial_timeout_;
    std::chrono::milliseconds keep_alive_timeout_;
    bool stopped_ = false;
};


#endif //AIZIX_HTTP_SESSION_HPP
