//
// Created by Aiziboy on 2025/11/21.
//

#ifndef XINGJU_SERVER_CONTROLLER_HPP
#define XINGJU_SERVER_CONTROLLER_HPP

#include <boost/asio/awaitable.hpp>
#include <aizix/controller/HttpController.hpp>
#include <aizix/http/router.hpp>
#include "ServiceInstanceManager.hpp"

class XingjuController final : public aizix::HttpController {
public:
    explicit XingjuController(std::shared_ptr<ServiceInstanceManager> service_instance_manager);
    void registerRoutes(Router& router) override;

private:
    boost::asio::awaitable<void> registerInstance(RequestContext& ctx) const;
    boost::asio::awaitable<void> ping(RequestContext& ctx) const;
    boost::asio::awaitable<void> getInstances(RequestContext& ctx) const;
    boost::asio::awaitable<void> deregister(RequestContext& ctx) const;

    std::shared_ptr<ServiceInstanceManager> service_instance_manager_;
};


#endif //XINGJU_SERVER_CONTROLLER_HPP