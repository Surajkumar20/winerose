#include "params/ParamTypes.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace winerose {

namespace {

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

bool expUsable(const NumericMeta& m) noexcept { return m.min_val > 0.0 && m.max_val > m.min_val; }

// Index of the choice whose value is nearest to `plain` (exact match wins). Choices are non-empty.
std::size_t nearestChoiceIndex(const EnumMeta& m, double plain) noexcept
{
    std::size_t best = 0;
    double bestDist = std::abs(m.choices[0].value - plain);
    for (std::size_t i = 1; i < m.choices.size(); ++i) {
        const double d = std::abs(m.choices[i].value - plain);
        if (d < bestDist) { best = i; bestDist = d; }
    }
    return best;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::optional<double> parseDouble(const std::string& raw)
{
    const std::string s = trim(raw);
    if (s.empty()) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(v)) return std::nullopt;
    return v;
}

} // namespace

double toNormalized(const ParamMeta& meta, double plain) noexcept
{
    return std::visit(overloaded{
        [&](const NumericMeta& m) -> double {
            if (m.max_val <= m.min_val) return 0.0;
            const double v = std::clamp(plain, m.min_val, m.max_val);
            switch (m.curve) {
                case NumericMeta::Curve::Exp:
                    if (expUsable(m)) return std::log(v / m.min_val) / std::log(m.max_val / m.min_val);
                    break;
                case NumericMeta::Curve::Power:
                    if (m.curve_param > 0.0)
                        return std::pow((v - m.min_val) / (m.max_val - m.min_val), 1.0 / m.curve_param);
                    break;
                case NumericMeta::Curve::Linear: break;
            }
            return (v - m.min_val) / (m.max_val - m.min_val);
        },
        [&](const EnumMeta& m) -> double {
            if (m.choices.size() < 2) return 0.0;
            return static_cast<double>(nearestChoiceIndex(m, plain)) / static_cast<double>(m.choices.size() - 1);
        },
        [](const StringMeta&) -> double { return 0.0; },
    }, meta);
}

double fromNormalized(const ParamMeta& meta, double normalized) noexcept
{
    const double n = std::clamp(normalized, 0.0, 1.0);
    return std::visit(overloaded{
        [&](const NumericMeta& m) -> double {
            double v = m.min_val + (m.max_val - m.min_val) * n;
            switch (m.curve) {
                case NumericMeta::Curve::Exp:
                    if (expUsable(m)) v = m.min_val * std::pow(m.max_val / m.min_val, n);
                    break;
                case NumericMeta::Curve::Power:
                    if (m.curve_param > 0.0) v = m.min_val + (m.max_val - m.min_val) * std::pow(n, m.curve_param);
                    break;
                case NumericMeta::Curve::Linear: break;
            }
            return clampPlain(meta, v);
        },
        [&](const EnumMeta& m) -> double {
            if (m.choices.empty()) return 0.0;
            const auto idx = static_cast<std::size_t>(std::lround(n * static_cast<double>(m.choices.size() - 1)));
            return m.choices[std::min(idx, m.choices.size() - 1)].value;
        },
        [](const StringMeta&) -> double { return 0.0; },
    }, meta);
}

double clampPlain(const ParamMeta& meta, double plain) noexcept
{
    return std::visit(overloaded{
        [&](const NumericMeta& m) -> double {
            double v = std::isfinite(plain) ? plain : m.min_val;
            v = std::clamp(v, m.min_val, std::max(m.min_val, m.max_val));
            if (m.subtype == NumericMeta::SubType::INT) v = std::round(v);
            return v;
        },
        [&](const EnumMeta& m) -> double {
            if (m.choices.empty()) return 0.0;
            return m.choices[nearestChoiceIndex(m, std::isfinite(plain) ? plain : 0.0)].value;
        },
        [](const StringMeta&) -> double { return 0.0; },
    }, meta);
}

std::optional<double> parsePlain(const ParamMeta& meta, const std::string& text)
{
    return std::visit(overloaded{
        [&](const NumericMeta&) -> std::optional<double> {
            auto v = parseDouble(text);
            if (!v) return std::nullopt;
            return clampPlain(meta, *v);
        },
        [&](const EnumMeta& m) -> std::optional<double> {
            if (auto v = parseDouble(text)) {
                for (const auto& c : m.choices)
                    if (static_cast<double>(c.value) == *v) return *v;
                return std::nullopt;   // a number, but not a legal choice
            }
            const std::string wanted = lower(trim(text));
            for (const auto& c : m.choices)
                if (lower(c.label) == wanted) return static_cast<double>(c.value);
            if (m.is_bool) {
                if (wanted == "true" || wanted == "on" || wanted == "yes")  return 1.0;
                if (wanted == "false" || wanted == "off" || wanted == "no") return 0.0;
            }
            return std::nullopt;
        },
        [](const StringMeta&) -> std::optional<double> { return std::nullopt; },
    }, meta);
}

std::string formatNumber(double value)
{
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value);
    return std::string(buf, res.ptr);
}

std::string formatPlain(const ParamMeta& meta, double plain)
{
    return std::visit(overloaded{
        [&](const NumericMeta& m) -> std::string {
            if (m.subtype == NumericMeta::SubType::INT) return std::to_string(std::llround(plain));
            if (m.subtype == NumericMeta::SubType::FLOAT) return formatNumber(static_cast<float>(plain));
            return formatNumber(plain);
        },
        [&](const EnumMeta& m) -> std::string {
            if (m.choices.empty()) return {};
            return m.choices[nearestChoiceIndex(m, plain)].label;
        },
        [](const StringMeta&) -> std::string { return {}; },
    }, meta);
}

const char* typeName(const ParamMeta& meta) noexcept
{
    return std::visit(overloaded{
        [](const NumericMeta& m) -> const char* {
            switch (m.subtype) {
                case NumericMeta::SubType::INT:    return "int";
                case NumericMeta::SubType::FLOAT:  return "float";
                case NumericMeta::SubType::DOUBLE: return "double";
            }
            return "float";
        },
        [](const EnumMeta& m) -> const char* { return m.is_bool ? "bool" : "enum"; },
        [](const StringMeta&) -> const char* { return "string"; },
    }, meta);
}

const char* curveName(NumericMeta::Curve curve) noexcept
{
    switch (curve) {
        case NumericMeta::Curve::Linear: return "linear";
        case NumericMeta::Curve::Exp:    return "exp";
        case NumericMeta::Curve::Power:  return "power";
    }
    return "linear";
}

} // namespace winerose
