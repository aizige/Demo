//
// Created by Aiziboy on 25-8-5.
//

#ifndef USER_SERVICE_HPP
#define USER_SERVICE_HPP


#include "http/request_context.hpp"


#include "core/client/http1_connection.hpp"
#include "core/client/ihttp_client.hpp"
#include "core/client/WebSocketClient.hpp"


class UserService {
public:
    // 通过构造函数注入一个 IHttpClient 的共享指针
    explicit UserService(std::shared_ptr<IHttpClient> http_client, std::shared_ptr<WebSocketClient> ws_client);


    boost::asio::awaitable<std::string> get_user_by_id(int id, std::string_view name);


    boost::asio::awaitable<std::string> search_users(std::string_view kw, std::string_view page);

    boost::asio::awaitable<std::string> test_http_client(std::string_view url);

    boost::asio::awaitable<std::string> connect_to_status_stream(std::string_view body);

private:
    std::remove_reference_t<std::shared_ptr<IHttpClient>&> http_client_;
    std::remove_reference_t<std::shared_ptr<WebSocketClient>&> ws_client_;
};


#endif //USER_SERVICE_HPP
