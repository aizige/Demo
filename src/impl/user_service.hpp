//
// Created by Aiziboy on 25-8-5.
//

#ifndef USER_SERVICE_HPP
#define USER_SERVICE_HPP


#include "http/request_context.hpp"
#include <nlohmann/json.hpp>   // 引入 nlohmann/json 库，用于轻松地创建和序列化 JSON 对象

#include "core/client/http1_connection.hpp"
#include "core/client/ihttp_client.hpp"
#include "core/client/WebSocketClient.hpp"


class UserService {
public:
    // 通过构造函数注入一个 IHttpClient 的共享指针
    explicit UserService(std::shared_ptr<IHttpClient> http_client,std::shared_ptr<WebSocketClient> ws_client);


    boost::asio::awaitable<void> get_user_by_id(RequestContext &ctx);


    boost::asio::awaitable<void> search_users(RequestContext &ctx);

    boost::asio::awaitable<void> test_http_client(RequestContext &ctx);

    boost::asio::awaitable<void> connect_to_status_stream(RequestContext &ctx) ;

private:
    std::remove_reference_t<std::shared_ptr<IHttpClient> &> http_client_;
    std::remove_reference_t<std::shared_ptr<WebSocketClient>&> ws_client_;
};


#endif //USER_SERVICE_HPP
