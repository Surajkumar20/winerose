#pragma once

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace winerose::control {

// Value-semantic types that cross the UI boundary. All JSON-serializable (control/Json.h), so the same
// API can be served in-process (JUCE editor) or over IPC (WebView / Electron bridge) — PLAN.md §1.4.

/** A parameter's plain (real-unit) value: a number for numeric/enum/bool params, text for strings. */
struct ParamValue {
    std::variant<double, std::string> v { 0.0 };

    ParamValue() = default;
    ParamValue(double d) : v(d) {}                               // NOLINT(implicit)
    ParamValue(std::string s) : v(std::move(s)) {}               // NOLINT(implicit)
    ParamValue(const char* s) : v(std::string(s)) {}             // NOLINT(implicit)

    bool               isNumber() const { return std::holds_alternative<double>(v); }
    double             number() const { return isNumber() ? std::get<double>(v) : 0.0; }
    const std::string& text() const { static const std::string empty; return isNumber() ? empty : std::get<std::string>(v); }

    friend bool operator==(const ParamValue&, const ParamValue&) = default;
};

/** Everything a UI needs to draw and edit one parameter, without access to the params layer. */
struct ParamSchema {
    std::string nsKey;          // "Filter0.cutoff" — the identity used by every IController call
    std::string module;         // "Filter0"
    std::string key;            // "cutoff" — also the display label
    std::string group;
    std::string tooltip;
    std::string type;           // "int" | "float" | "double" | "bool" | "enum" | "string"
    double      min = 0.0;
    double      max = 1.0;
    ParamValue  defaultValue;
    std::string curve;          // "linear" | "exp" | "power" (numeric only)
    double      curveParam = 1.0;
    std::string unit;
    std::vector<std::pair<int, std::string>> choices;   // enum / bool only
    bool        automatable = true;
    int         vst3Id = -1;
};

/** A change notification. everything=true means "re-read all values" (preset load, undo of a batch, …). */
struct ParamChange {
    std::string nsKey;
    ParamValue  value;
    bool        everything = false;
};

struct Result {
    bool        ok = true;
    std::string error;

    static Result success() { return {}; }
    static Result failure(std::string message) { return {false, std::move(message)}; }
};

struct MeterSnapshot {
    float peakLeft  = 0.0f;
    float peakRight = 0.0f;
};

} // namespace winerose::control
