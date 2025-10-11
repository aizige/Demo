//
// Created by Aiziboy on 2025/10/11.
//

#ifndef UNTITLED1_PARSER_HPP
#define UNTITLED1_PARSER_HPP

#include <charconv>
#include <optional>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <algorithm> // for std::equal
#include <string>    // for std::string conversion

namespace param_parser {

    // 不区分大小写的string_view比较）
    inline bool isEquals(std::string_view a, std::string_view b) {
        return std::equal(a.begin(), a.end(),
                          b.begin(), b.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    }

    template<typename T>
    std::optional<T> tryParse(std::string_view sv) {
        // 对于数字类型（整数和浮点）
        if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>) {
            T value;
            auto result = std::from_chars(sv.data(), sv.data() + sv.size(), value);
            if (result.ec == std::errc() && result.ptr == sv.data() + sv.size()) {
                return value;
            }
        }
        // 对于 bool
        else if constexpr (std::is_same_v<T, bool>) {
            if (isEquals(sv, "true") || sv == "1") return true;
            if (isEquals(sv, "false") || sv == "0") return false;
        }
        // 对于 std::string （创建副本）
        else if constexpr (std::is_same_v<T, std::string>) {
            return std::string{sv};
        }
        // 对于 std::string_view 直接返回
        else if constexpr (std::is_same_v<T, std::string_view>) {
            return sv;
        }

        // All other types fail to parse
        return std::nullopt;
    }

} // namespace utils


#endif //UNTITLED1_PARSER_HPP