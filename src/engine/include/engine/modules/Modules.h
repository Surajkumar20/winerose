#pragma once

#include "engine/dsp/Envelope.h"

#include "params/ConfigManager.h"
#include "params/ParamHandle.h"
#include "params/ParamRegistry.h"

#include <memory>
#include <string>

namespace winerose::modules {

// Each module: owns its ParamRegistry (named like the Serum 2 CBOR module it mirrors), registers its
// parameters in the constructor, and offers read() — a lock-free snapshot of its plain values for the
// audio thread, built from ParamHandles. Keys are listed next to each module; UIs address them as
// "<Module><index>.<key>" (e.g. "Oscillator0.wtPos"). Defaults marked TODO-MEASURE await the
// measure_host sweep of Serum 2 (SPEC §5.10).

// --- Oscillator (Oscillator0..2 = A/B/C) -------------------------------------------------------------
namespace osc_keys {
inline constexpr const char* enabled = "enabled";
inline constexpr const char* level   = "level";
inline constexpr const char* pan     = "pan";
inline constexpr const char* octave  = "octave";
inline constexpr const char* semi    = "semi";
inline constexpr const char* fine    = "fine";
inline constexpr const char* wtPos   = "wtPos";
inline constexpr const char* phase   = "phase";
inline constexpr const char* random  = "random";
}

class OscillatorModule {
public:
    struct Values {
        bool  enabled;
        float level;        // linear 0..1 (TODO-MEASURE dB law)
        float pan;          // -1..1
        float pitchSemis;   // octave·12 + semi + fine/100
        float wtPos;        // 0..1 across the table's frames
        float phase;        // 0..1 start phase
        float random;       // 0..1 random start-phase spread
    };

    OscillatorModule(std::shared_ptr<ConfigManager> config, int index);
    Values read() const noexcept;
    ParamRegistry& registry() noexcept { return *m_registry; }

private:
    std::unique_ptr<ParamRegistry> m_registry;
    ParamHandle m_enabled, m_level, m_pan, m_octave, m_semi, m_fine, m_wtPos, m_phase, m_random;
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
