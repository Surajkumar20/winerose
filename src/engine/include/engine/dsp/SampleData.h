#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace winerose::dsp {

/** Process-unique id for an immutable asset; voices compare ids (never pointers) to notice a swapped asset. */
std::uint64_t nextAssetId() noexcept;

/**
 * @class SampleData
 * @brief An immutable audio sample shared read-only with the audio thread (SPEC §1.4 Sample / Multisample /
 *        Granular / Spectral sources). Built off the audio thread, published through the EngineSnapshot.
 *
 * Mipmapped: level L is the sample low-passed and decimated by 2^L, so reading it faster than real time never
 * aliases (SPEC: "8-16-tap windowed-sinc with mipmaps by ratio"). Every level carries kPad zero samples on each
 * side so readers never bounds-check inside the kernel.
 */
class SampleData {
public:
    static constexpr int kMaxLevels = 8;
    static constexpr int kPad       = 8;

    /**
     * @param left, right  channel data; `right` empty for mono
     * @param loopStart/loopEnd  frames from the file's smpl chunk, or -1
     */
    static std::shared_ptr<const SampleData> build(std::vector<float> left, std::vector<float> right, double sampleRate,
                                                   int rootKey = 60, std::int64_t loopStart = -1, std::int64_t loopEnd = -1,
                                                   std::string name = {});

    std::uint64_t      id() const noexcept { return m_id; }
    double             sampleRate() const noexcept { return m_sampleRate; }
    int                channels() const noexcept { return m_channels; }
    std::int64_t       frames() const noexcept { return m_frames; }
    int                rootKey() const noexcept { return m_rootKey; }
    std::int64_t       loopStart() const noexcept { return m_loopStart; }
    std::int64_t       loopEnd() const noexcept { return m_loopEnd; }
    const std::string& name() const noexcept { return m_name; }
    int                levelCount() const noexcept { return static_cast<int>(m_levels.size()); }
    std::int64_t       levelFrames(int level) const noexcept { return (m_frames + (std::int64_t{1} << level) - 1) >> level; }

    /** Pointer to sample 0 of a level/channel (kPad readable samples exist before it and after the end). */
    const float* data(int level, int channel) const noexcept
    {
        return m_levels[static_cast<std::size_t>(level)][static_cast<std::size_t>(channel < m_channels ? channel : 0)].data() + kPad;
    }

    /** Mono mix of level 0 (for granular / spectral analysis). */
    float monoAt(std::int64_t i) const noexcept
    {
        if (i < 0 || i >= m_frames) return 0.0f;
        return m_channels == 1 ? data(0, 0)[i] : 0.5f * (data(0, 0)[i] + data(0, 1)[i]);
    }

private:
    std::uint64_t m_id = 0;
    double       m_sampleRate = 48000.0;
    int          m_channels = 1;
    std::int64_t m_frames = 0;
    int          m_rootKey = 60;
    std::int64_t m_loopStart = -1, m_loopEnd = -1;
    std::string  m_name;
    std::vector<std::array<std::vector<float>, 2>> m_levels;
};

/**
 * @brief Band-limited fractional-delay reads: 16-tap Kaiser-windowed sinc, 256 phases (linearly interpolated),
 *        five cutoffs. Call sinc::init() once off the audio thread (Voice::initDownsamplerCoefs does).
 */
namespace sinc {
inline constexpr int kTaps = 16;     // taps -7 .. +8 around floor(pos)
inline constexpr int kPhases = 256;
inline constexpr int kCutoffs = 5;   // 0: full band (ratio <= 1); 1..4: quarter-octave steps for ratio in [1, 2)

void init() noexcept;

/** Mip level and cutoff table for a read increment (level-0 samples per output sample). */
struct ReadSetup {
    int    level = 0;
    int    cutoff = 0;
    double scale = 1.0;   // 1 / 2^level: converts level-0 positions to this level
};
ReadSetup setup(double increment, int levelCount) noexcept;

/** Read one channel at a level-relative position (finite; caller keeps it within [-kPad+8, frames+kPad-8]). */
float read(const float* data, double pos, int cutoff) noexcept;

/** Read two channels with one kernel. */
void read2(const float* left, const float* right, double pos, int cutoff, float& outL, float& outR) noexcept;
}

/**
 * @brief Plays one SampleData region (Sample oscillator, and one SFZ region of a Multisample oscillator).
 *        Positions are in level-0 frames; reads go through the mip level matching the increment.
 */
struct SamplePlayer {
    enum class Loop : int { Off = 0, Forward, PingPong, Sustain, Count };   // Sustain: loop until release, then play out
    static constexpr const char* kLoopNames[] = {"One-shot", "Forward", "Ping-pong", "Sustain"};

    // Region (set at note start)
    std::int64_t start = 0, end = 0;               // play range [start, end)
    std::int64_t loopStart = 0, loopEnd = 0;       // loop range (inside [start, end))
    double       xfade = 0.0;                      // forward-loop crossfade length (frames)
    Loop         loop = Loop::Off;
    float        gainL = 1.0f, gainR = 1.0f;
    double       rate = 1.0;                       // level-0 frames per output sample at pitch ratio 1

    // Playback state
    double pos = 0.0;
    int    dir = 1;
    bool   active = false;
    bool   released = false;

    void begin(double startPos) noexcept { pos = startPos; dir = 1; active = true; released = false; }

    /** Adds n samples into outL/outR. `pitch` multiplies `rate` (per-block modulation). */
    void render(const SampleData& s, float* outL, float* outR, int n, double pitch) noexcept;
};

} // namespace winerose::dsp
