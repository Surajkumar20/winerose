#pragma once

// Serum → Winerose compatibility tables (SPEC §5.6, "compat/serum_tables.hpp").
//
// Every entry carries a provenance status (SPEC §5.0):
//   VERIFIED      measured against a licensed Serum 2 with tools/measure_host
//   INFERRED      derived from published third-party format facts or naming, not yet measured
//   TODO-MEASURE  placeholder that needs a measurement before it can be trusted
//
// STATE OF KNOWLEDGE (2026-10): the container format is public and verified by several independent tools,
// but the kParam* key names inside plainParams, their value curves and Serum's menu orders are NOT published
// (KennethWussmann/serum-preset-packager: "what are valid properties is unknown"; the only project with name
// tables, serum2vital, is GPL-3.0 and therefore off-limits for this clean-room build). So:
//   * kExplicit is empty until measured — add VERIFIED rows here as measure_host data arrives;
//   * kSynonyms maps plausible key names by normalized spelling (INFERRED, reported as such on import);
//   * anything unmapped is preserved verbatim in the Winerose state ("Serum2.<module>.<key>") so a later,
//     better table can re-import it without the original file.

#include <string_view>

namespace winerose::presets::serum {

enum class Status { Verified, Inferred, TodoMeasure };

// How a stored value becomes a Winerose plain value.
enum class Transform {
    Identity,        // already in Winerose's plain units
    Normalized,      // 0..1 → fromNormalized(destination's curve)
};

struct Explicit {
    std::string_view module;    // Serum CBOR module ("Oscillator0"), or "*" for any module of the family
    std::string_view key;       // kParam* name
    std::string_view target;    // Winerose key within the mapped registry ("level")
    Transform transform;
    Status status;
};

// Measured mappings go here (none yet — see the note above).
inline constexpr Explicit kExplicit[] = {
    {"", "", "", Transform::Identity, Status::TodoMeasure},   // sentinel; ignored
};

// Module families: CBOR module name prefix → Winerose registry name prefix (same index). INFERRED from the
// module list in SPEC §2.1 (Oscillator0..4 = A/B/C/Noise/Sub, Env0.., LFO*, Macro0..7) — Winerose uses the
// same names on purpose, so most modules map one to one.
struct Family { std::string_view serumPrefix; std::string_view wineroseRegistry; };
inline constexpr Family kFamilies[] = {
    {"Oscillator", "Oscillator"}, {"Env", "Env"}, {"LFO", "LFO"}, {"Macro", "Macro"},
};

// Normalized key spelling (lower case, "kparam" prefix and non-alphanumerics removed) → Winerose key, per
// family. All INFERRED: plausible names for the same control; values are assumed to be plain units.
struct Synonym { std::string_view family; std::string_view key; std::string_view target; };
inline constexpr Synonym kSynonyms[] = {
    // Oscillators A/B/C (Oscillator0..2)
    {"Oscillator", "volume", "level"}, {"Oscillator", "level", "level"}, {"Oscillator", "vol", "level"},
    {"Oscillator", "pan", "pan"},
    {"Oscillator", "octave", "octave"}, {"Oscillator", "oct", "octave"},
    {"Oscillator", "semi", "semi"}, {"Oscillator", "semitone", "semi"}, {"Oscillator", "semitones", "semi"},
    {"Oscillator", "fine", "fine"}, {"Oscillator", "finetune", "fine"}, {"Oscillator", "cents", "fine"},
    {"Oscillator", "coarse", "coarse"}, {"Oscillator", "coarsepitch", "coarse"},
    {"Oscillator", "wtpos", "wtPos"}, {"Oscillator", "wavetablepos", "wtPos"}, {"Oscillator", "wavetableposition", "wtPos"},
    {"Oscillator", "tablepos", "wtPos"}, {"Oscillator", "wtposition", "wtPos"},
    {"Oscillator", "unison", "unison"}, {"Oscillator", "unisonvoices", "unison"}, {"Oscillator", "numunison", "unison"},
    {"Oscillator", "unisondetune", "uniDetune"}, {"Oscillator", "unidetune", "uniDetune"}, {"Oscillator", "detune", "uniDetune"},
    {"Oscillator", "unisonblend", "uniBlend"}, {"Oscillator", "uniblend", "uniBlend"}, {"Oscillator", "blend", "uniBlend"},
    {"Oscillator", "unisonwidth", "uniWidth"}, {"Oscillator", "uniwidth", "uniWidth"},
    {"Oscillator", "phase", "phase"}, {"Oscillator", "randomphase", "random"}, {"Oscillator", "random", "random"},
    {"Oscillator", "enable", "enabled"}, {"Oscillator", "enabled", "enabled"}, {"Oscillator", "on", "enabled"},
    {"Oscillator", "warpamount", "warp1Amount"}, {"Oscillator", "warp1amount", "warp1Amount"}, {"Oscillator", "warp2amount", "warp2Amount"},
    // Envelopes
    {"Env", "attack", "attack"}, {"Env", "atk", "attack"}, {"Env", "hold", "hold"}, {"Env", "decay", "decay"},
    {"Env", "dec", "decay"}, {"Env", "sustain", "sustain"}, {"Env", "sus", "sustain"}, {"Env", "release", "release"},
    {"Env", "rel", "release"},
    // LFOs
    {"LFO", "rate", "rate"}, {"LFO", "phase", "phase"}, {"LFO", "delay", "delay"}, {"LFO", "rise", "rise"},
    {"LFO", "smooth", "smooth"},
    // Macros
    {"Macro", "value", "value"}, {"Macro", "macro", "value"}, {"Macro", "amount", "value"},
};

} // namespace winerose::presets::serum
