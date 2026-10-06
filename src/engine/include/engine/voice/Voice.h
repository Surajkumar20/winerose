#pragma once

#include "engine/Smoother.h"
#include "engine/dsp/Envelope.h"
#include "engine/dsp/Svf.h"
#include "engine/dsp/Unison.h"
#include "engine/dsp/Warp.h"
#include "engine/dsp/WavetableBank.h"
#include "engine/dsp/filters/FilterUnit.h"
#include "engine/modulation/Lfo.h"
#include "engine/modules/EngineModules.h"
#include "engine/voice/VoiceSettings.h"

#include "hiir/Downsampler2xFpu.h"

#include <array>
#include <cstdint>
#include <memory>

namespace winerose::voice {

inline constexpr int kDownsamplerCoefs = 12;

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
    std::uint64_t nextSeed() noexcept { next(); return state; }
};

/** Global (channel-wide) modulation sources and performance state. */
struct GlobalModState {
    float  modWheel   = 0.0f;
    float  pitchBend  = 0.5f;   // 0..1, rest at 0.5
    float  aftertouch = 0.0f;
    float  bendSemis  = 0.0f;   // pitch-bend offset applied to every oscillator
    double bpm        = 120.0;
};

/** The six stereo buses a voice renders into (Main → FX Main; Bus 1/2 → FX buses; Direct → output). */
struct VoiceOutputs {
    float* mainL; float* mainR;
    float* directL; float* directR;
    float* bus1L; float* bus1R;
    float* bus2L; float* bus2R;
};

/** What the Engine hands every voice at each control tick. */
struct ControlContext {
    const float* base = nullptr;                          // every ModTargets value, unmodulated
    const modules::EngineModules* modules = nullptr;
    const std::array<modules::SlotParams, modulation::kSlotCount>* slots = nullptr;
    const std::array<std::int16_t, modulation::kSlotCount>* slotDest = nullptr;   // target index or -1
    GlobalModState global;
    std::array<double, modulation::kLfoCount> lfoFreePhase {};
    double sampleRate = 48000.0;
    int    oversample = 1;
    VoiceTables tables;
};

/**
 * @class Voice
 * @brief One note (SPEC §5.4 Phases 1-4): oscillators A/B/C (unison, dual warp) + noise + sub, each routed
 *        to Filter (balance 1↔2) / Main / Direct / None → optional oversampling → two filters (serial or
 *        parallel, every FilterType) → amp envelope → Main and Direct outputs. Modulated per voice by
 *        Env1-4, LFO1-10, macros and performance sources through the 64-slot matrix.
 *
 * Modulation runs at control rate (every kControlBlock samples): the voice copies the base values, applies
 * its matrix slots in normalized space, clamps, and rebuilds its settings. Slots from a (non-chaos) LFO to
 * oscillator pitch/level/wavetable position or filter cutoff instead run per sample, so audio-rate LFOs
 * (up to 1 kHz) modulate without stair-stepping.
 */
class Voice {
public:
    static void initDownsamplerCoefs() noexcept;   // call once, off the audio thread

    Voice();
    ~Voice();
    Voice(Voice&&) noexcept;
    Voice& operator=(Voice&&) noexcept;

    void prepare(double sampleRate) noexcept;

    void start(int note, int velocity, std::uint64_t order, const ControlContext& ctx, Rng& rng) noexcept;
    void release() noexcept;
    void kill() noexcept;

    /** Retrigger an in-use voice for a new note (stealing): envelopes attack from their current level and
     *  oscillator phases continue, so the steal doesn't click. */
    void steal(int note, int velocity, std::uint64_t order, const ControlContext& ctx) noexcept;

    /** Control tick: evaluate modulation, rebuild settings, recompute layout. */
    void control(const ControlContext& ctx) noexcept;

    /** Adds into the output buses (numSamples <= kControlBlock). */
    void render(const VoiceOutputs& out, int numSamples, const VoiceTables& tables) noexcept;

    bool          active() const noexcept { return m_env[0].isActive(); }
    bool          released() const noexcept { return m_released; }
    int           note() const noexcept { return m_note; }
    std::uint64_t order() const noexcept { return m_order; }
    bool          sustained() const noexcept { return m_sustained; }
    void          setSustained(bool s) noexcept { m_sustained = s; }

    /** Current value of a source for this voice (for tests / meters). */
    float sourceValue(modulation::Source s) const noexcept;

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

        // Phase 7 (non-wavetable sources)
        modules::OscType type = modules::OscType::Wavetable;
        bool   mapped = true;          // note and velocity inside the oscillator's key/velocity ranges
        float  genGainL = 1.0f, genGainR = 1.0f;
        double genOffsetSemis = 0.0;   // pitch offset (octave/semi/fine/coarse + bend), excluding the note
        float  prevL = 0.0f, prevR = 0.0f;   // last base-rate generator sample (interpolation when oversampling)
        dsp::SamplePlayer::Loop smpLoop = dsp::SamplePlayer::Loop::Off;
        float  smpStart = 0.0f, smpEnd = 1.0f, smpLoopStart = 0.0f, smpLoopEnd = 1.0f, smpXfade = 0.0f;
        bool   smpFileLoop = true;
        dsp::granular::Params grn {};
        dsp::spectral::Params spc {};
    };

    // Per-voice generator state for Sample / Multisample / Granular / Spectral (heap: ~70 KB per voice).
    struct Generator {
        static constexpr int kLayers = 4;   // simultaneous SFZ regions (layers, release triggers)
        std::array<dsp::SamplePlayer, kLayers> players {};
        std::array<int, kLayers> region {};
        int  playerCount = 0;
        bool started = false;
        modules::OscType startedType = modules::OscType::Wavetable;
        std::uint64_t assetId = 0;          // the asset the generator was started on
        dsp::granular::Engine granular;
        dsp::spectral::Voice  spectral;
        std::array<float, kControlBlock> left {}, right {};
    };

    struct FastSlot {
        int   lfo = 0;
        float scale = 0.0f;            // amount · aux factor · output scale
        float curve = 0.0f;
        bool  bipolar = false;
        modules::FastDest dest {};
    };

    void evaluateModulation(const ControlContext& ctx) noexcept;
    void layout(const VoiceControl& control) noexcept;
    void resetDownsamplers() noexcept;
    void configureModSources(const ControlContext& ctx) noexcept;
    void startGenerator(int o, const VoiceTables& tables) noexcept;
    void startReleaseRegions(int o, const VoiceTables& tables) noexcept;
    bool renderGenerator(int o, int numSamples, const VoiceTables& tables) noexcept;
    float sourceAtTick(modulation::Source s) const noexcept;

    std::array<Osc, kOscCount> m_osc {};
    std::unique_ptr<std::array<Generator, kOscCount>> m_gen;
    bool m_releasePending = false;   // start SFZ trigger=release regions at the next render
    int  m_velocityRaw = 0;

    // Sub (computed when audible or when a warp uses it as an FM source)
    bool   m_subOn = false;
    bool   m_subNeeded = false;
    int    m_subShape = 0;
    double m_subPhase = 0.0, m_subInc = 0.0;
    dsp::WavetableBank::LevelChoice m_subLevels { 0, 0.0f };
    float  m_subGainL = 1.0f, m_subGainR = 1.0f;
    LinearSmoother m_subLevel;

    // Noise (read when audible or when a warp uses it as an FM source)
    bool   m_noiseOn = false;
    bool   m_noiseNeeded = false;
    dsp::NoiseTables::Type m_noiseType = dsp::NoiseTables::Type::White;
    double m_noisePos = 0.0, m_noiseRate = 1.0;
    float  m_noiseGainL = 1.0f, m_noiseGainR = 1.0f;
    LinearSmoother m_noiseLevel;

    // Routing: per source (osc A/B/C, noise, sub) the weight it sends to each bus.
    enum Bus { kBusF1 = 0, kBusF2, kBusMain, kBusDirect, kBusB1, kBusB2, kBusCount };
    static constexpr int kSourceCount = kOscCount + 2;   // osc 0..2, noise, sub
    static constexpr int kNoiseSource = kOscCount, kSubSource = kOscCount + 1;
    // Each source feeds at most two buses (a filter balance); bypassed filters are folded into the
    // routing at control rate, so their buses only exist while the filter is actually running.
    struct Send { int bus[4] = {kBusMain, kBusMain, kBusMain, kBusMain}; float gain[4] = {}; int count = 0; };
    std::array<Send, kSourceCount> m_sends {};
    std::array<bool, kBusCount> m_busUsed {};

    // Post-oscillator
    int m_oversample = 1;
    std::array<hiir::Downsampler2xFpu<kDownsamplerCoefs>, kBusCount * 4> m_down;   // per bus: [L 4→2, R 4→2, L 2→1, R 2→1]
    std::array<dsp::FilterUnit, modules::FilterModule::kCount> m_filters;
    std::array<bool, modules::FilterModule::kCount> m_filterOn {};
    std::array<modules::FilterOutput, modules::FilterModule::kCount> m_filterOut {};
    std::array<float, modules::FilterModule::kCount> m_cutoffNorm {};   // modulated, before keytrack (fast-path base)
    std::array<float, modules::FilterModule::kCount> m_keytrackMul {};
    modules::FilterRouting m_filterRouting = modules::FilterRouting::Serial;

    // Modulation
    std::array<dsp::Envelope, modulation::kEnvCount> m_env;   // m_env[0] = amplitude
    std::array<modulation::LfoState, modulation::kLfoCount> m_lfo;
    std::array<bool, modulation::kLfoCount> m_lfoUsed {};     // referenced by any slot this tick
    std::array<bool, modulation::kLfoCount> m_lfoFast {};     // advanced per sample by the fast path
    std::array<FastSlot, modulation::kSlotCount> m_fast {};
    int   m_fastCount = 0;
    std::array<float, modules::ModTargets::kMaxTargets> m_plain {};
    std::array<float, modules::ModTargets::kMaxTargets> m_delta {};
    std::array<std::int16_t, modulation::kSlotCount> m_touched {};
    VoiceSettings m_settings {};
    GlobalModState m_global {};
    std::array<float, modulation::kMacroCount> m_macro {};
    float m_velocity = 0.0f, m_noteNorm = 0.0f, m_random1 = 0.0f, m_random2 = 0.0f;
    const modules::EngineModules* m_modules = nullptr;

    double        m_sampleRate = 48000.0;
    int           m_note = -1;
    std::uint64_t m_order = 0;
    bool          m_released = true;
    bool          m_sustained = false;
};

} // namespace winerose::voice
