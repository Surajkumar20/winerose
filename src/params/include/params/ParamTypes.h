#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace winerose {

// Detects std::chrono::duration<Rep,Period>. Lets ParamRegistry::get/set store any duration
// transparently as an integer millisecond count while handing engine code a typed std::chrono
// value back — no manual int↔chrono conversion at the call site.
template<typename T> struct is_std_chrono_duration : std::false_type {};
template<typename R, typename P>
struct is_std_chrono_duration<std::chrono::duration<R, P>> : std::true_type {};

struct EnumChoice {
    int         value;
    std::string label;
};

struct NumericMeta {
    enum class SubType { INT, FLOAT, DOUBLE } subtype;
    double min_val;
    double max_val;
    // How a 0..1 normalized value (host automation, UI knob travel) maps onto [min_val, max_val].
    // Linear: min + (max-min)·n.  Exp: min·(max/min)^n (requires min > 0, else falls back to Linear).
    // Power: min + (max-min)·n^curve_param.
    enum class Curve { Linear, Exp, Power } curve = Curve::Linear;
    double      curve_param = 1.0;
    std::string unit;          // "Hz", "s", "ms", "st", "%", "dB" — display only
};

struct EnumMeta {
    std::vector<EnumChoice> choices;
    bool is_bool = false;      // registerBool() stores 0/1 as a two-choice enum
};

struct StringMeta {};

using ParamMeta = std::variant<NumericMeta, EnumMeta, StringMeta>;

// Optional trailing argument to every register*() call. Defaults keep the example call shapes working.
struct ParamOpts {
    NumericMeta::Curve curve       = NumericMeta::Curve::Linear;
    double             curve_param = 1.0;
    std::string        unit;
    bool               automatable = true;  // false → never exposed to the host as an automatable parameter
    int                vst3_id     = -1;    // Serum 2 VST3 ID block (SPEC §5.3) — mapping metadata for compat/tools
};

struct ParamDef {
    std::string key;            // unqualified key, e.g. "cutoff" — ParamRegistry namespaces it.
                                // Also what the UI displays — there is no separate display_name.
    std::string group;          // UI section label (empty = ungrouped)
    std::string tooltip;
    std::string default_value;  // stringified factory default — for hydration, reset and schema export
    ParamMeta   meta;
    bool        automatable = true;
    int         vst3_id     = -1;
};

struct ValidationError {
    std::string key;
    std::string message;
};

// --- Pure value math. No locks, no allocation for numeric/enum metas: safe on the audio thread,
//     which is why the host adapter and UIs call these instead of going through a registry. ---

double toNormalized(const ParamMeta& meta, double plain) noexcept;
double fromNormalized(const ParamMeta& meta, double normalized) noexcept;

// Numeric: clamp to [min,max], round INT. Enum: snap to the nearest registered choice value.
double clampPlain(const ParamMeta& meta, double plain) noexcept;

// Interpret user-typed text per the meta. Numeric: std::stod-style ("50" and "50.0" both work for INT).
// Enum: a registered choice value ("2") or label ("Ultra", case-insensitive). nullopt if unparseable or
// not a legal choice. Not meaningful for StringMeta (always nullopt — strings are stored verbatim).
std::optional<double> parsePlain(const ParamMeta& meta, const std::string& text);

// Display text for a plain value: shortest round-trip number, or the enum label.
std::string formatPlain(const ParamMeta& meta, double plain);

// Shortest round-trip decimal for a number (used for stringified defaults and state).
std::string formatNumber(double value);

// "int" | "float" | "double" | "bool" | "enum" | "string"
const char* typeName(const ParamMeta& meta) noexcept;

const char* curveName(NumericMeta::Curve curve) noexcept;

} // namespace winerose
