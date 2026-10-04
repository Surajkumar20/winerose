#pragma once

#include "engine/Smoother.h"
#include "engine/dsp/Envelope.h"
#include "engine/dsp/Svf.h"
#include "engine/dsp/WavetableBank.h"
#include "engine/modules/Modules.h"

#include <cstdint>
#include <initializer_list>

namespace winerose::voice {

// Control-rate state shared by every voice, recomputed by the Engine at each control tick
// (every kControlBlock samples, aligned to the absolute sample count so output doesn't depend on the
// host's block size).
struct VoiceControl {
    modules::OscillatorModule::Values osc {};
    modules::FilterModule::Values     filter {};
    dsp::Envelope::Settings           env {};
    dsp::Svf::Coefs                   filterCoefs {};
    double                            sampleRate = 48000.0;
};

inline constexpr int kControlBlock = 32;

/**
 * @class Voice
 * @brief One note: wavetable oscillator → (optional) SVF low-pass → amp envelope (Env0) → constant-power pan.
 *        Phase 1 of SPEC §5.4. Allocation-free; all state is preallocated in the VoiceManager's pool.
 */
class Voice {
public:
    void prepare(double sampleRate) noexcept;

    void start(int note, std::uint64_t order, double startPhase, const VoiceControl& control) noexcept;
    void release() noexcept { m_env.noteOff(); m_released = true; }
    void kill() noexcept { m_env.reset(); m_released = true; }

    /** Retrigger an in-use voice for a new note (voice stealing): the envelope attacks from its current
     *  level and the phase continues, so the steal doesn't click. */
    void steal(int note, std::uint64_t order, const VoiceControl& control) noexcept;

    /** Control tick: new targets; per-sample values ramp to them over kControlBlock samples. */
    void control(const VoiceControl& control) noexcept;

    /** Adds into left/right. bank may be null (no table published yet → silence). */
    void render(float* left, float* right, int numSamples, const dsp::WavetableBank* bank) noexcept;

    bool          active() const noexcept { return m_env.isActive(); }
    bool          released() const noexcept { return m_released; }
    int           note() const noexcept { return m_note; }
    std::uint64_t order() const noexcept { return m_order; }
    bool          sustained() const noexcept { return m_sustained; }
    void          setSustained(bool s) noexcept { m_sustained = s; }

private:
    void updatePitch(const VoiceControl& control) noexcept;

    dsp::Envelope m_env;
    dsp::Svf      m_svf;
    dsp::Svf::Coefs m_coefs {};
    bool          m_filterOn = false;
    bool          m_oscOn    = true;

    double m_phase = 0.0;
    double m_increment = 0.0;
    dsp::WavetableBank::LevelChoice m_levels { 0, 0.0f };

    LinearSmoother m_wtPos, m_gain, m_panL, m_panR;

    int           m_note = -1;
    std::uint64_t m_order = 0;
    bool          m_released = true;
    bool          m_sustained = false;
};

} // namespace winerose::voice
