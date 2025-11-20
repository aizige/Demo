//
// Created by Aiziboy on 2025/10/10.
//

#ifndef AIZIX_PARAMETER_SET_HPP
#define AIZIX_PARAMETER_SET_HPP
#include <string>
#include <string_view>
#include <optional>
#include <charconv>
#include <stdexcept>

// 自定义异常
class BadRequestError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};


template<typename MapType>
class ParameterSet {
public:

    explicit ParameterSet(const MapType& params) : params_(params) {}


    // 获取 string_view (零拷贝)
    std::optional<std::string_view> get_sv(const std::string& key) const {
        auto it = params_.find(key);
        if (it != params_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    // --- 强大的 get<T> 方法 ---
    template<typename T>
    T get(const std::string& key) const {
        auto sv_opt = get_sv(key);
        if (!sv_opt) {
            throw BadRequestError("Missing required parameter: " + key);
        }

        if constexpr (std::is_same_v<T, std::string>) {
            return std::string(*sv_opt);
        }
        else if constexpr (std::is_same_v<T, std::string_view>) {
            return *sv_opt;
        }
        else if constexpr (std::is_integral_v<T>) {
            T value;
            auto [ptr, ec] = std::from_chars(sv_opt->data(), sv_opt->data() + sv_opt->size(), value);
            if (ec == std::errc() && ptr == sv_opt->data() + sv_opt->size()) {
                return value;
            } else {
                throw BadRequestError("Parameter '" + key + "' with value '" + std::string(*sv_opt) + "' is not a valid integer.");
            }
        }
        else if constexpr (std::is_floating_point_v<T>) {
            T value;
            try {
                value = std::stod(std::string(*sv_opt));
                return value;
            } catch (const std::exception&) {
                throw BadRequestError("Parameter '" + key + "' with value '" + std::string(*sv_opt) + "' is not a valid floating point number.");
            }
        }
        else {
            // 静态断言，如果是不支持的类型，编译时就会报错
            static_assert(!sizeof(T), "Unsupported type for parameter conversion.");

            throw std::logic_error("Should never reach here due to static_assert.");

        }
    }

    // --- 方便的 get_optional<T> ---
    template<typename T>
       std::optional<T> get_optional(const std::string& key) const noexcept {
        try {
            return get<T>(key);
        } catch (const BadRequestError&) {
            return std::nullopt;
        }
    }

    // --- 方便的 get_or_default<T> ---
    template<typename T>
    T get_or_default(const std::string& key, T&& default_value) const {
        auto opt = get_optional<T>(key);
        return opt.value_or(std::forward<T>(default_value));
    }
private:
    const MapType& params_;

};
#endif //AIZIX_PARAMETER_SET_HPP