#pragma once

#include <cmath>
#include <type_traits>

namespace winerose {

namespace detail {
template<typename> inline constexpr bool always_false_v = false;
}

template<typename T>
T ConfigManager::get(const std::string& key, const T& defaultValue) const
{
    WINEROSE_ASSERT_NOT_REALTIME();
    if constexpr (std::is_same_v<T, bool>) {
        const auto v = numericValue(key);
        return v ? (*v != 0.0) : defaultValue;
    } else if constexpr (std::is_integral_v<T>) {
        const auto v = numericValue(key);
        return v ? static_cast<T>(std::llround(*v)) : defaultValue;
    } else if constexpr (std::is_floating_point_v<T>) {
        const auto v = numericValue(key);
        return v ? static_cast<T>(*v) : defaultValue;
    } else if constexpr (std::is_same_v<T, std::string>) {
        auto v = textValue(key);
        return v ? *v : defaultValue;
    } else {
        static_assert(detail::always_false_v<T>, "ConfigManager::get supports arithmetic, bool and std::string");
    }
}

template<typename T>
void ConfigManager::set(const std::string& key, const T& value)
{
    WINEROSE_ASSERT_NOT_REALTIME();
    if constexpr (std::is_same_v<T, bool>) {
        setNumeric(key, value ? 1.0 : 0.0);
    } else if constexpr (std::is_arithmetic_v<T>) {
        setNumeric(key, static_cast<double>(value));
    } else if constexpr (std::is_convertible_v<T, std::string>) {
        setText(key, std::string(value));
    } else {
        static_assert(detail::always_false_v<T>, "ConfigManager::set supports arithmetic, bool and std::string");
    }
}

} // namespace winerose
