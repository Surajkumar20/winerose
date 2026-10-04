#pragma once

#include <algorithm>
#include <cmath>

namespace winerose::dsp {

/**
 * @brief Serum-1-style oscillator warps (SPEC §1.2), as pure per-sample functions.
 *
 * A warp maps the oscillator's phase p in [0,1) to a new read phase and/or an amplitude multiplier.
 * Two warp slots chain: slot 1's output phase feeds slot 2, amplitudes multiply. Amount k is 0..1;
 * k = 0 is always "no effect" (Bend+/- and Asym+/- are bipolar around k = 0.5 instead).
 *
 * All formulas are the SPEC's reconstructions (INFERRED), not Xfer's code, and the menu ORDER is
 * TODO-MEASURE: the enum order becomes the automation/preset mapping, so confirm it against Serum's
 * warp menu with measure_host before release. Remap 1/2 use an identity curve until drawable curves land
 * with the modulation work (Phase 3), which makes Remap 1 a no-op and Remap 2 equal to Mirror for now.
 */
enum class WarpMode : int {
    Off = 0,
    Sync,           // self sync: plays faster, restarts each cycle
    WindowSync,     // sync with a Hann window over the original cycle
    BendPlus,       // pinch toward the middle
    BendMinus,      // pinch away from the middle
    BendPlusMinus,  // bipolar bend (k = 0.5 neutral)
    Pwm,            // squeeze the cycle left, silence the remainder
    AsymPlus,       // bend the whole cycle right
    AsymMinus,      // bend the whole cycle left
    AsymPlusMinus,  // bipolar asym (k = 0.5 neutral)
    Flip,           // polarity flip from position 1-k
    Mirror,         // second half mirrors the first
    Remap1,         // drawable curve (identity until Phase 3)
    Remap2,         // mirrored curve
    Remap3,         // sinusoidal remap
    Remap4,         // four sinusoidal segments
    Quantize,       // sample-and-hold the phase: 2048 → 2 steps
    Fm,             // phase modulation from the paired oscillator (A←B, B←A, C←A)
    Am,             // amplitude modulation from the paired oscillator
    Rm,             // ring modulation from the paired oscillator
    FmNoise,        // phase modulation from the noise oscillator
    FmSub,          // phase modulation from the sub oscillator
    Count
};

inline constexpr const char* kWarpNames[] = {
    "Off", "Sync", "Window Sync", "Bend +", "Bend -", "Bend +/-", "PWM", "Asym +", "Asym -", "Asym +/-",
    "Flip", "Mirror", "Remap 1", "Remap 2", "Remap 3", "Remap 4", "Quantize", "FM", "AM", "RM",
    "FM (Noise)", "FM (Sub)",
};
static_assert(sizeof(kWarpNames) / sizeof(kWarpNames[0]) == static_cast<int>(WarpMode::Count));

struct WarpConstants {
    static constexpr double kSyncMaxRatio = 8.0;   // INFERRED (SPEC: R ≈ 8-16)
    static constexpr double kBendRange    = 3.0;   // exponent 2^(±3): 1/8 .. 8
    static constexpr double kFmDepth      = 1.0;   // cycles of phase offset at k = 1
};

/** Which modulation input a mode needs (so the voice only computes what is used). */
enum class WarpInput { None, PairedOsc, Noise, Sub };

constexpr WarpInput warpInput(WarpMode m) noexcept
{
    switch (m) {
        case WarpMode::Fm: case WarpMode::Am: case WarpMode::Rm: return WarpInput::PairedOsc;
        case WarpMode::FmNoise: return WarpInput::Noise;
        case WarpMode::FmSub:   return WarpInput::Sub;
        default:                return WarpInput::None;
    }
}

/** Frequency multiplier a mode applies to the table read (used to pick a safe mip level). */
inline double warpPitchFactor(WarpMode m, float k) noexcept
{
    if (m == WarpMode::Sync || m == WarpMode::WindowSync) return 1.0 + k * (WarpConstants::kSyncMaxRatio - 1.0);
    if (m == WarpMode::Mirror || m == WarpMode::Remap2) return 1.0 + k;   // up to double speed
    return 1.0;
}

/** Whether a mode adds content the mip level can't account for (so oversampling helps). */
constexpr bool warpNeedsOversampling(WarpMode m) noexcept
{
    return m != WarpMode::Off && m != WarpMode::Remap1;
}

struct WarpOut {
    double phase;
    float  amp;
};

namespace warp_detail {
inline double frac(double x) noexcept { return x - std::floor(x); }
inline double pinch(double p, double e) noexcept
{
    return p < 0.5 ? 0.5 * std::pow(2.0 * p, e) : 1.0 - 0.5 * std::pow(2.0 - 2.0 * p, e);
}
inline double mirror(double p) noexcept { return p < 0.5 ? 2.0 * p : 2.0 - 2.0 * p; }
inline double lerp(double a, double b, double t) noexcept { return a + (b - a) * t; }
}

/**
 * @param p    input phase in [0,1)
 * @param k    amount 0..1
 * @param mod  modulation input in -1..1 (paired osc / noise / sub sample) — ignored by other modes
 */
inline WarpOut applyWarp(WarpMode mode, float k, double p, float mod) noexcept
{
    using namespace warp_detail;
    constexpr double kPi = 3.14159265358979323846;
    const double c = WarpConstants::kBendRange;
    switch (mode) {
        case WarpMode::Off:
        case WarpMode::Count:
            return {p, 1.0f};
        case WarpMode::Sync:
            return {frac(p * (1.0 + k * (WarpConstants::kSyncMaxRatio - 1.0))), 1.0f};
        case WarpMode::WindowSync: {
            const float window = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * p));
            return {frac(p * (1.0 + k * (WarpConstants::kSyncMaxRatio - 1.0))), 1.0f + (window - 1.0f) * k};
        }
        case WarpMode::BendPlus:      return {pinch(p, std::exp2(c * k)), 1.0f};
        case WarpMode::BendMinus:     return {pinch(p, std::exp2(-c * k)), 1.0f};
        case WarpMode::BendPlusMinus: return {pinch(p, std::exp2(c * (2.0 * k - 1.0))), 1.0f};
        case WarpMode::Pwm: {
            const double q = p / (1.0 - 0.99 * k);
            return q >= 1.0 ? WarpOut{0.0, 0.0f} : WarpOut{q, 1.0f};
        }
        case WarpMode::AsymPlus:      return {std::pow(p, std::exp2(c * k)), 1.0f};
        case WarpMode::AsymMinus:     return {std::pow(p, std::exp2(-c * k)), 1.0f};
        case WarpMode::AsymPlusMinus: return {std::pow(p, std::exp2(c * (2.0 * k - 1.0))), 1.0f};
        case WarpMode::Flip:
            return {p, p < 1.0 - k ? 1.0f : -1.0f};
        case WarpMode::Mirror:
        case WarpMode::Remap2:
            return {frac(lerp(p, mirror(p), k)), 1.0f};
        case WarpMode::Remap1:
            return {p, 1.0f};
        case WarpMode::Remap3:
            return {lerp(p, 0.5 - 0.5 * std::cos(kPi * p), k), 1.0f};
        case WarpMode::Remap4: {
            const double s = std::floor(4.0 * p), x = 4.0 * p - s;
            return {lerp(p, (s + 0.5 - 0.5 * std::cos(kPi * x)) / 4.0, k), 1.0f};
        }
        case WarpMode::Quantize: {
            const double steps = 2048.0 * std::exp2(-10.0 * k);   // 2048 → 2
            return {std::floor(p * steps) / steps, 1.0f};
        }
        case WarpMode::Fm:
        case WarpMode::FmNoise:
        case WarpMode::FmSub:
            return {frac(p + k * WarpConstants::kFmDepth * mod), 1.0f};
        case WarpMode::Am:
            return {p, 1.0f - k + k * (0.5f + 0.5f * mod)};
        case WarpMode::Rm:
            return {p, 1.0f - k + k * mod};
    }
    return {p, 1.0f};
}

/** Two chained warp slots. */
inline WarpOut applyDualWarp(WarpMode m1, float k1, float mod1, WarpMode m2, float k2, float mod2, double p) noexcept
{
    const WarpOut a = applyWarp(m1, k1, p, mod1);
    if (m2 == WarpMode::Off) return a;
    const WarpOut b = applyWarp(m2, k2, a.phase, mod2);
    return {b.phase, a.amp * b.amp};
}

} // namespace winerose::dsp
