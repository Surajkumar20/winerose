#pragma once

#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace winerose::dsp {

/**
 * @class WavetableBank
 * @brief Immutable, band-limited (mipmapped) wavetable: N frames × 2048 samples × kLevels levels.
 *
 * SPEC §5.5. Built once on a non-realtime thread, then shared read-only with the audio thread through the
 * EngineSnapshot. Level L keeps harmonics 1 .. (1024 >> L) of every frame (DC removed), so level 0 is
 * full-band and level 10 is the pure fundamental.
 *
 * Level selection (selectLevel) deviates from the SPEC's "floor(log2(inc·2048))" by one level: with that
 * formula the top harmonics of the chosen level can sit up to an octave above Nyquist. Using
 * floor(log2(inc·2048)) + 1 (crossfading toward the next level by the fractional part) guarantees every
 * partial stays below Nyquist — the Phase 1 acceptance criterion is aliasing < -90 dBFS — while staying
 * continuous as pitch sweeps.
 */
class WavetableBank {
public:
    static constexpr int kFrameSize = 2048;
    static constexpr int kLevels    = 11;
    static constexpr int kMaxFrames = 256;
    static constexpr int kGuardPre  = 1;   // samples before index 0 (for 4-point interpolation)
    static constexpr int kGuardPost = 2;   // samples after index 2047
    static constexpr int kStride    = kFrameSize + kGuardPre + kGuardPost;

    /**
     * @param frames     concatenated single-cycle frames, frameCount × frameSize samples
     * @param frameSize  64..8192; resampled to 2048 if different
     * @param name       display name (e.g. file stem)
     * Frames beyond kMaxFrames are dropped. An empty input yields a single silent frame.
     */
    static std::shared_ptr<const WavetableBank> build(const std::vector<float>& frames, int frameSize,
                                                      std::string name);

    int                frameCount() const noexcept { return m_frameCount; }
    const std::string& name() const noexcept { return m_name; }

    /** Highest harmonic present in a level. */
    static constexpr int maxHarmonic(int level) noexcept { return (kFrameSize / 2) >> level; }

    struct LevelChoice {
        int   level;     // primary level (has more harmonics)
        float blend;     // 0..1 weight of level + 1
    };

    /** Choose mip levels for a phase increment in cycles/sample (frequency / sampleRate). */
    static LevelChoice selectLevel(double increment) noexcept
    {
        const double pos = std::log2(std::max(increment * kFrameSize, 0.5));   // >= -1
        const double fl  = std::floor(pos);
        int level = static_cast<int>(fl) + 1;
        float blend = static_cast<float>(pos - fl);
        if (level >= kLevels - 1) { level = kLevels - 1; blend = 0.0f; }
        return {level, blend};
    }

    /** Pointer to sample 0 of (level, frame); valid indices -kGuardPre .. kFrameSize-1+kGuardPost. */
    const float* frameData(int level, int frame) const noexcept
    {
        return m_data.data() + (static_cast<std::size_t>(level) * static_cast<std::size_t>(m_frameCount)
                                + static_cast<std::size_t>(frame)) * kStride + kGuardPre;
    }

    /**
     * @brief Realtime read: phase in [0,1), framePos in [0, frameCount-1] (fractional = smooth morph between
     *        adjacent frames), using a precomputed LevelChoice. 4-point Hermite within a frame.
     */
    float read(double phase, float framePos, LevelChoice lc) const noexcept
    {
        const float pos  = static_cast<float>(phase * kFrameSize);
        const int   idx  = static_cast<int>(pos);
        const float t    = pos - static_cast<float>(idx);
        const int   f0   = std::min(static_cast<int>(framePos), m_frameCount - 1);
        const int   f1   = std::min(f0 + 1, m_frameCount - 1);
        const float ft   = framePos - static_cast<float>(f0);

        float a = sampleLevel(lc.level, f0, f1, ft, idx, t);
        if (lc.blend > 0.0f) {
            const float b = sampleLevel(lc.level + 1, f0, f1, ft, idx, t);
            a += (b - a) * lc.blend;
        }
        return a;
    }

private:
    WavetableBank() = default;

    static float hermite(const float* x, int i, float t) noexcept
    {
        const float xm1 = x[i - 1], x0 = x[i], x1 = x[i + 1], x2 = x[i + 2];
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    float sampleLevel(int level, int f0, int f1, float ft, int idx, float t) const noexcept
    {
        const float a = hermite(frameData(level, f0), idx, t);
        if (f1 == f0 || ft <= 0.0f) return a;
        const float b = hermite(frameData(level, f1), idx, t);
        return a + (b - a) * ft;
    }

    int                m_frameCount = 0;
    std::string        m_name;
    std::vector<float> m_data;   // [level][frame][kStride]
};

/** The built-in "Basic Shapes" table: saw, square, triangle, sine (wtPos 0 = saw). Original content. */
std::shared_ptr<const WavetableBank> makeBasicShapesTable();

} // namespace winerose::dsp
