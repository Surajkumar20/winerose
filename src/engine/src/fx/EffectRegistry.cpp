#include "engine/fx/Effects.h"

#include "engine/modulation/Lfo.h"

#include <cmath>
#include <cstdio>

namespace winerose::fx {

namespace {

using K = ParamSpec::Kind;

// Choice lists (orders must match the effects' choice() calls).
constexpr const char* kDistModes[]   = {"Tube", "Soft Clip", "Hard Clip", "Diode", "Linear Fold", "Sine Fold", "Zero-Square", "Downsample"};
constexpr const char* kDistFilter[]  = {"Off", "Pre LP", "Pre BP", "Pre HP", "Post LP", "Post BP", "Post HP"};
constexpr const char* kStages[]      = {"2", "4", "6", "8", "10", "12"};
constexpr const char* kDelayModes[]  = {"Normal", "Ping-Pong", "Tap"};
constexpr const char* kReverbModes[] = {"Plate", "Hall"};
constexpr const char* kEqLow[]       = {"Low Shelf", "Peak", "High-pass"};
constexpr const char* kEqHigh[]      = {"High Shelf", "Peak", "Low-pass"};
constexpr const char* kVoices[]      = {"1", "2", "3", "4", "5", "6", "7"};

template<std::size_t N>
constexpr ParamSpec choice(const char* name, const char* const (&list)[N]) { return {name, K::Choice, 0, 1, "", list, static_cast<int>(N)}; }
constexpr ParamSpec lin(const char* name, float lo, float hi, const char* unit) { return {name, K::Linear, lo, hi, unit}; }
constexpr ParamSpec expo(const char* name, float lo, float hi, const char* unit) { return {name, K::Exponential, lo, hi, unit}; }
constexpr ParamSpec db(const char* name, float range) { return {name, K::BipolarDb, -range, range, "dB"}; }
constexpr ParamSpec pct(const char* name) { return {name, K::Percent, 0, 100, "%"}; }
constexpr ParamSpec toggle(const char* name) { return {name, K::Toggle}; }
constexpr ParamSpec special(const char* name, K kind, float lo = 0, float hi = 1, const char* unit = "") { return {name, kind, lo, hi, unit}; }
constexpr ParamSpec unused() { return {}; }

using Specs = std::array<ParamSpec, kParamCount>;
using Defaults = std::array<float, kParamCount>;
struct TypeInfo { Specs specs; Defaults defaults; };

// Generic slot parameters p0..p7 per type — mirrors each effect's setParams(). Defaults are neutral-ish
// starting points (TODO-MEASURE against Serum's).
const TypeInfo kInfo[] = {
    /* None */ {{unused(), unused(), unused(), unused(), unused(), unused(), unused(), unused()}, {0, 0, 0, 0, 0, 0, 0, 0}},
    /* Distortion */ {{lin("Drive", 0, 48, "dB"), choice("Mode", kDistModes), choice("Filter", kDistFilter),
                       expo("Filter Freq", 20, 20000, "Hz"), expo("Filter Q", 0.5f, 10, ""), lin("DC Bias", -1, 1, ""),
                       db("Output", 24), unused()}, {0.3f, 0.2f, 0, 0.5f, 0.3f, 0.5f, 0.5f, 0}},
    /* Flanger */ {{expo("Rate", 0.01f, 20, "Hz"), pct("Depth"), lin("Feedback", -95, 95, "%"), lin("Stereo Phase", 0, 180, "deg"),
                    unused(), unused(), unused(), unused()}, {0.35f, 0.5f, 0.6f, 0.5f, 0, 0, 0, 0}},
    /* Phaser */ {{expo("Rate", 0.01f, 10, "Hz"), pct("Depth"), expo("Centre", 100, 8000, "Hz"), lin("Feedback", -95, 95, "%"),
                   lin("Stereo Phase", 0, 180, "deg"), choice("Stages", kStages), unused(), unused()},
                  {0.35f, 0.6f, 0.5f, 0.6f, 0.5f, 0.3f, 0, 0}},
    /* Chorus */ {{expo("Rate", 0.05f, 5, "Hz"), pct("Depth"), lin("Delay 1", 1, 30, "ms"), lin("Delay 2", 1, 30, "ms"),
                   lin("Feedback", 0, 90, "%"), expo("Low-pass", 200, 20000, "Hz"), unused(), unused()},
                  {0.4f, 0.4f, 0.25f, 0.45f, 0, 1.0f, 0, 0}},
    /* Delay */ {{choice("Mode", kDelayModes), toggle("Sync"), special("Time", K::DelayTime), expo("Right/Left", 0.25f, 4, "x"),
                  lin("Feedback", 0, 98, "%"), expo("Filter Freq", 100, 10000, "Hz"), special("Filter Width", K::ExpOffLow, 0.3f, 4, ""),
                  unused()}, {0, 0, 0.6f, 0.5f, 0.4f, 0.5f, 0, 0}},
    /* Compressor */ {{lin("Threshold", -60, 0, "dB"), special("Ratio", K::Ratio, 1, 20), expo("Attack", 0.1f, 100, "ms"),
                       expo("Release", 5, 1000, "ms"), lin("Makeup", 0, 36, "dB"), lin("Knee", 0, 12, "dB"), unused(), unused()},
                      {0.67f, 0.46f, 0.5f, 0.5f, 0, 0.25f, 0, 0}},
    /* Multiband */ {{pct("Depth"), expo("Time", 0.1f, 10, "x"), pct("Upward"), pct("Downward"), db("Low Gain", 12),
                      db("Mid Gain", 12), db("High Gain", 12), db("Output", 12)}, {0.5f, 0.5f, 1.0f, 1.0f, 0.5f, 0.5f, 0.5f, 0.5f}},
    /* Reverb */ {{choice("Mode", kReverbModes), expo("Decay", 0.1f, 20, "s"), pct("Size"), lin("Pre-delay", 0, 250, "ms"),
                   expo("Low Cut", 20, 1000, "Hz"), expo("High Cut", 1000, 20000, "Hz"), pct("Damping"), pct("Width")},
                  {0, 0.45f, 0.5f, 0, 0, 1.0f, 0.3f, 1.0f}},
    /* EQ */ {{choice("Low Type", kEqLow), expo("Low Freq", 20, 2000, "Hz"), db("Low Gain", 24), expo("Low Q", 0.3f, 10, ""),
               choice("High Type", kEqHigh), expo("High Freq", 500, 20000, "Hz"), db("High Gain", 24), expo("High Q", 0.3f, 10, "")},
              {0, 0.33f, 0.5f, 0.35f, 0, 0.6f, 0.5f, 0.35f}},
    /* Filter */ {{special("Type", K::FilterType), expo("Cutoff", 20, 20000, "Hz"), pct("Resonance"), pct("Drive"), pct("Var"),
                   pct("Stereo"), pct("X"), pct("Y")},
                  {static_cast<float>(winerose::dsp::FilterType::Lp12) / static_cast<float>(winerose::dsp::FilterType::Count) + 0.001f,
                   0.7f, 0.1f, 0, 0.5f, 0, 0, 0.5f}},
    /* Hyper */ {{expo("Rate", 0.1f, 5, "Hz"), pct("Detune"), choice("Unison", kVoices), pct("Hyper Mix"), unused(),
                  lin("Dimension Size", 20, 100, "%"), pct("Dimension Mix"), unused()}, {0.5f, 0.3f, 0.5f, 0.5f, 0, 0.5f, 0, 0}},
    /* Bode */ {{special("Shift", K::BodeShift), pct("Lower Sideband"), lin("Feedback", 0, 90, "%"), unused(), unused(), unused(),
                 unused(), unused()}, {0.6f, 0, 0, 0, 0, 0, 0, 0}},
    /* Convolve */ {{expo("Length", 0.05f, 2.5f, "s"), lin("Pre-delay", 0, 250, "ms"), special("Low Cut", K::ExpOffLow, 20, 1000, "Hz"),
                     special("High Cut", K::ExpOffHigh, 1000, 20000, "Hz"), unused(), unused(), unused(), unused()},
                    {0.6f, 0, 0, 1.0f, 0, 0, 0, 0}},
    /* Utility */ {{special("Gain", K::UtilityGain), lin("Pan", -100, 100, "%"), lin("Width", 0, 200, "%"), toggle("Invert L"),
                    toggle("Invert R"), toggle("Swap L/R"), special("Bass Mono", K::ExpOffLow, 20, 500, "Hz"), unused()},
                   {0.8f, 0.5f, 0.5f, 0, 0, 0, 0, 0}},
    /* Split L/H */ {{expo("Crossover", 20, 20000, "Hz"), db("Low Level", 24), db("High Level", 24), unused(), unused(), unused(),
                      unused(), unused()}, {0.5f, 0.5f, 0.5f, 0, 0, 0, 0, 0}},
    /* Split L/M/H */ {{expo("Low Crossover", 20, 20000, "Hz"), expo("High Crossover", 20, 20000, "Hz"), db("Low Level", 24),
                        db("Mid Level", 24), db("High Level", 24), unused(), unused(), unused()}, {0.3f, 0.7f, 0.5f, 0.5f, 0.5f, 0, 0, 0}},
    /* Split M/S */ {{db("Mid Level", 24), db("Side Level", 24), unused(), unused(), unused(), unused(), unused(), unused()},
                     {0.5f, 0.5f, 0, 0, 0, 0, 0, 0}},
};
static_assert(sizeof(kInfo) / sizeof(kInfo[0]) == static_cast<int>(FxType::Count));

std::array<std::array<const char*, kParamCount>, static_cast<int>(FxType::Count)> buildNames()
{
    std::array<std::array<const char*, kParamCount>, static_cast<int>(FxType::Count)> out {};
    for (int t = 0; t < static_cast<int>(FxType::Count); ++t)
        for (int i = 0; i < kParamCount; ++i) out[static_cast<std::size_t>(t)][static_cast<std::size_t>(i)] = kInfo[t].specs[static_cast<std::size_t>(i)].name;
    return out;
}

std::string number(float v, const char* unit)
{
    char buf[48];
    const float a = std::abs(v);
    if (std::string(unit) == "Hz" && a >= 1000.0f) std::snprintf(buf, sizeof(buf), "%.2f kHz", v / 1000.0f);
    else if (std::string(unit) == "Hz") std::snprintf(buf, sizeof(buf), a < 10 ? "%.2f Hz" : "%.0f Hz", v);
    else if (std::string(unit) == "dB") std::snprintf(buf, sizeof(buf), "%+.1f dB", v);
    else if (std::string(unit) == "ms" && a >= 1000.0f) std::snprintf(buf, sizeof(buf), "%.2f s", v / 1000.0f);
    else if (std::string(unit) == "ms") std::snprintf(buf, sizeof(buf), a < 10 ? "%.1f ms" : "%.0f ms", v);
    else if (std::string(unit) == "s") std::snprintf(buf, sizeof(buf), "%.2f s", v);
    else if (std::string(unit) == "%" || std::string(unit) == "deg") std::snprintf(buf, sizeof(buf), "%.0f%s", v, std::string(unit) == "%" ? "%" : " deg");
    else if (std::string(unit) == "x") std::snprintf(buf, sizeof(buf), "%.2fx", v);
    else std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

int choiceIndex(float p, int count) { return std::clamp(static_cast<int>(p * count), 0, count - 1); }
float expMap(float p, float lo, float hi) { return lo * std::pow(hi / lo, std::clamp(p, 0.0f, 1.0f)); }

} // namespace

const std::array<const char*, kParamCount>& paramNames(FxType type) noexcept
{
    static const auto names = buildNames();
    return names[static_cast<std::size_t>(type)];
}

const std::array<float, kParamCount>& defaultParams(FxType type) noexcept
{
    return kInfo[static_cast<int>(type)].defaults;
}

const ParamSpec& paramSpec(FxType type, int index) noexcept
{
    return kInfo[static_cast<int>(type)].specs[static_cast<std::size_t>(std::clamp(index, 0, kParamCount - 1))];
}

std::string formatParam(FxType type, int index, const std::array<float, kParamCount>& values)
{
    const ParamSpec& s = paramSpec(type, index);
    const float p = std::clamp(values[static_cast<std::size_t>(std::clamp(index, 0, kParamCount - 1))], 0.0f, 1.0f);
    switch (s.kind) {
        case K::Unused:      return "-";
        case K::Linear:      return number(s.lo + (s.hi - s.lo) * p, s.unit);
        case K::Exponential: return number(expMap(p, s.lo, s.hi), s.unit);
        case K::Percent:     return number(100.0f * p, "%");
        case K::BipolarDb: {
            const float v = (p - 0.5f) * 2.0f * s.hi;
            return number(std::abs(v) < 1e-6f ? 0.0f : v, "dB");
        }
        case K::Choice:      return s.choices[choiceIndex(p, s.choiceCount)];
        case K::Toggle:      return p >= 0.5f ? "On" : "Off";
        case K::UtilityGain: {
            const float v = 60.0f * p - 48.0f;
            return number(std::abs(v) < 1e-4f ? 0.0f : v, "dB");
        }
        case K::BodeShift: {
            const float x = 2.0f * p - 1.0f;
            const float hz = (x < 0 ? -1.0f : 1.0f) * 5000.0f * std::abs(x * x * x);
            return (hz >= 0 ? "+" : "") + number(hz, "Hz");
        }
        case K::DelayTime:
            if (values[1] >= 0.5f) return modulation::kSyncDivisions[choiceIndex(p, modulation::kSyncDivisionCount)].name;
            return number(expMap(p, 1.0f, 2000.0f), "ms");
        case K::Ratio:
            return p > 0.99f ? std::string("Limit") : number(expMap(p, s.lo, s.hi), "") + ":1";
        case K::FilterType:
            return winerose::dsp::kFilterInfo[choiceIndex(p, static_cast<int>(winerose::dsp::FilterType::Count))].name;
        case K::ExpOffLow:
            return p <= 0.01f ? std::string("Off") : number(expMap(p, s.lo, s.hi), s.unit);
        case K::ExpOffHigh:
            return p >= 0.999f ? std::string("Off") : number(expMap(p, s.lo, s.hi), s.unit);
    }
    return {};
}

std::unique_ptr<Effect> createEffect(FxType type, double sampleRate)
{
    std::unique_ptr<Effect> e;
    switch (type) {
        case FxType::Distortion: e = std::make_unique<Distortion>(); break;
        case FxType::Flanger:    e = std::make_unique<Flanger>(); break;
        case FxType::Phaser:     e = std::make_unique<Phaser>(); break;
        case FxType::Chorus:     e = std::make_unique<Chorus>(); break;
        case FxType::Delay:      e = std::make_unique<Delay>(); break;
        case FxType::Compressor: e = std::make_unique<Compressor>(); break;
        case FxType::Multiband:  e = std::make_unique<Multiband>(); break;
        case FxType::Reverb:     e = std::make_unique<Reverb>(); break;
        case FxType::Eq:         e = std::make_unique<Eq>(); break;
        case FxType::Filter:     e = std::make_unique<FilterFx>(); break;
        case FxType::Hyper:      e = std::make_unique<Hyper>(); break;
        case FxType::Bode:       e = std::make_unique<Bode>(); break;
        case FxType::Convolve:   e = std::make_unique<Convolve>(); break;
        case FxType::Utility:    e = std::make_unique<Utility>(); break;
        default:                 return nullptr;   // None and splitters: handled by the rack
    }
    e->prepare(sampleRate);
    e->setParams(defaultParams(type), FxContext{});
    return e;
}

} // namespace winerose::fx
