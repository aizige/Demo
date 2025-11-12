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


boost::asio::awaitable<void> UserController::handle_concurrent_test(RequestContext& ctx) {
    auto sum = ctx.query_param_as<int16_t>("sum");
    auto url = ctx.query_param_as<std::string_view>("url");

    SPDLOG_DEBUG("sum: {}, url: {}", *sum, *url);
    auto user_result = co_await user_service_->concurrent_test(*sum, *url);
    co_return ctx.string(http::status::ok, user_result);
}

boost::asio::awaitable<void> UserController::handle_http_client(RequestContext& ctx) {
    auto basic_string_view = ctx.queryParam("url");
    if (!basic_string_view) {
        co_return ctx.string(http::status::bad_request, "url不合法");
    }
    SPDLOG_DEBUG("url1: {}", *basic_string_view);

    auto query_param_as = ctx.query_param_as<std::string>("url");
    if (!query_param_as) {
        co_return ctx.string(http::status::bad_request, "url不合法1111111");
    }
    SPDLOG_DEBUG("url2: {}", *query_param_as);


    auto basic_string = co_await user_service_->test_http_client(*basic_string_view);
    ctx.json(http::status::ok, basic_string);
}

boost::asio::awaitable<void> UserController::handle_test_wss(RequestContext& ctx) {
    //auto body = ctx.request().body();
    //SPDLOG_DEBUG("kw: {}", body);
    co_return ctx.string(http::status::ok, "Hello world!");
}

boost::asio::awaitable<void> UserController::intensive_computing(RequestContext& ctx) {
    auto user_result = co_await user_service_->intensiveComputing();
    SPDLOG_DEBUG("计算结果: {}", user_result);
    std::string str = fmt::format("计算结果: {}\t  ",user_result);
    co_return ctx.json(http::status::ok,str);
}
