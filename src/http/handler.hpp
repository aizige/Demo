#ifndef HANDLER_HPP
#define HANDLER_HPP

#include <functional>
#include <boost/asio/awaitable.hpp>

// 向前声明 RequestContext，因为它被 HandlerFunc 的签名所需要
class RequestContext;

/**
 * @brief 定义了所有请求处理函数的统一函数签名。
 *
 * 这是一个 std::function 对象，它可以包装任何可调用对象
 * （如 lambda、函数指针、成员函数指针），只要其签名匹配：
 * - 接收一个对 RequestContext 的引用。
 * - 返回一个 boost::asio::awaitable<void>，表示它是一个协程。
 */
using HandlerFunc = std::function<boost::asio::awaitable<void>(RequestContext&)>;

#endif // HANDLER_HPP