//
// Created by Aiziboy on 25-7-27.
//

#ifndef AIZIX_FINALLY_HPP
#define AIZIX_FINALLY_HPP
#include <utility>
#include <type_traits> // For std::is_nothrow_invocable

// 使用 C++17 的 [[nodiscard]] 属性，如果创建了 guard 但未使用，编译器会发出警告
// 这样可以防止意外地创建了一个临时的、马上就被销毁的 guard
template<typename Func>
struct [[nodiscard]] Finally {
    Func func;
    bool active = true;

    // 构造函数接收一个可调用对象
    // 使用 std::move 是正确的，因为我们通常传递 lambda 右值
    explicit Finally(Func&& f) noexcept : func(std::move(f)) {}

    // 移动构造函数，用于转移所有权
    Finally(Finally&& other) noexcept : func(std::move(other.func)), active(other.active) {
        other.active = false;
    }

    // 析构函数，使用 C++17 的 if constexpr 来检查 func 是否是 noexcept
    ~Finally() noexcept {
        if (active) {
            // 这个检查是编译时的，没有运行时开销
            if constexpr (std::is_nothrow_invocable_v<Func>) {
                func(); // 如果 func 本身是 noexcept，那么析构函数也是
            } else {
                try {
                    func(); // 否则，为了遵守 noexcept 规范，捕获可能的异常
                } catch (...) {
                    // 在析构函数中抛出异常通常会导致 std::terminate()
                    // 这里我们选择压制异常，或者你可以记录日志
                }
            }
        }
    }

    // “解除” guard 的方法，原有的 release
    void release() noexcept {
        active = false;
    }

    // disarm() 是一个更通用的、表达“解除武装”意图的词
    // 我们让它直接调用 release()
    void disarm() noexcept {
        release();
    }


    // 禁止拷贝
    Finally(const Finally&) = delete;
    Finally& operator=(const Finally&) = delete;
    Finally& operator=(Finally&&) = delete;
};

// 辅助的工厂函数，用于自动推导模板类型
// 在 C++17 中，由于类模板参数推导(CTAD)，这个函数不再是必需的，
// 你可以直接写 Finally guard([&]{ ... });
// 但保留它可以提供更好的向后兼容性和清晰度
template<typename Func>
[[nodiscard]] auto make_finally(Func&& f) {
    return Finally<std::decay_t<Func>>(std::forward<Func>(f));
}
#endif //AIZIX_FINALLY_HPP