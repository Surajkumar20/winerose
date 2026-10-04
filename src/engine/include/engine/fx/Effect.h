#pragma once

#include <array>
#include <memory>
#include <string>

namespace winerose::fx {

inline constexpr int kParamCount   = 8;    // generic per-slot parameters p0..p7, each 0..1
inline constexpr int kSlotsPerRack = 8;
inline constexpr int kRackCount    = 3;    // Main, Bus 1, Bus 2 (Serum 2's FXRack0..2)

/**
 * @brief Effect types (SPEC §1.8). Winerose's own list and order (see PLAN.md "Enum ownership"); Serum's
 *        FX are mapped by the compat layer. Append only.
 *
 * Splitters don't process audio themselves: they divide the signal into bands and each band runs through
 * one of the slots that FOLLOW the splitter (band 1 → next slot, band 2 → the one after, ...); the bands are
 * then summed (or M/S-decoded) and processing continues after those slots.
 */
enum class FxType : int {
    None = 0, Distortion, Flanger, Phaser, Chorus, Delay, Compressor, Multiband, Reverb, Eq, Filter,
    Hyper, Bode, Convolve, Utility, SplitLowHigh, SplitLowMidHigh, SplitMidSide, Count
};

inline constexpr const char* kFxTypeNames[] = {
    "None", "Distortion", "Flanger", "Phaser", "Chorus", "Delay", "Compressor", "Multiband", "Reverb", "EQ",
    "Filter", "Hyper/Dimension", "Bode Shifter", "Convolve", "Utility", "Split Low/High", "Split Low/Mid/High",
    "Split Mid/Side",
};
static_assert(sizeof(kFxTypeNames) / sizeof(kFxTypeNames[0]) == static_cast<int>(FxType::Count));

constexpr bool isSplitter(FxType t) noexcept { return t >= FxType::SplitLowHigh && t <= FxType::SplitMidSide; }
constexpr int  splitterBands(FxType t) noexcept
{
    return t == FxType::SplitLowMidHigh ? 3 : (isSplitter(t) ? 2 : 0);
}

/** What each generic parameter means for a type (for UIs and tooltips); empty = unused. */
const std::array<const char*, kParamCount>& paramNames(FxType type) noexcept;

/**
 * @brief How a generic 0..1 slot parameter maps to what it controls, so UIs can label and display it
 *        ("High-pass", "120 Hz", "-6.0 dB"). Must mirror the effect's own setParams() mapping.
 */
struct ParamSpec {
    enum class Kind : unsigned char {
        Unused, Linear, Exponential, BipolarDb, Percent, Choice, Toggle,
        UtilityGain, BodeShift, DelayTime, Ratio, FilterType, ExpOffLow, ExpOffHigh
    };
    const char* name = "";
    Kind kind = Kind::Unused;
    float lo = 0.0f, hi = 1.0f;          // range (Linear/Exponential/ExpOff*), ±range for BipolarDb
    const char* unit = "";
    const char* const* choices = nullptr; // Choice
    int choiceCount = 0;
};

const ParamSpec& paramSpec(FxType type, int index) noexcept;

/**
 * @brief Display text for parameter `index` of a slot of `type`, given all eight values (some displays
 *        depend on another parameter, e.g. the delay time on the sync switch).
 */
std::string formatParam(FxType type, int index, const std::array<float, kParamCount>& values);

/** Default p0..p7 when a slot is switched to a type (applied by the UI / preset layer; engine reads values). */
const std::array<float, kParamCount>& defaultParams(FxType type) noexcept;

struct FxContext {
    double bpm = 120.0;
};

/**
 * @class Effect
 * @brief One effect instance in a rack slot. Created and prepared on the message thread, then owned by the
 *        audio thread (handed over through ObjectExchange). process() is realtime-safe.
 */
class Effect {
public:
    virtual ~Effect() = default;

    /** Non-realtime: allocate for this sample rate. */
    virtual void prepare(double sampleRate) = 0;
    virtual void reset() noexcept = 0;

    /** Control tick: the slot's p0..p7 (each 0..1). Implementations map them to real units. */
    virtual void setParams(const std::array<float, kParamCount>& p, const FxContext& ctx) noexcept = 0;

    /** In place, numSamples <= 32 (the engine's control block). */
    virtual void process(float* left, float* right, int numSamples) noexcept = 0;

    FxType type() const noexcept { return m_type; }

protected:
    explicit Effect(FxType type) : m_type(type) {}

private:
    FxType m_type;
};

/** Factory (message thread). Returns nullptr for None and the splitters (the rack handles those). */
std::unique_ptr<Effect> createEffect(FxType type, double sampleRate);

} // namespace winerose::fx
