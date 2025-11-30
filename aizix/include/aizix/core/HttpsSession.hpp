//
// Created by Aiziboy on 2025/11/20.
//

#ifndef AIZIX_HTTPS_SESSION_HPP
#define AIZIX_HTTPS_SESSION_HPP

#include <aizix/http/router.hpp>

#include <boost/asio/use_awaitable.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>     // Asio 的 SSL/TLS 功能
#include <spdlog/spdlog.h>
#include <aizix/http/request_context.hpp>


class HttpsSession : public std::enable_shared_from_this<HttpsSession> {
public:
    HttpsSession(std::shared_ptr<boost::asio::ssl::stream<tcp::socket>> stream, Router& router, boost::asio::any_io_executor work_exec, size_t body_limit, std::chrono::milliseconds timeout, std::chrono::milliseconds keep_alive);
    ~HttpsSession();

    // 启动会话
    boost::asio::awaitable<void> run();

    // 优雅关闭接口
    void stop();

private:
    // 将 close 拆分为 force_close
    // ReSharper disable once CppMemberFunctionMayBeConst
    void force_close();

    // 正常的 close (保留给运行循环自然结束时用)
    boost::asio::awaitable<void> graceful_close();

    boost::asio::ip::tcp::endpoint remote_endpoint() const;

    std::shared_ptr<boost::asio::ssl::stream<tcp::socket>> stream_;
    Router& router_;
    boost::asio::any_io_executor work_exec_;
    size_t body_limit_;
    std::chrono::milliseconds initial_timeout_;
    std::chrono::milliseconds keep_alive_timeout_;
    bool stopped_ = false;
};


#endif //AIZIX_HTTPS_SESSION_HPP
