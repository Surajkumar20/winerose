#pragma once

#include "engine/dsp/Envelope.h"
#include "engine/dsp/NoiseTable.h"
#include "engine/dsp/Unison.h"
#include "engine/dsp/Warp.h"
#include "engine/dsp/WavetableBank.h"

#include "params/ConfigManager.h"
#include "params/ParamHandle.h"
#include "params/ParamRegistry.h"

#include <memory>
#include <string>

namespace winerose::modules {

// Each module: owns its ParamRegistry (named like the Serum 2 CBOR module it mirrors), registers its
// parameters in the constructor, and offers read() — a lock-free snapshot of its plain values for the
// audio thread, built from ParamHandles. Keys are listed next to each module; UIs address them as
// "<Module><index>.<key>" (e.g. "Oscillator0.wtPos"). Defaults and enum orders marked TODO-MEASURE await
// the measure_host sweep of Serum 2 (SPEC §5.10).

// --- Wavetable oscillator (Oscillator0..2 = A/B/C) ---------------------------------------------------
namespace osc_keys {
inline constexpr const char* enabled      = "enabled";
inline constexpr const char* level        = "level";
inline constexpr const char* pan          = "pan";
inline constexpr const char* octave       = "octave";
inline constexpr const char* semi         = "semi";
inline constexpr const char* fine         = "fine";
inline constexpr const char* wtPos        = "wtPos";
inline constexpr const char* wtSmooth     = "wtSmooth";
inline constexpr const char* phase        = "phase";
inline constexpr const char* random       = "random";
inline constexpr const char* unison       = "unison";
inline constexpr const char* uniDetune    = "uniDetune";
inline constexpr const char* uniBlend     = "uniBlend";
inline constexpr const char* uniWidth     = "uniWidth";
inline constexpr const char* uniRange     = "uniRange";
inline constexpr const char* uniStack     = "uniStack";
inline constexpr const char* uniMode      = "uniMode";
inline constexpr const char* uniSpan      = "uniSpan";
inline constexpr const char* uniRandStart = "uniRandStart";
inline constexpr const char* uniWarp      = "uniWarp";
inline constexpr const char* warp1Mode    = "warp1Mode";
inline constexpr const char* warp1Amount  = "warp1Amount";
inline constexpr const char* warp2Mode    = "warp2Mode";
inline constexpr const char* warp2Amount  = "warp2Amount";
}

class OscillatorModule {
public:
    static constexpr int kCount = 3;

    struct Values {
        bool  enabled;
        float level;          // linear 0..1 (TODO-MEASURE dB law)
        float pan;            // -1..1
        float pitchSemis;     // octave·12 + semi + fine/100
        float wtPos;          // 0..1 across the table's frames
        bool  wtSmooth;       // true: continuous morph between frames; false: snap to the nearest frame
        float phase;          // 0..1 start phase; >= kPhaseMem means "Mem" (continue from the last note)
        float random;         // 0..1 random start-phase spread
        int   unison;         // 1..16
        float uniDetune;      // 0..1 of uniRange
        float uniBlend;       // 0..1 (0.75 = even)
        float uniWidth;       // 0..1 stereo spread
        float uniRange;       // semitones, 0..48
        dsp::unison::Stack stack;
        dsp::unison::Mode  mode;
        float uniSpan;        // 0..1 wavetable-position spread across unison voices (INFERRED meaning)
        float uniRandStart;   // 0..1: 0 = unison voices share one random phase, 1 = independent (INFERRED)
        float uniWarp;        // 0..1 warp-amount spread across unison voices
        dsp::WarpMode warp1;
        float         warp1Amount;
        dsp::WarpMode warp2;
        float         warp2Amount;
    };
    static constexpr float kPhaseMem = 0.999f;

    OscillatorModule(std::shared_ptr<ConfigManager> config, int index);
    Values read() const noexcept;
    ParamRegistry& registry() noexcept { return *m_registry; }

private:
    std::unique_ptr<ParamRegistry> m_registry;
    ParamHandle m_enabled, m_level, m_pan, m_octave, m_semi, m_fine, m_wtPos, m_wtSmooth, m_phase, m_random,
                m_unison, m_uniDetune, m_uniBlend, m_uniWidth, m_uniRange, m_uniStack, m_uniMode, m_uniSpan,
                m_uniRandStart, m_uniWarp, m_warp1Mode, m_warp1Amount, m_warp2Mode, m_warp2Amount;
};

// --- Noise oscillator (Oscillator3, Serum's "NoiseOsc3") ---------------------------------------------
namespace noise_keys {
inline constexpr const char* enabled  = "enabled";
inline constexpr const char* type     = "type";
inline constexpr const char* level    = "level";
inline constexpr const char* pan      = "pan";
inline constexpr const char* keytrack = "keytrack";
inline constexpr const char* pitch    = "pitch";
}

class NoiseModule {
public:
    static constexpr int kIndex = 3;

    struct Values {
        bool  enabled;
        dsp::NoiseTables::Type type;
        float level;
        float pan;
        bool  keytrack;     // playback rate follows the note (relative to C4)
        float pitchSemis;   // playback-rate offset
    };

    explicit NoiseModule(std::shared_ptr<ConfigManager> config);
    Values read() const noexcept;
    ParamRegistry& registry() noexcept { return *m_registry; }

private:
    std::unique_ptr<ParamRegistry> m_registry;
    ParamHandle m_enabled, m_type, m_level, m_pan, m_keytrack, m_pitch;
};

// --- Sub oscillator (Oscillator4, Serum's "SubOsc4") -------------------------------------------------
namespace sub_keys {
inline constexpr const char* enabled = "enabled";
inline constexpr const char* shape   = "shape";
inline constexpr const char* octave  = "octave";
inline constexpr const char* level   = "level";
inline constexpr const char* pan     = "pan";
}

class SubOscModule {
public:
    static constexpr int kIndex = 4;

    struct Values {
        bool  enabled;
        dsp::SubShape shape;
        float pitchSemis;
        float level;
        float pan;
    };

    explicit SubOscModule(std::shared_ptr<ConfigManager> config);
    Values read() const noexcept;
    ParamRegistry& registry() noexcept { return *m_registry; }

private:
    std::unique_ptr<ParamRegistry> m_registry;
    ParamHandle m_enabled, m_shape, m_octave, m_level, m_pan;
};

// --- Filter (Filter0..1) -----------------------------------------------------------------------------
namespace filter_keys {
inline constexpr const char* enabled   = "enabled";
inline constexpr const char* cutoff    = "cutoff";
inline constexpr const char* resonance = "resonance";
}

class FilterModule {
public:
    struct Values {
        bool  enabled;
        float cutoffHz;
        float resonance;   // 0..1
    };

    FilterModule(std::shared_ptr<ConfigManager> config, int index);
    Values read() const noexcept;
    ParamRegistry& registry() noexcept { return *m_registry; }

private:
    std::unique_ptr<ParamRegistry> m_registry;
    ParamHandle m_enabled, m_cutoff, m_resonance;
};

// --- Envelope (Env0..3; Env0 drives amplitude) -------------------------------------------------------
namespace env_keys {
inline constexpr const char* attack  = "attack";
inline constexpr const char* hold    = "hold";
inline constexpr const char* decay   = "decay";
inline constexpr const char* sustain = "sustain";
inline constexpr const char* release = "release";
}

class EnvelopeModule {
public:
    EnvelopeModule(std::shared_ptr<ConfigManager> config, int index);
    dsp::Envelope::Settings read() const noexcept;
    ParamRegistry& registry() noexcept { return *m_registry; }

private:
    std::unique_ptr<ParamRegistry> m_registry;
    ParamHandle m_attack, m_hold, m_decay, m_sustain, m_release;
};

} // namespace winerose::modules
