#include "user_controller.hpp"

UserController::UserController(std::shared_ptr<UserService> user_service)
    : user_service_(std::move(user_service)) {
}

boost::asio::awaitable<void> UserController::handle_get_user(RequestContext& ctx) {
    auto id = ctx.pathParam("id");
    auto id_name = ctx.pathParam("name");


    if (!id || !id_name) {
        // 如果参数不存在，填充一个 400 Bad Request 响应
        ctx.string(http::status::bad_request, "Missing id or name");
        co_return;
    }

    // 转为int
    int id_int = 0;
    auto [ptr, ec] = std::from_chars(id->data(), id->data() + id->size(), id_int);
    if (ec != std::errc() || ptr != id->data() + id->size()) {
        throw std::runtime_error("Invalid integer format: not fully consumed");
    }

    std::string name_str{*id_name}; // 转换为 std::string

    auto user_result = co_await user_service_->get_user_by_id(id_int, name_str);
    ctx.string(http::status::ok, user_result);
}


boost::asio::awaitable<void> UserController::handle_search_users(RequestContext& ctx) {

    for (const auto& [key, values] : ctx.queryParamAll()) {
        SPDLOG_DEBUG("Key: {}",key );
        for (const auto& value : values) {
            SPDLOG_DEBUG("Value: {}",value );
        }
    }



    auto ke = ctx.queryParam("kw");
    auto page = ctx.queryParam("page");
    std::string key_str{*ke};
    std::string page_str{*page};
    SPDLOG_DEBUG("kw: {}, page: {}", key_str, page_str);

    co_return ctx.string(http::status::ok, page_str + "--" + key_str);
}

boost::asio::awaitable<void> UserController::handle_http_client(RequestContext& ctx) {
    auto basic_string_view = ctx.queryParam("url");
    if (!basic_string_view) {
       co_return ctx.string(http::status::bad_request, "url不合法");
    }
SPDLOG_DEBUG("url1: {}",*basic_string_view );

    auto query_param_as = ctx.query_param_as<std::string>("url");
    if (!query_param_as) {
        co_return ctx.string(http::status::bad_request, "url不合法1111111");
    }
    SPDLOG_DEBUG("url2: {}",*query_param_as );



    auto param_as = ctx.query_param_as<std::string>("url2");
    if (!param_as) {
        co_return ctx.string(http::status::bad_request, "url不合法2222");
    }

    SPDLOG_DEBUG("url3: {}",*param_as );

    auto basic_string = co_await user_service_ ->test_http_client(*basic_string_view);
    ctx.string(http::status::ok, basic_string);
}

boost::asio::awaitable<void> UserController::handle_test_wss(RequestContext& ctx) {
    auto body = ctx.request().body();
    SPDLOG_DEBUG("kw: {}", body);
    co_return;
}
