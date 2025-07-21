#ifndef CONTROLLER_HPP
#define CONTROLLER_HPP



// 向前声明 Router，而不是包含整个头文件
class Router;

class Controller {
public:

    virtual void register_routes(Router& router) = 0;

    virtual ~Controller() = default;
};

#endif // CONTROLLER_HPP