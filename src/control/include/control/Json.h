#pragma once

#include "control/ControlTypes.h"

#include <nlohmann/json.hpp>

namespace winerose::control {

// JSON mappings for every type in the IController API. This is the wire format a future
// WebView / Electron bridge speaks; keep field names stable once a UI depends on them.

inline void to_json(nlohmann::json& j, const ParamValue& v)
{
    if (v.isNumber()) j = v.number();
    else              j = v.text();
}

inline void from_json(const nlohmann::json& j, ParamValue& v)
{
    if (j.is_number())       v = ParamValue(j.get<double>());
    else if (j.is_boolean()) v = ParamValue(j.get<bool>() ? 1.0 : 0.0);
    else if (j.is_string())  v = ParamValue(j.get<std::string>());
    else                     v = ParamValue();
}

inline void to_json(nlohmann::json& j, const ParamSchema& s)
{
    j = nlohmann::json{
        {"nsKey", s.nsKey}, {"module", s.module}, {"key", s.key}, {"group", s.group},
        {"tooltip", s.tooltip}, {"type", s.type}, {"min", s.min}, {"max", s.max},
        {"default", s.defaultValue}, {"curve", s.curve}, {"curveParam", s.curveParam},
        {"unit", s.unit}, {"automatable", s.automatable}, {"vst3Id", s.vst3Id}, {"modulatable", s.modulatable},
    };
    auto choices = nlohmann::json::array();
    for (const auto& [value, label] : s.choices) choices.push_back({{"value", value}, {"label", label}});
    j["choices"] = std::move(choices);
}

inline void from_json(const nlohmann::json& j, ParamSchema& s)
{
    s.nsKey       = j.value("nsKey", std::string{});
    s.module      = j.value("module", std::string{});
    s.key         = j.value("key", std::string{});
    s.group       = j.value("group", std::string{});
    s.tooltip     = j.value("tooltip", std::string{});
    s.type        = j.value("type", std::string{});
    s.min         = j.value("min", 0.0);
    s.max         = j.value("max", 1.0);
    if (j.contains("default")) j.at("default").get_to(s.defaultValue);
    s.curve       = j.value("curve", std::string{});
    s.curveParam  = j.value("curveParam", 1.0);
    s.unit        = j.value("unit", std::string{});
    s.automatable = j.value("automatable", true);
    s.vst3Id      = j.value("vst3Id", -1);
    s.modulatable = j.value("modulatable", false);
    s.choices.clear();
    if (j.contains("choices"))
        for (const auto& c : j.at("choices"))
            s.choices.emplace_back(c.value("value", 0), c.value("label", std::string{}));
}

inline void to_json(nlohmann::json& j, const ParamChange& c)
{
    j = nlohmann::json{{"nsKey", c.nsKey}, {"value", c.value}, {"everything", c.everything}};
}

inline void from_json(const nlohmann::json& j, ParamChange& c)
{
    c.nsKey = j.value("nsKey", std::string{});
    if (j.contains("value")) j.at("value").get_to(c.value);
    c.everything = j.value("everything", false);
}

inline void to_json(nlohmann::json& j, const Result& r)
{
    j = nlohmann::json{{"ok", r.ok}, {"error", r.error}, {"message", r.message}};
}

inline void to_json(nlohmann::json& j, const MeterSnapshot& m)
{
    j = nlohmann::json{{"peakLeft", m.peakLeft}, {"peakRight", m.peakRight}};
}

} // namespace winerose::control
