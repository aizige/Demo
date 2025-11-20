//
// Created by Aiziboy on 25-8-5.
//

#ifndef XINGJU_USER_SERVICE_HPP
#define XINGJU_USER_SERVICE_HPP


#include <boost/asio/thread_pool.hpp>

#include <aizix/http/request_context.hpp>
#include <aizix/core/client/http_connection.hpp>
#include <aizix/core/client/ihttp_client.hpp>
#include <aizix/core/client/websocket_client.hpp>


class UserService {
public:
    // 通过构造函数注入一个 IHttpClient 的共享指针
    explicit UserService(boost::asio::any_io_executor io_executor, boost::asio::any_io_executor worker_executor, const std::shared_ptr<IHttpClient>& http_client, const std::shared_ptr<WebSocketClient>& ws_client);

    boost::asio::awaitable<void> simulate_high_concurrency(int num_requests, std::string_view url);


    boost::asio::awaitable<std::string> get_user_by_id(int id, std::string_view name);


    boost::asio::awaitable<std::string> concurrent_test(int sum, std::string_view url);

    boost::asio::awaitable<std::string> test_http_client(std::string_view url);

    [[nodiscard]] boost::asio::awaitable<std::string> connect_to_status_stream(std::string_view body) const;
    boost::asio::awaitable<double> intensiveComputing();

private:
    double heavy_compute();

    boost::asio::any_io_executor ioc_executor_;
    boost::asio::any_io_executor worker_executor_;
    std::shared_ptr<IHttpClient> http_client_;
    std::shared_ptr<WebSocketClient> ws_client_;
};


#endif //XINGJU_USER_SERVICE_HPP
