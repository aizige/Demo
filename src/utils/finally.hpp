//
// Created by Aiziboy on 25-7-27.
//

#ifndef FINALLY_HPP
#define FINALLY_HPP
#include <utility>


template<typename Func>
struct Finally {
    Func func;
    bool active = true;

    // 构造函数接收一个可调用对象
    explicit Finally(Func&& f) noexcept : func(std::forward<Func>(f)) {}

    // 移动构造函数，用于转移所有权
    Finally(Finally&& other) noexcept : func(std::move(other.func)), active(other.active) {
        other.active = false;
    }

    // 析构函数，标记为 noexcept
    ~Finally() noexcept {
        if (active) {
            func();
        }
    }

    // “解除” guard 的方法
    void release() noexcept {
        active = false;
    }

    // 禁止拷贝
    Finally(const Finally&) = delete;
    Finally& operator=(const Finally&) = delete;
    Finally& operator=(Finally&&) = delete;
};

// 辅助的工厂函数，用于自动推导模板类型
template<typename Func>
Finally<Func> make_finally(Func&& f) {
    return Finally<Func>(std::forward<Func>(f));
}
#endif //FINALLY_HPP
