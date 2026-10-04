#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>

namespace winerose {

/**
 * @class GlobalSettings
 * @brief Machine-level settings shared by every Winerose instance: Serum content folders for the
 *        AssetResolver, default quality, UI scale, …
 *
 * This is the example ConfigManager's original job, kept almost verbatim: a simple key = value INI
 * file with write-through on every set() (PLAN.md §3.2). It is deliberately separate from the
 * per-instance ConfigManager — these values belong to the machine, not to one patch, and are never
 * part of host state or presets. The caller chooses the file location (the JUCE adapter passes the
 * user application-data folder), so this class stays JUCE-free.
 */
class GlobalSettings {
public:
    explicit GlobalSettings(std::filesystem::path file);

    /** Re-read the file. Missing file = empty settings (not an error). @return false on a read error. */
    bool load();

    /** Write all settings, creating parent directories. @return false on failure. */
    bool save() const;

    template<typename T>
    T get(const std::string& key, const T& defaultValue) const
    {
        const auto raw = getRaw(key);
        if (!raw) return defaultValue;
        if constexpr (std::is_same_v<T, std::string>) {
            return *raw;
        } else if constexpr (std::is_same_v<T, bool>) {
            return *raw == "true" || *raw == "1";
        } else if constexpr (std::is_integral_v<T>) {
            try { return static_cast<T>(std::stoll(*raw)); } catch (...) { return defaultValue; }
        } else if constexpr (std::is_floating_point_v<T>) {
            try { return static_cast<T>(std::stod(*raw)); } catch (...) { return defaultValue; }
        } else {
            static_assert(std::is_same_v<T, std::string>, "GlobalSettings::get: unsupported type");
        }
    }

    /** Sets and immediately persists (write-through). */
    template<typename T>
    void set(const std::string& key, const T& value)
    {
        if constexpr (std::is_convertible_v<T, std::string>) {
            setRaw(key, std::string(value));
        } else if constexpr (std::is_same_v<T, bool>) {
            setRaw(key, value ? "true" : "false");
        } else {
            static_assert(std::is_arithmetic_v<T>, "GlobalSettings::set: unsupported type");
            setRaw(key, std::to_string(value));
        }
    }

    const std::filesystem::path& filePath() const { return m_file_; }

private:
    std::optional<std::string> getRaw(const std::string& key) const;
    void setRaw(const std::string& key, const std::string& value);

    std::filesystem::path              m_file_;
    std::map<std::string, std::string> m_data_;
    mutable std::mutex                 m_mutex_;
};

} // namespace winerose
