#pragma once

#include "engine/Smoother.h"
#include "engine/dsp/Envelope.h"
#include "engine/dsp/NoiseTable.h"
#include "engine/dsp/Svf.h"
#include "engine/dsp/Unison.h"
#include "engine/dsp/Warp.h"
#include "engine/dsp/WavetableBank.h"
#include "engine/modules/Modules.h"

#include "hiir/Downsampler2xFpu.h"

#include <array>
#include <cstdint>

namespace winerose::voice {

inline constexpr int kControlBlock  = 32;
inline constexpr int kMaxOversample = 4;
inline constexpr int kOscCount      = modules::OscillatorModule::kCount;
inline constexpr int kDownsamplerCoefs = 12;

// Control-rate state shared by every voice, recomputed by the Engine at each control tick (every
// kControlBlock samples, aligned to the absolute sample count so output doesn't depend on host block size).
struct VoiceControl {
    std::array<modules::OscillatorModule::Values, kOscCount> osc {};
    modules::NoiseModule::Values  noise {};
    modules::SubOscModule::Values sub {};
    modules::FilterModule::Values filter {};
    dsp::Envelope::Settings       env {};
    dsp::Svf::Coefs               filterCoefs {};
    double                        sampleRate = 48000.0;
    int                           oversample = 1;   // 1, 2 or 4 (Quality, only when a warp needs it)
};

// Tables the voice reads this block, taken from the CURRENT EngineSnapshot (never cached across blocks).
struct VoiceTables {
    std::array<const dsp::WavetableBank*, kOscCount> osc {};
    const dsp::WavetableBank* sub = nullptr;
    const dsp::NoiseTables*   noise = nullptr;
};

/** Deterministic xorshift64* generator in [0,1). */
struct Rng {
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    double next() noexcept
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return static_cast<double>((state * 0x2545F4914F6CDD1Dull) >> 11) * (1.0 / 9007199254740992.0);
    }
};

/**
 * @class Voice
 * @brief One note (SPEC §5.4 Phases 1-2): oscillators A/B/C (unison up to 16, dual warp) + noise + sub →
 *        optional oversampling (HIIR half-band downsamplers) → stereo SVF low-pass → amp envelope (Env0).
 *        Allocation-free; all state lives in the VoiceManager's preallocated pool.
 *
 * Signal routing is "everything through Filter0 when enabled" until the routing matrix lands (Phase 4).
 */
class Voice {
public:
    static void initDownsamplerCoefs() noexcept;   // call once, off the audio thread

    void prepare(double sampleRate) noexcept;

    void start(int note, std::uint64_t order, const VoiceControl& control, Rng& rng) noexcept;
    void release() noexcept { m_env.noteOff(); m_released = true; }
    void kill() noexcept { m_env.reset(); m_released = true; }

    /** Retrigger an in-use voice for a new note (stealing): envelope attacks from its current level and
     *  oscillator phases continue, so the steal doesn't click. */
    void steal(int note, std::uint64_t order, const VoiceControl& control) noexcept;

    /** Control tick: recompute unison layout, increments and targets. */
    void control(const VoiceControl& control) noexcept;

    /** Adds into left/right (numSamples <= kControlBlock). */
    void render(float* left, float* right, int numSamples, const VoiceTables& tables) noexcept;

    bool          active() const noexcept { return m_env.isActive(); }
    bool          released() const noexcept { return m_released; }
    int           note() const noexcept { return m_note; }
    std::uint64_t order() const noexcept { return m_order; }
    bool          sustained() const noexcept { return m_sustained; }
    void          setSustained(bool s) noexcept { m_sustained = s; }

private:
    struct Unison {
        double phase = 0.0;
        double inc   = 0.0;            // cycles per OVERSAMPLED sample
        dsp::WavetableBank::LevelChoice levels { 0, 0.0f };
        float  gainL = 0.0f, gainR = 0.0f;
        float  spread = 0.0f;          // position t in [-1,1]
        float  warp1 = 0.0f, warp2 = 0.0f;
        float  randomDetune = 0.0f;    // per-note value for unison Mode::Random
    };

    struct Osc {
        std::array<Unison, dsp::unison::kMaxVoices> voices {};
        int  count = 1;
        bool on = false;
        bool smooth = true;
        float span = 0.0f;
        dsp::WarpMode warp1 = dsp::WarpMode::Off, warp2 = dsp::WarpMode::Off;
        LinearSmoother wtPos, level;
        float last = 0.0f;             // previous output (mono, unit level) for FM/AM/RM of the paired osc
    };

    void layout(const VoiceControl& control) noexcept;
    void resetDownsamplers() noexcept;

    std::array<Osc, kOscCount> m_osc {};

    // Sub
    bool   m_subOn = false;
    int    m_subShape = 0;
    double m_subPhase = 0.0, m_subInc = 0.0;
    dsp::WavetableBank::LevelChoice m_subLevels { 0, 0.0f };
    float  m_subGainL = 1.0f, m_subGainR = 1.0f;
    LinearSmoother m_subLevel;

    // Noise
    bool   m_noiseOn = false;
    dsp::NoiseTables::Type m_noiseType = dsp::NoiseTables::Type::White;
    double m_noisePos = 0.0, m_noiseRate = 1.0;
    float  m_noiseGainL = 1.0f, m_noiseGainR = 1.0f;
    LinearSmoother m_noiseLevel;

    // Post-oscillator
    int m_oversample = 1;
    std::array<hiir::Downsampler2xFpu<kDownsamplerCoefs>, 4> m_down;   // [L 4→2, R 4→2, L 2→1, R 2→1]
    dsp::Svf m_svfL, m_svfR;
    dsp::Svf::Coefs m_coefs {};
    bool m_filterOn = false;
    dsp::Envelope m_env;

    double        m_sampleRate = 48000.0;
    int           m_note = -1;
    std::uint64_t m_order = 0;
    bool          m_released = true;
    bool          m_sustained = false;
};

} // namespace winerose::voice
