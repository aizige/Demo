//
// Created by Aiziboy on 2025/9/30.
//

#ifndef AIZIX_AIZIX_ERROR_HPP
#define AIZIX_AIZIX_ERROR_HPP

#include <system_error>
#include <string>

// =======================================================================
// 🔹 命名空间： aizix_error::network (通用网络错误)
// =======================================================================
namespace aizix_error::network {
    // 定义错误枚举
    enum class code {
        connection_timeout = 1,     // 网络连接超时
        connection_error,           // 网络连接错误
    };

    // 自定义网络错误类别 (继承 std::error_category)
    class category_impl final : public std::error_category {
    public:
        const char* name() const noexcept override {
            return "network_error";
        }

        std::string message(int ev) const override {
            switch (static_cast<code>(ev)) {
                case code::connection_timeout: return "Network connection timeout";
                case code::connection_error: return "An unknown error occurred during network connection";
                default: return "Unknown network error";
            }
        }
    };

    // 全局访问接口
    inline const std::error_category& category() {
        static category_impl instance;
        return instance;
    }

    // 为了让 error_code 能从枚举隐式构造，必须在同命名空间提供此函数 (ADL)
    inline std::error_code make_error_code(code e) {
        return {static_cast<int>(e), category()};
    }

    // 预定义的 error_code 常量
    inline const std::error_code connection_timeout = make_error_code(code::connection_timeout);
    inline const std::error_code connection_error   = make_error_code(code::connection_error);

} // namespace aizix_error::network


// =======================================================================
// 🔹 命名空间： aizix_error::h2 (HTTP/2 特定错误)
// =======================================================================
namespace aizix_error::h2 {

    // 定义错误枚举
    enum class code {
        receive_timeout = 1,        // H2请求stream响应超时
        actor_unreachable,          // actor 未唤醒
        goaway_received,            // 收到 GOAWAY 帧
        mailbox_closed,             // actor 邮箱已关闭
        connection_unusable,        // 连接处于关闭或异常状态
    };

// 自定义错误类别，继承 Boost 的 error_category
    class category_impl final : public std::error_category {
public:
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

// 提供全局访问接口，返回错误类别实例
    inline const std::error_category& category() {
        static category_impl instance;
        return instance;
    }

    //  ADL 支持函数
    inline std::error_code make_error_code(code e) {
        return {static_cast<int>(e), category()};
    }

    // 预定义的 error_code 常量
    inline const std::error_code receive_timeout     = make_error_code(code::receive_timeout);
    inline const std::error_code actor_unreachable   = make_error_code(code::actor_unreachable);
    inline const std::error_code goaway_received     = make_error_code(code::goaway_received);
    inline const std::error_code mailbox_closed      = make_error_code(code::mailbox_closed);
    inline const std::error_code connection_unusable = make_error_code(code::connection_unusable);

} // namespace aizix_error::h2


// =======================================================================
//  让枚举支持自动转换为 std::error_code (标准库集成)
// =======================================================================
namespace std {

    // 为 network::code 特化
    template <>
    struct is_error_code_enum<aizix_error::network::code> : true_type {};

    // 为 h2::code 特化
    template <>
    struct is_error_code_enum<aizix_error::h2::code> : true_type {};

} // namespace std

#endif //AIZIX_AIZIX_ERROR_HPP