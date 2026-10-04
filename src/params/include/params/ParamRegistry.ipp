#pragma once

#include <type_traits>

namespace winerose {

template<typename T>
T ParamRegistry::get(const std::string& key) const
{
    WINEROSE_ASSERT_NOT_REALTIME();
    const std::string ns = namespacedKey(key);
    if constexpr (is_std_chrono_duration<T>::value) {
        const auto ms = m_config_->get<long long>(ns, 0);
        return std::chrono::duration_cast<T>(std::chrono::milliseconds(ms));
    } else if constexpr (std::is_enum_v<T>) {
        return static_cast<T>(m_config_->get<int>(ns, 0));
    } else {
        return m_config_->get<T>(ns, T{});
    }
}

template<typename T>
void ParamRegistry::set(const std::string& key, const T& value)
{
    WINEROSE_ASSERT_NOT_REALTIME();
    const auto meta = metaFor(key);   // copied out: never hold m_mutex_ while calling ConfigManager
    if (!meta) return;

    if constexpr (std::is_convertible_v<T, std::string>) {
        if (std::holds_alternative<StringMeta>(*meta)) {
            m_config_->set<std::string>(namespacedKey(key), std::string(value));
        } else if (auto parsed = parsePlain(*meta, std::string(value))) {
            setPlain(key, *meta, *parsed);
        }
    } else if (std::holds_alternative<StringMeta>(*meta)) {
        return;   // numeric value for a string param: ignored
    } else if constexpr (is_std_chrono_duration<T>::value) {
        setPlain(key, *meta, static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(value).count()));
    } else if constexpr (std::is_enum_v<T>) {
        setPlain(key, *meta, static_cast<double>(static_cast<int>(value)));
    } else if constexpr (std::is_same_v<T, bool>) {
        setPlain(key, *meta, value ? 1.0 : 0.0);
    } else {
        static_assert(std::is_arithmetic_v<T>, "ParamRegistry::set: unsupported type");
        setPlain(key, *meta, static_cast<double>(value));
    }
}

} // namespace winerose
