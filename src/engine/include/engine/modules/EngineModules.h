#pragma once

#include "engine/modules/Modules.h"
#include "engine/voice/VoiceSettings.h"

#include <array>
#include <memory>
#include <vector>

namespace winerose::modules {

/** Destinations that an LFO can modulate per sample (the audio-rate path, SPEC §5.3). */
struct FastDest {
    enum class Kind : std::uint8_t { None, OscPitch, OscLevel, OscWtPos, FilterCutoff } kind = Kind::None;
    std::uint8_t osc = 0;     // oscillator index for Osc* kinds
    float span = 0.0f;        // plain units per normalized unit (linear destinations)
};

/**
 * @brief All engine modules, their ModTargets table, and the builder that turns a value array into a
 *        voice's settings. Owned by the Engine; read-only for voices.
 */
struct EngineModules {
    explicit EngineModules(std::shared_ptr<ConfigManager> config);

    /** Build every per-voice setting from plain values (realtime-safe). */
    void build(const float* plain, double sampleRate, voice::VoiceSettings& out) const noexcept;

    ModTargets targets;
    std::array<std::unique_ptr<OscillatorModule>, voice::kOscCount> osc;
    std::unique_ptr<NoiseModule>  noise;
    std::unique_ptr<SubOscModule> sub;
    std::unique_ptr<FilterModule> filter0;
    std::array<std::unique_ptr<EnvelopeModule>, modulation::kEnvCount> env;
    std::array<std::unique_ptr<LfoModule>, modulation::kLfoCount>      lfo;
    std::array<std::unique_ptr<MacroModule>, modulation::kMacroCount>  macro;
    std::unique_ptr<MatrixModule> matrix;

    std::vector<FastDest> fastDest;   // per target index
    std::array<int, modulation::kMacroCount> macroTarget {};   // target index of each macro's value
};

} // namespace winerose::modules
