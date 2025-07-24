#ifndef USERSERVICE_HPP
#define USERSERVICE_HPP

#include "http/request_context.hpp"
#include <nlohmann/json.hpp>   // 引入 nlohmann/json 库，用于轻松地创建和序列化 JSON 对象

#include "core/client/http1_connection.hpp"
#include "core/client/ihttp_client.hpp"


class UserService {
public:
    // 通过构造函数注入一个 IHttpClient 的共享指针
    explicit UserService(std::shared_ptr<IHttpClient> http_client)
        : http_client_(std::move(http_client)) {}

    boost::asio::awaitable<void> get_user_by_id(RequestContext& ctx) {
        SPDLOG_INFO("User ID:");
        // 从 ctx 中获取路径参数
       // auto id_str = ctx.path_param("id");
       // if (!id_str) {
       //     // 如果参数不存在，填充一个 400 Bad Request 响应
       //     ctx.string(http::status::bad_request, "Missing user ID");
       //     co_return;
       // }
//
       // // ... 业务逻辑 ...
       // nlohmann::json result = {
       //     {"id", id_str},
       //     {"data", {"Tom", "Jerry", "Spike"}} // 示例数据
       // };
       // SPDLOG_INFO("User ID: {}", id_str.value());
       // // 使用便利函数填充 200 OK 响应
       // ctx.json(http::status::ok, result);
        ctx.string(http::status::ok, "Hello, World!");

        co_return;
    }


    static boost::asio::awaitable<void> search_users(RequestContext& ctx) {
        // 2. 从上下文中获取名为 "kw" 和 "page" 的查询参数
        auto kw = ctx.query_param("kw");
        auto page = ctx.query_param("page");
        SPDLOG_DEBUG("进来了");
        // 3. 构造一个 JSON 对象来表示搜索结果
        nlohmann::json result = {
            {"kw", kw}, {"page", page},
            {"data", {"Tom", "Jerry", "Spike"}} // 示例数据
        };

        ctx.json(http::status::ok, result);

        // 5. 返回响应
        co_return ;
    }

    boost::asio::awaitable<void> test_http_client(RequestContext& ctx) {
        // 2. 从上下文中获取名为 "kw" 和 "page" 的查询参数
        auto url = ctx.query_param("url");
        std::string_view value = url.value();

        //ctx.string(http::status::ok, "TLS_ST_CR_SRVR_HELLO");
        //co_return ;
        try {
            // 直接使用注入的 http_client_ 实例
            SPDLOG_DEBUG("准备对：{} 发起请求",value);
            HttpResponse response = co_await http_client_->get(value, {});

            if (response.result() == http::status::ok) {
                SPDLOG_DEBUG("请求成功");
                std::string_view content_type;
                auto it = response.find(http::field::content_type);
                if (it != response.end()) {
                    content_type = it->value();
                }

                ctx.string(http::status::ok, response.body(), content_type);
                co_return ;
            }

            ctx.string(http::status::ok, http::obsolete_reason(response.result()));
            co_return;
        } catch (const std::exception& e) {
            // 处理异常
            SPDLOG_ERROR("Failed to get user from external API: {}", e.what());
            ctx.json(http::status::bad_gateway,
                     {
                         {"error", "Failed to fetch from upstream server"},
                         {"reason", e.what()}
                     });
        }
    }

private:
    std::remove_reference<std::shared_ptr<IHttpClient>&>::type http_client_;
};

#endif // USERSERVICE_HPP
