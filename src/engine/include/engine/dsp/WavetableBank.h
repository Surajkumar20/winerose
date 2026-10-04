#pragma once

#include <algorithm>
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
 * full-band and level 10 is the pure fundamental (plus DC when built with removeDc = false).
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
     * @param removeDc   true for oscillators; false for LFO shapes, whose offset is part of the signal
     * Frames beyond kMaxFrames are dropped. An empty input yields a single silent frame.
     */
    static std::shared_ptr<const WavetableBank> build(const std::vector<float>& frames, int frameSize,
                                                      std::string name, bool removeDc = true);

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

    /** Resolved frame position: the two adjacent frames and the morph fraction between them. */
    struct FramePos {
        int   f0 = 0, f1 = 0;
        float ft = 0.0f;
    };

    /** framePos in [0, frameCount-1]; fractional = smooth morph between adjacent frames. */
    FramePos resolveFrame(float framePos) const noexcept
    {
        FramePos fp;
        fp.f0 = std::clamp(static_cast<int>(framePos), 0, m_frameCount - 1);
        fp.f1 = std::min(fp.f0 + 1, m_frameCount - 1);
        fp.ft = fp.f1 == fp.f0 ? 0.0f : framePos - static_cast<float>(fp.f0);
        return fp;
    }

    /**
     * @brief Realtime read: phase in [0,1) at a resolved frame position, using a precomputed LevelChoice.
     *
     * Hermite interpolation is linear in its four input samples, so the frame morph and the mip-level
     * crossfade are applied to the samples first and a single Hermite evaluated — identical to
     * interpolating each table and mixing, at a quarter of the cost.
     */
    float read(double phase, const FramePos& fp, LevelChoice lc) const noexcept
    {
        const float pos = static_cast<float>(phase * kFrameSize);
        int         idx = static_cast<int>(pos);
        const float t   = pos - static_cast<float>(idx);
        if (idx >= kFrameSize) idx -= kFrameSize;   // phase == 1.0 (warps can land exactly on it) is phase 0
        if (idx < 0) idx = 0;

        // Scalar locals on purpose: small arrays here made MSVC bounce values through memory
        // (store-forwarding stalls), tripling the cost of the level crossfade.
        const float* a = frameData(lc.level, fp.f0) + idx;
        float s0 = a[-1], s1 = a[0], s2 = a[1], s3 = a[2];
        if (fp.ft > 0.0f) {
            const float* b = frameData(lc.level, fp.f1) + idx;
            s0 += (b[-1] - s0) * fp.ft;
            s1 += (b[0]  - s1) * fp.ft;
            s2 += (b[1]  - s2) * fp.ft;
            s3 += (b[2]  - s3) * fp.ft;
        }
        if (lc.blend > 0.0f) {
            const float* c = frameData(lc.level + 1, fp.f0) + idx;
            float u0 = c[-1], u1 = c[0], u2 = c[1], u3 = c[2];
            if (fp.ft > 0.0f) {
                const float* d = frameData(lc.level + 1, fp.f1) + idx;
                u0 += (d[-1] - u0) * fp.ft;
                u1 += (d[0]  - u1) * fp.ft;
                u2 += (d[1]  - u2) * fp.ft;
                u3 += (d[2]  - u3) * fp.ft;
            }
            s0 += (u0 - s0) * lc.blend;
            s1 += (u1 - s1) * lc.blend;
            s2 += (u2 - s2) * lc.blend;
            s3 += (u3 - s3) * lc.blend;
        }
        const float c1 = 0.5f * (s2 - s0);
        const float c2 = s0 - 2.5f * s1 + 2.0f * s2 - 0.5f * s3;
        const float c3 = 0.5f * (s3 - s0) + 1.5f * (s1 - s2);
        return ((c3 * t + c2) * t + c1) * t + s1;
    }

    /** Convenience overload: resolves the frame position per call. */
    float read(double phase, float framePos, LevelChoice lc) const noexcept
    {
        return read(phase, resolveFrame(framePos), lc);
    }

    /**
     * @brief Four reads at once — unison voices sharing a table and frame position, each with its own phase
     *        and level choice. out[i] equals read(phase[i], fp, lc[i]) bit for bit (same operation order),
     *        SSE2-vectorized on x86 with a scalar fallback elsewhere.
     */
    void read4(const double* phase, const FramePos& fp, const LevelChoice* lc, float* out) const noexcept;

private:
    WavetableBank() = default;


    int                m_frameCount = 0;
    std::string        m_name;
    std::vector<float> m_data;   // [level][frame][kStride]
};

/** The built-in "Basic Shapes" table: saw, square, triangle, sine (wtPos 0 = saw). Original content. */
std::shared_ptr<const WavetableBank> makeBasicShapesTable();

/** Sub oscillator shapes, one frame each, in SubShape order. Original content. */
enum class SubShape : int { Sine = 0, RoundedRect, Triangle, Saw, Square, Pulse, Count };
inline constexpr const char* kSubShapeNames[] = {"Sine", "Rounded Rect", "Triangle", "Saw", "Square", "Pulse"};
std::shared_ptr<const WavetableBank> makeSubShapesTable();

} // namespace winerose::dsp
