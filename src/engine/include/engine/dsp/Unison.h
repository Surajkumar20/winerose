#pragma once

#include <algorithm>
#include <cmath>

namespace winerose::dsp {

/**
 * @brief Unison voice layout (SPEC §1.2, §5.5): detune offsets, stacking, blend weights and spread.
 *        Pure functions, evaluated per control tick for each unison voice u of U.
 */
namespace unison {

inline constexpr int kMaxVoices = 16;

// Tuning-mode curve applied to the normalized spread position t in [-1,1] (exponents INFERRED).
enum class Mode : int { Linear = 0, Super, Exp, Inv, Random, Count };
inline constexpr const char* kModeNames[] = {"Linear", "Super", "Exp", "Inv", "Random"};

// Stack: octave / fifth offsets layered onto the unison voices. Order TODO-MEASURE.
enum class Stack : int { Off = 0, Octave1x, Octave2x, Fifth, OctaveFifth, Center12, Center24, Count };
inline constexpr const char* kStackNames[] = {"Off", "12 (1x)", "12 (2x)", "+7", "+7 +12", "Center -12", "Center -24"};

/** Spread position of voice u in [-1,1]: evenly spaced, 0 for a single voice. */
inline float position(int u, int count) noexcept
{
    return count <= 1 ? 0.0f : 2.0f * static_cast<float>(u) / static_cast<float>(count - 1) - 1.0f;
}

/** Detune curve. For Mode::Random the caller passes a per-note random value in [-1,1] as t. */
inline float curve(Mode mode, float t) noexcept
{
    const float s = t < 0.0f ? -1.0f : 1.0f, a = std::abs(t);
    switch (mode) {
        case Mode::Super: return s * std::pow(a, 1.3f);
        case Mode::Exp:   return s * a * a;
        case Mode::Inv:   return s * std::sqrt(a);
        case Mode::Linear:
        case Mode::Random:
        case Mode::Count: break;
    }
    return t;
}

/** Index of the "centre" voice: the middle one (for even counts, the upper middle). */
inline int centerIndex(int count) noexcept { return count / 2; }

inline bool isCenter(int u, int count) noexcept
{
    if (count <= 1) return true;
    return count % 2 == 1 ? u == count / 2 : (u == count / 2 || u == count / 2 - 1);
}

/** Semitone offset added by the stack mode. */
inline float stackSemis(Stack stack, int u, int count) noexcept
{
    switch (stack) {
        case Stack::Octave1x:    return (u % 2) ? 12.0f : 0.0f;
        case Stack::Octave2x:    return static_cast<float>((u % 3) * 12);
        case Stack::Fifth:       return (u % 2) ? 7.0f : 0.0f;
        case Stack::OctaveFifth: { const int m = u % 3; return m == 0 ? 0.0f : (m == 1 ? 7.0f : 12.0f); }
        case Stack::Center12:    return u == centerIndex(count) ? -12.0f : 0.0f;
        case Stack::Center24:    return u == centerIndex(count) ? -24.0f : 0.0f;
        case Stack::Off:
        case Stack::Count: break;
    }
    return 0.0f;
}

/**
 * Blend weight: blend 0 = centre voice(s) only, 0.75 = all voices equal (Serum's default), 1 = side
 * voices only. Callers normalize the weights by 1/sqrt(sum of squares) for constant power.
 */
inline float blendWeight(float blend, int u, int count) noexcept
{
    if (count <= 2) return 1.0f;   // one or two voices have no centre/side distinction
    const float side   = std::min(1.0f, blend / 0.75f);
    const float centre = std::min(1.0f, (1.0f - blend) / 0.25f);
    return isCenter(u, count) ? centre : side;
}

} // namespace unison
} // namespace winerose::dsp
