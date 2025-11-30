#ifndef XINGJU_RESPONSE_RESULT_HPP
#define XINGJU_RESPONSE_RESULT_HPP

#include "ResponseState.hpp"
#include <string>
#include <type_traits>
#include <optional>
#include <boost/json.hpp>

// 如果需要 Instance.hpp 里的 tag_invoke 支持，则保留引用
// #include "Instance.hpp"

namespace xingju {
    namespace json = boost::json;

    // ========================================================================
    //                          ResponseResult<T> 泛型实现
    // ========================================================================
    template <typename T>
    struct ResponseResult {
        int code{};
        std::string message;
        std::optional<T> data;

        // ---------------------- 静态工厂方法：成功场景 ----------------------

        // 1. 无数据 (Zero Copy)
        static ResponseResult<T> success(const StateInfo& state = ResponseState::SUCCESS) {
            return {state.code, state.message, std::nullopt};
        }

        // 2. 有数据 - 右值移动 (Zero Copy) -> success(std::move(vec))
        static ResponseResult<T> success(T&& data) {
            return {ResponseState::SUCCESS.code, ResponseState::SUCCESS.message, std::make_optional(std::move(data))};
        }
        
        // 3. 有数据 - 左值拷贝 (One Copy) -> success(vec)
        static ResponseResult<T> success(const T& data) {
            return {ResponseState::SUCCESS.code, ResponseState::SUCCESS.message, std::make_optional(data)};
        }

        // 4. 自定义消息 + 无数据
        static ResponseResult<T> success(std::string message) {
            return {ResponseState::SUCCESS.code, std::move(message), std::nullopt};
        }

        // 5. 自定义消息 + 右值数据 (Zero Copy)
        static ResponseResult<T> success(std::string message, T&& data) {
            return {ResponseState::SUCCESS.code, std::move(message), std::make_optional(std::move(data))};
        }

        // 6. 自定义消息 + 左值数据 (One Copy)
        static ResponseResult<T> success(std::string message, const T& data) {
            return {ResponseState::SUCCESS.code, std::move(message), std::make_optional(data)};
        }

        // ---------------------- 静态工厂方法：失败场景 ----------------------

        // 1. 仅状态
        static ResponseResult<T> error(const StateInfo& state) {
            return {state.code, state.message, std::nullopt};
        }

        // 2. 状态 + 自定义消息
        static ResponseResult<T> error(const StateInfo& state, std::string message) {
            return {state.code, std::move(message), std::nullopt};
        }

        // 3. 状态 + 右值数据 (Zero Copy)
        static ResponseResult<T> error(const StateInfo& state, T&& data) {
            return {state.code, state.message, std::make_optional(std::move(data))};
        }
        
        // 4. 状态 + 左值数据 (One Copy)
        static ResponseResult<T> error(const StateInfo& state, const T& data) {
            return {state.code, state.message, std::make_optional(data)};
        }

        // 5. 完全自定义
        static ResponseResult<T> error(const int code, std::string message) {
            return {code, std::move(message), std::nullopt};
        }


    };


    // ========================================================================
    //                          ResponseResult<void> 特化实现
    //             (void 类型没有 data，不需要 move/copy 优化，直接传值即可)
    // ========================================================================
    template <>
    struct ResponseResult<void> {
        int code;
        std::string message;

        static ResponseResult<void> success(const StateInfo& state = ResponseState::SUCCESS) {
            return {state.code, state.message};
        }

        static ResponseResult<void> success(std::string message) {
            return {ResponseState::SUCCESS.code, std::move(message)};
        }

        static ResponseResult<void> error(const StateInfo& state) {
            return {state.code, state.message};
        }

        static ResponseResult<void> error(std::string message) {
            return {ResponseState::FAILED.code, std::move(message)};
        }

        static ResponseResult<void> error(const StateInfo& state, std::string message) {
            return {state.code, std::move(message)};
        }

        static ResponseResult<void> error(const int code, std::string message) {
            return {code, std::move(message)};
        }
    };

    // ========================================================================
    //                    Boost.JSON 序列化支持 (保持不变)
    // ========================================================================
    template <typename T>
    void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ResponseResult<T>& result) {
        jv = {
            {"code", result.code},
            {"message", result.message},
            {"data", result.data ? boost::json::value_from(*result.data) : boost::json::value(nullptr)}
        };
    }

    template <typename T>
    ResponseResult<T> tag_invoke(boost::json::value_to_tag<ResponseResult<T>>, const boost::json::value& jv) {
        const auto& obj = jv.as_object();
        ResponseResult<T> result;
        result.code = static_cast<int>(obj.at("code").as_int64());
        result.message = boost::json::value_to<std::string>(obj.at("message"));
        if (obj.contains("data") && !obj.at("data").is_null()) {
            result.data = boost::json::value_to<T>(obj.at("data"));
        }
        return result;
    }

    inline void tag_invoke(boost::json::value_from_tag, boost::json::value& jv, const ResponseResult<void>& result) {
        jv = {
            {"code", result.code},
            {"message", result.message},
            {"data", boost::json::value(nullptr)}
        };
    }

    inline ResponseResult<void> tag_invoke(boost::json::value_to_tag<ResponseResult<void>>, const boost::json::value& jv) {
        const auto& obj = jv.as_object();
        return {
            static_cast<int>(obj.at("code").as_int64()),
            boost::json::value_to<std::string>(obj.at("message"))
        };
    }
}

#endif //XINGJU_RESPONSE_RESULT_HPP