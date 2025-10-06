//
// Created by Aiziboy on 2025/9/30.
//

#ifndef UNTITLED1_MY_ERROR_HPP
#define UNTITLED1_MY_ERROR_HPP

#include <boost/system/error_code.hpp>
#include <string>

namespace my_error {
namespace h2 {

// 🔹 定义错误枚举（仅用于内部标识）
enum class code {
    receive_timeout = 1,        // H2请求stream响应超时，未收到响应
    actor_unreachable,         // actor 未唤醒或邮箱卡住
    goaway_received,           // 收到 GOAWAY 帧，连接不可继续使用
    mailbox_closed,            // actor 邮箱已关闭，无法发送请求
    connection_unusable        // 连接处于关闭或异常状态
};

// 🔹 自定义错误类别，继承 Boost 的 error_category
class category_impl : public boost::system::error_category {
public:
    virtual ~category_impl() = default;
    // 返回错误类别名称（用于日志和调试）
    const char* name() const noexcept override {
        return "h2_error";
    }

    // 根据错误枚举值返回对应的错误信息
    std::string message(int ev) const override {
        switch (static_cast<code>(ev)) {
            case code::receive_timeout: return "H2 stream receive timeout";
            case code::actor_unreachable: return "H2 actor unreachable";
            case code::goaway_received: return "H2 GOAWAY received";
            case code::mailbox_closed: return "H2 mailbox closed";
            case code::connection_unusable: return "H2 connection unusable";
            default: return "Unknown H2 error";
        }
    }
};

// 🔹 提供全局访问接口，返回错误类别实例
inline const boost::system::error_category& category() {
    static category_impl instance;
    return instance;
}

// 🔹 直接暴露 error_code 常量，供外部使用（无需 make_error_code）
inline const boost::system::error_code receive_timeout{
    static_cast<int>(code::receive_timeout), category()
};

inline const boost::system::error_code actor_unreachable{
    static_cast<int>(code::actor_unreachable), category()
};

inline const boost::system::error_code goaway_received{
    static_cast<int>(code::goaway_received), category()
};

inline const boost::system::error_code mailbox_closed{
    static_cast<int>(code::mailbox_closed), category()
};

inline const boost::system::error_code connection_unusable{
    static_cast<int>(code::connection_unusable), category()
};

} // namespace h2
} // namespace my_error

// 🔹 让枚举支持自动转换为 error_code（可选，便于扩展）
namespace boost::system {
    template <>
    struct is_error_code_enum<my_error::h2::code> : std::true_type {};
}

#endif //UNTITLED1_MY_ERROR_HPP