//
// Created by Aiziboy on 2025/11/21.
//

#include "XingjuController.hpp"

#include <iostream>
#include <boost/url/error_types.hpp>
#include <spdlog/spdlog.h>

#include "aizix/version.hpp"
#include "aizix/http/request_context.hpp"
#include "error/xingju_error.hpp"
#include "response/ResponseResult.hpp"
#include "response/Instance.hpp"

XingjuController::XingjuController(std::shared_ptr<ServiceInstanceManager> service_instance_manager)
    : service_instance_manager_(std::move(service_instance_manager)) {
}

void XingjuController::registerRoutes(Router& router) {
    router.POST("/register", [this](RequestContext& ctx) -> boost::asio::awaitable<void> {
        return this->registerInstance(ctx);
    });

    router.PUT("/ping?service_name={}&instance_id={}&version={}", [this](RequestContext& ctx) -> boost::asio::awaitable<void> {
        return this->ping(ctx);
    });

    router.GET("/instances?service_name={}", [this](RequestContext& ctx) -> boost::asio::awaitable<void> {
        return this->getInstances(ctx);
    });

    router.PUT("/deregister?service_name={}&instance_id={}", [this](RequestContext& ctx) -> boost::asio::awaitable<void> {
        return this->deregister(ctx);
    });
}

boost::asio::awaitable<void> XingjuController::registerInstance(RequestContext& ctx) const {
    try {
        // 判断content_type
        if (auto it = ctx.request().find(http::field::content_type); it == ctx.request().end() || it->value() != "application/json") {
            auto error = xingju::ResponseResult<void>::error(ResponseState::CONTENT_TYPE_ERROR, "只接受`application/json`");
            auto value_from = boost::json::value_from(error);
            co_return ctx.json(http::status::created, value_from);
        }

        const auto body_string = ctx.request().body();
        const boost::json::value value = boost::json::parse(body_string);
        auto instance = boost::json::value_to<aizix::Instance>(value);
        SPDLOG_DEBUG("request body: {}", serialize(value));
        if (instance.instance_id.empty() || instance.service_name.empty() || instance.port == 0 || instance.port > 65535 || instance.check_interval == 0 || instance.check_critical_timeout == 0) {
            auto error = xingju::ResponseResult<void>::error("missing query param: `instance_id` or `service_name` or `port(0 - 65535)` or `check_interval(0 - UINT64_MAX)` or `check_critical_timeout(0 - UINT64_MAX)`");
            co_return ctx.json(boost::json::value_from(error));
        }

        if (instance.ip.empty()) {
            instance.ip = ctx.ip();
        }

        bool result = co_await service_instance_manager_->registerInstance(instance);
        xingju::ResponseResult<void> response_result;
        if (!result) {
            response_result = xingju::ResponseResult<void>::error(ResponseState::SERVICE_ERROR, "服务端内部错误. 注册实例失败");
        } else {
            response_result = xingju::ResponseResult<void>::success("注册实例成功");
        }

        co_return ctx.json(boost::json::value_from(response_result));
    } catch (std::exception& e) {
        SPDLOG_ERROR("registerInstance() Error{}", e.what());
        auto error = xingju::ResponseResult<void>::error(ResponseState::SERVICE_ERROR);
        co_return ctx.json(http::status::internal_server_error, boost::json::value_from(error));
    }
}

boost::asio::awaitable<void> XingjuController::ping(RequestContext& ctx) const {
    try {
        // --- 参数解析 ---
        auto service_name_opt = ctx.query_param_as<std::string>("service_name");
        auto instance_id_opt = ctx.query_param_as<std::string>("instance_id");
        auto version_opt = ctx.query_param_as<uint64_t>("version");

        if (!service_name_opt || !instance_id_opt) {
            auto error = xingju::ResponseResult<void>::error(ResponseState::ERROR_MISSING_FIELDS,"missing query param: `service_name` or `instance_id`");
            co_return ctx.json(boost::json::value_from(error));
        }

        uint64_t version = version_opt.value_or(0); // 使用 value_or 简化代码

        SPDLOG_DEBUG("version: {}", version);

        // 获取结果
        auto instances = co_await service_instance_manager_->ping(service_name_opt.value(), instance_id_opt.value(), version);

        // 成功返回
        auto success = xingju::ResponseResult<InstanceArray>::success(instances);
        co_return ctx.json(boost::json::value_from(success));

    } catch (const std::system_error& error) {
        // 特殊错误处理：实例丢失 -> 通知客户端重新注册
        if (error.code() == xingju::error::instance_not_found) {
            SPDLOG_ERROR("实例未注册: {}", error.what());
            auto response_result = xingju::ResponseResult<void>::error(ResponseState::ERROR_INSTANCE_NOT_FOUND);
            co_return ctx.json(boost::json::value_from(response_result));
        }
        throw; // 其他系统错误向上抛出
    } catch (std::exception& e) {
        SPDLOG_ERROR("ping() Error: {}", e.what());
        auto error = xingju::ResponseResult<void>::error(ResponseState::SERVICE_ERROR);
        co_return ctx.json(boost::json::value_from(error));
    }
}

boost::asio::awaitable<void> XingjuController::getInstances(RequestContext& ctx) const {
    try {
        constexpr std::string_view KEY = "service_name";
        InstanceArray instances;
        const std::vector<std::string_view> service_name_opt = ctx.queryParamList(KEY);
        if (service_name_opt.empty()) {
            instances = co_await service_instance_manager_->instances();
        } else {
            instances = co_await service_instance_manager_->instances(service_name_opt);
        }
        auto success = xingju::ResponseResult<InstanceArray>::success(std::move(instances));
        co_return ctx.json(boost::json::value_from(success));
    } catch (std::exception& e) {
        SPDLOG_ERROR("getInstances() Error{}", e.what());
        auto error = xingju::ResponseResult<void>::error(ResponseState::SERVICE_ERROR);
        co_return ctx.json(boost::json::value_from(error));
    }
}

boost::asio::awaitable<void> XingjuController::deregister(RequestContext& ctx) const {
    try {
        const auto service_name_opt = ctx.query_param_as<std::string>("service_name");
        const auto instance_id_opt = ctx.query_param_as<std::string>("instance_id");
        if (!service_name_opt || !instance_id_opt) {
            auto error = xingju::ResponseResult<void>::error(ResponseState::ERROR_MISSING_FIELDS,"missing query param: `service_name` or `instance_id`");
            co_return ctx.json(boost::json::value_from(error));
        }

        service_instance_manager_->removeInstance(*service_name_opt, *instance_id_opt);
        auto success = xingju::ResponseResult<void>::success();
        co_return ctx.json(boost::json::value_from(success));
    } catch (std::exception& e) {
        SPDLOG_ERROR("deregister() Error{}", e.what());
        auto error = xingju::ResponseResult<void>::error(ResponseState::SERVICE_ERROR);
        co_return ctx.json(boost::json::value_from(error));
    }
}
