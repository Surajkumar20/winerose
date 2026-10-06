#pragma once

#include "engine/dsp/Curve.h"
#include "engine/dsp/Envelope.h"
#include "engine/dsp/Fft.h"
#include "engine/dsp/Multisample.h"
#include "engine/dsp/SampleData.h"
#include "engine/dsp/Spectral.h"
#include "engine/dsp/NoiseTable.h"
#include "engine/dsp/Svf.h"
#include "engine/dsp/WavetableBank.h"
#include "engine/modulation/Lfo.h"
#include "engine/modulation/Sources.h"
#include "engine/modules/Modules.h"

#include <array>
#include <cstdint>

namespace winerose::voice {

inline constexpr int kControlBlock  = 32;
inline constexpr int kMaxOversample = 4;
inline constexpr int kOscCount      = modules::OscillatorModule::kCount;

// Per-voice control state for one control tick: module values after this voice's modulation.
struct VoiceControl {
    std::array<modules::OscillatorModule::Values, kOscCount> osc {};
    modules::NoiseModule::Values  noise {};
    modules::SubOscModule::Values sub {};
    std::array<modules::FilterModule::Values, modules::FilterModule::kCount> filter {};
    modules::FilterRouting        filterRouting = modules::FilterRouting::Serial;
    dsp::Envelope::Settings       env {};          // Env0 (amplitude)
    double                        sampleRate = 48000.0;
    int                           oversample = 1;  // 1, 2 or 4 (Quality, only when a warp needs it)
    float                         bendSemis = 0.0f;
};

// Everything a voice derives from the (modulated) value array each tick.
struct VoiceSettings {
    VoiceControl control;
    std::array<dsp::Envelope::Settings, modulation::kEnvCount> env {};      // env[0] mirrors control.env
    std::array<modulation::LfoSettings, modulation::kLfoCount> lfo {};
    std::array<float, modulation::kMacroCount> macro {};
};

// Tables the voice reads this block, taken from the CURRENT EngineSnapshot (never cached across blocks).
struct VoiceTables {
    std::array<const dsp::WavetableBank*, kOscCount> osc {};
    std::array<const dsp::CurveTable*, kOscCount>    remap {};
    const dsp::WavetableBank* sub = nullptr;
    const dsp::NoiseTables*   noise = nullptr;
    const dsp::WavetableBank* lfoShapes = nullptr;
    std::array<const dsp::WavetableBank*, modulation::kLfoCount> lfoPaths {};
    // Phase 7 sources (oscillators A/B/C): the loaded sample (Sample / Granular), its spectral analysis
    // (Spectral), and the SFZ instrument (Multisample).
    std::array<const dsp::SampleData*, kOscCount>   sample {};
    std::array<const dsp::SpectralData*, kOscCount> spectral {};
    std::array<const dsp::Multisample*, kOscCount>  multi {};
    dsp::RealFft*  fft = nullptr;          // shared spectral-resynthesis FFT (voices render one at a time)
    std::uint32_t* roundRobin = nullptr;   // per-oscillator SFZ round-robin counters (kOscCount entries)

    modulation::LfoTables lfo(int index) const noexcept
    {
        return {lfoShapes, lfoPaths[static_cast<std::size_t>(index)]};
    }
};

} // namespace winerose::voice
