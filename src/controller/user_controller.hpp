#ifndef USERCONTROLLER_HPP
#define USERCONTROLLER_HPP

// 1. 包含基类的定义
#include "controller.hpp"

// 2. 包含需要使用的组件的完整定义
#include "http/router.hpp"        // 因为要调用 router.GET()
#include "impl/user_service.hpp"   // 因为是成员变量

// 3. 包含需要的库类型
#include <boost/asio/awaitable.hpp> // 因为 Lambda 返回类型需要
#include <memory>                   // 因为用到了 std::shared_ptr

// 4. 对 Lambda 参数类型使用向前声明（如果可以的话）
// RequestContext 只是作为引用出现在 Lambda 签名中，可以向前声明
class RequestContext;

class UserController : public Controller {
public:
    explicit UserController(std::shared_ptr<UserService> user_service);

    void register_routes(Router& router) override {
        // 实现保持不变，现在它应该可以编译通过了
        router.GET("/user/:id/:name", [this](RequestContext& ctx) -> boost::asio::awaitable<void> {
            return this->handle_get_user(ctx);
        });

        router.GET("/concurrent?sum={}&url={}", [this](RequestContext& ctx) -> boost::asio::awaitable<void> {
            return this->handle_concurrent_test(ctx);
        });
        
        router.GET("/request?url={}", [this](RequestContext& ctx) -> boost::asio::awaitable<void> {
            return this->handle_http_client(ctx);
        });
        router.GET("/ws", [this](RequestContext& ctx) -> boost::asio::awaitable<void> {
            return this->handle_test_wss(ctx);
        });
        router.POST("/intensiveComputing", [this](RequestContext& ctx) -> boost::asio::awaitable<void> {
            return this->intensive_computing(ctx);
        });
    }

private:
    boost::asio::awaitable<void> handle_get_user(RequestContext& ctx);
    boost::asio::awaitable<void> handle_concurrent_test(RequestContext& ctx);
    boost::asio::awaitable<void> handle_http_client(RequestContext& ctx);
    boost::asio::awaitable<void> handle_test_wss(RequestContext& ctx);
    boost::asio::awaitable<void> intensive_computing(RequestContext& ctx);
    std::shared_ptr<UserService> user_service_;
};

#endif //USERCONTROLLER_HPP