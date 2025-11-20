#ifndef AIZIX_HTTP_CONTROLLER_HPP
#define AIZIX_HTTP_CONTROLLER_HPP



// 向前声明 Router，而不是包含整个头文件
class Router;
namespace aizix {
    class HttpController {
    public:

        virtual void registerRoutes(Router& router) = 0;

        virtual ~HttpController() = default;
    };
}
#endif // AIZIX_HTTP_CONTROLLER_HPP