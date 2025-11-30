//
// Created by Aiziboy on 2025/11/29.
//

#ifndef XINGJU_ERROR_HPP
#define XINGJU_ERROR_HPP

#include <system_error>
#include <string>

namespace xingju::error {

    // 1. 定义错误枚举 (保持不变)
    enum class code {
        instance_not_found = 11,
    };

    // 2. 自定义错误类别 (继承 std::error_category)
    class category_impl final : public std::error_category {
    public:
        const char* name() const noexcept override {
            return "xingju_error";
        }

        std::string message(int ev) const override {
            switch (static_cast<code>(ev)) {
                case code::instance_not_found: return "Instance not found";
                default: return "Unknown xingju error";
            }
        }
    };

    // 3. 全局访问接口
    inline const std::error_category& category() {
        static category_impl instance;
        return instance;
    }

    // 4. 生成 std::error_code 的辅助函数
    inline std::error_code make_error_code(code c) {
        return {static_cast<int>(c), category()};
    }

    // 5. 预定义的 error_code 常量
    inline const std::error_code instance_not_found = make_error_code(code::instance_not_found);

} // namespace xingju::error

// 6. 开启枚举到 error_code 的隐式转换 (这是标准库要求的特化)
namespace std {
    template <>
    struct is_error_code_enum<xingju::error::code> : true_type {};
}

#endif //XINGJU_ERROR_HPP