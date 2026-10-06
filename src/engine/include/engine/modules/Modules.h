#pragma once

#include "engine/dsp/Curve.h"
#include "engine/dsp/Envelope.h"
#include "engine/dsp/Granular.h"
#include "engine/dsp/SampleData.h"
#include "engine/dsp/NoiseTable.h"
#include "engine/dsp/Unison.h"
#include "engine/dsp/Warp.h"
#include "engine/dsp/WavetableBank.h"
#include "engine/dsp/filters/FilterUnit.h"
#include "engine/modulation/Lfo.h"
#include "engine/modulation/ModTargets.h"
#include "engine/modulation/Sources.h"

#include "params/ConfigManager.h"
#include "params/ParamHandle.h"
#include "params/ParamRegistry.h"

#include <array>
#include <memory>
#include <string>

namespace winerose::modules {

// Each module owns its ParamRegistry (named like the Serum 2 CBOR module it mirrors), registers its
// parameters in the constructor, adds every numeric one to the engine's ModTargets, and reads its plain
// values back from a (possibly modulated) value array with read(const float*). Keys are listed next to
// each module; UIs address them as "<Module><index>.<key>". Defaults and enum orders marked TODO-MEASURE
// await the measure_host sweep of Serum 2 (SPEC §5.10).

using modulation::ModTargets;

// Where a source's signal goes (SPEC §1.1 routing): through the filters (with a Filter 1 ↔ 2 balance), to
// Main (through the FX, Phase 5), Direct (bypassing filters and FX), or nowhere (modulation-only).
enum class Route : int { Filter = 0, Main, Direct, None, Count };
inline constexpr const char* kRouteNames[] = {"Filter", "Main", "Direct", "None"};

// Where a filter's output goes.
enum class FilterOutput : int { Main = 0, Direct, Count };
inline constexpr const char* kFilterOutputNames[] = {"Main", "Direct"};

// How the two filters connect.
enum class FilterRouting : int { Serial = 0, Parallel, Count };
inline constexpr const char* kFilterRoutingNames[] = {"Serial", "Parallel"};

// What an oscillator plays (SPEC §1.4). Winerose order; append only.
enum class OscType : int { Wavetable = 0, Sample, Multisample, Granular, Spectral, Count };
inline constexpr const char* kOscTypeNames[] = {"Wavetable", "Sample", "Multisample", "Granular", "Spectral"};

namespace route_keys {
inline constexpr const char* route   = "route";
inline constexpr const char* balance = "filterBalance";
inline constexpr const char* bus1    = "bus1Send";
inline constexpr const char* bus2    = "bus2Send";
}

// --- Wavetable oscillator (Oscillator0..2 = A/B/C) ---------------------------------------------------
namespace osc_keys {
inline constexpr const char* enabled      = "enabled";
inline constexpr const char* level        = "level";
inline constexpr const char* pan          = "pan";
inline constexpr const char* octave       = "octave";
inline constexpr const char* semi         = "semi";
inline constexpr const char* fine         = "fine";
inline constexpr const char* coarse       = "coarse";
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
inline constexpr const char* remapCurve   = "remapCurve";   // string: drawable curve for Remap 1/2
// Phase 7: oscillator type, key/velocity mapping, and the per-type controls.
inline constexpr const char* type         = "type";
inline constexpr const char* keyLo        = "keyLo";
inline constexpr const char* keyHi        = "keyHi";
inline constexpr const char* velLo        = "velLo";
inline constexpr const char* velHi        = "velHi";
inline constexpr const char* smpStart     = "smpStart";
inline constexpr const char* smpEnd       = "smpEnd";
inline constexpr const char* smpLoop      = "smpLoop";
inline constexpr const char* smpLoopStart = "smpLoopStart";
inline constexpr const char* smpLoopEnd   = "smpLoopEnd";
inline constexpr const char* smpXfade     = "smpXfade";
inline constexpr const char* smpFileLoop  = "smpFileLoop";
inline constexpr const char* grnPos       = "grnPos";
inline constexpr const char* grnScan      = "grnScan";
inline constexpr const char* grnSize      = "grnSize";
inline constexpr const char* grnDensity   = "grnDensity";
inline constexpr const char* grnPosRand   = "grnPosRand";
inline constexpr const char* grnPitchRand = "grnPitchRand";
inline constexpr const char* grnPanRand   = "grnPanRand";
inline constexpr const char* grnWindow    = "grnWindow";
inline constexpr const char* grnWindowAmt = "grnWindowAmt";
inline constexpr const char* spcPos       = "spcPos";
inline constexpr const char* spcScan      = "spcScan";
inline constexpr const char* spcTimbre    = "spcTimbre";
inline constexpr const char* spcFormant   = "spcFormant";
inline constexpr const char* spcLowCut    = "spcLowCut";
inline constexpr const char* spcHighCut   = "spcHighCut";
inline constexpr const char* spcTransients = "spcTransients";
// Asset files (strings; the control layer loads them, and reloads them when a state is restored).
inline constexpr const char* wavetablePath   = "wavetablePath";
inline constexpr const char* samplePath      = "samplePath";
inline constexpr const char* multisamplePath = "multisamplePath";
inline constexpr const char* wavetableData   = "wavetableData";   // embedded table (imports): see control/AssetLoader
}

class OscillatorModule {
public:
    static constexpr int kCount = 3;

    struct Values {
        bool  enabled;
        float level;          // linear 0..1 (TODO-MEASURE dB law)
        float pan;            // -1..1
        float pitchSemis;     // octave·12 + semi + coarse + fine/100
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
        Route route;
        float filterBalance;  // 0 = Filter 1, 1 = Filter 2
        float bus1Send, bus2Send;

        OscType type;
        int   keyLo, keyHi, velLo, velHi;   // the oscillator only sounds for notes inside both ranges
        // Sample
        float smpStart, smpEnd;             // 0..1 of the sample
        dsp::SamplePlayer::Loop smpLoop;
        float smpLoopStart, smpLoopEnd;     // 0..1 of the sample (used unless smpFileLoop and the file has a loop)
        float smpXfade;                     // 0..0.5 of the loop length
        bool  smpFileLoop;
        // Granular
        float grnPos, grnScan, grnSize, grnDensity, grnPosRand, grnPitchRand, grnPanRand;
        dsp::granular::Window grnWindow;
        float grnWindowAmt;
        // Spectral
        float spcPos, spcScan, spcTimbre, spcFormant, spcLowCut, spcHighCut;
        bool  spcTransients;
    };
    static constexpr float kPhaseMem = 0.999f;

    struct Indices {
        int enabled, level, pan, octave, semi, fine, coarse, wtPos, wtSmooth, phase, random, unison, uniDetune,
            uniBlend, uniWidth, uniRange, uniStack, uniMode, uniSpan, uniRandStart, uniWarp, warp1Mode,
            warp1Amount, warp2Mode, warp2Amount, route, balance, bus1, bus2;
        int type, keyLo, keyHi, velLo, velHi, smpStart, smpEnd, smpLoop, smpLoopStart, smpLoopEnd, smpXfade,
            smpFileLoop, grnPos, grnScan, grnSize, grnDensity, grnPosRand, grnPitchRand, grnPanRand, grnWindow,
            grnWindowAmt, spcPos, spcScan, spcTimbre, spcFormant, spcLowCut, spcHighCut, spcTransients;
    };

    OscillatorModule(std::shared_ptr<ConfigManager> config, int index, ModTargets& targets);
    Values read(const float* v) const noexcept;
    const Indices& indices() const noexcept { return m_i; }
    ParamRegistry& registry() noexcept { return *m_registry; }
    std::string remapCurve() const;   // message thread

private:
    std::unique_ptr<ParamRegistry> m_registry;
    Indices m_i {};
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
        Route route;
        float filterBalance;
        float bus1Send, bus2Send;
    };

    NoiseModule(std::shared_ptr<ConfigManager> config, ModTargets& targets);
    Values read(const float* v) const noexcept;
    ParamRegistry& registry() noexcept { return *m_registry; }

private:
    std::unique_ptr<ParamRegistry> m_registry;
    int m_enabled, m_type, m_level, m_pan, m_keytrack, m_pitch, m_route, m_balance, m_bus1, m_bus2;
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
        Route route;
        float filterBalance;
        float bus1Send, bus2Send;
    };

    SubOscModule(std::shared_ptr<ConfigManager> config, ModTargets& targets);
    Values read(const float* v) const noexcept;
    ParamRegistry& registry() noexcept { return *m_registry; }

private:
    std::unique_ptr<ParamRegistry> m_registry;
    int m_enabled, m_shape, m_octave, m_level, m_pan, m_route, m_balance, m_bus1, m_bus2;
};

// --- Filter (Filter0..1) -----------------------------------------------------------------------------
namespace filter_keys {
inline constexpr const char* enabled   = "enabled";
inline constexpr const char* type      = "type";
inline constexpr const char* cutoff    = "cutoff";
inline constexpr const char* resonance = "resonance";
inline constexpr const char* drive     = "drive";
inline constexpr const char* clean     = "clean";
inline constexpr const char* var       = "var";
inline constexpr const char* x         = "x";
inline constexpr const char* y         = "y";
inline constexpr const char* stereo    = "stereo";
inline constexpr const char* mix       = "mix";
inline constexpr const char* level     = "level";
inline constexpr const char* keytrack  = "keytrack";
inline constexpr const char* output    = "output";
}

class FilterModule {
public:
    static constexpr int kCount = 2;

    struct Values {
        bool  enabled;               // disabled = bypass (the routed signal passes through unfiltered)
        dsp::FilterSettings settings;
        float keytrack;              // 0..1: octaves of cutoff per octave of note, from C4
        FilterOutput output;
    };

    FilterModule(std::shared_ptr<ConfigManager> config, int index, ModTargets& targets);
    Values read(const float* v) const noexcept;
    int cutoffIndex() const noexcept { return m_cutoff; }
    ParamRegistry& registry() noexcept { return *m_registry; }

private:
    std::unique_ptr<ParamRegistry> m_registry;
    int m_enabled, m_type, m_cutoff, m_resonance, m_drive, m_clean, m_var, m_x, m_y, m_stereo, m_mix, m_level,
        m_keytrack, m_output;
};

// --- Filter routing ("Routing") ----------------------------------------------------------------------
class RoutingModule {
public:
    RoutingModule(std::shared_ptr<ConfigManager> config, ModTargets& targets);
    FilterRouting read(const float* v) const noexcept;
    ParamRegistry& registry() noexcept { return *m_registry; }

private:
    std::unique_ptr<ParamRegistry> m_registry;
    int m_filterRouting;
};

// --- Envelope (Env0..3; Env0 drives amplitude) -------------------------------------------------------
namespace env_keys {
inline constexpr const char* attack       = "attack";
inline constexpr const char* hold         = "hold";
inline constexpr const char* decay        = "decay";
inline constexpr const char* sustain      = "sustain";
inline constexpr const char* release      = "release";
inline constexpr const char* attackCurve  = "attackCurve";
inline constexpr const char* decayCurve   = "decayCurve";
inline constexpr const char* releaseCurve = "releaseCurve";
}

class EnvelopeModule {
public:
    EnvelopeModule(std::shared_ptr<ConfigManager> config, int index, ModTargets& targets);
    dsp::Envelope::Settings read(const float* v) const noexcept;
    ParamRegistry& registry() noexcept { return *m_registry; }

private:
    std::unique_ptr<ParamRegistry> m_registry;
    int m_attack, m_hold, m_decay, m_sustain, m_release, m_attackCurve, m_decayCurve, m_releaseCurve;
};

// --- LFO (LFO0..9) -----------------------------------------------------------------------------------
namespace lfo_keys {
inline constexpr const char* shape    = "shape";
inline constexpr const char* path     = "path";       // string: drawable shape for LfoShape::Path
inline constexpr const char* mode     = "mode";
inline constexpr const char* sync     = "sync";
inline constexpr const char* division = "division";
inline constexpr const char* rate     = "rate";
inline constexpr const char* phase    = "phase";
inline constexpr const char* delay    = "delay";
inline constexpr const char* rise     = "rise";
inline constexpr const char* smooth   = "smooth";
}

class LfoModule {
public:
    LfoModule(std::shared_ptr<ConfigManager> config, int index, ModTargets& targets);
    modulation::LfoSettings read(const float* v) const noexcept;
    ParamRegistry& registry() noexcept { return *m_registry; }
    std::string path() const;   // message thread
    int shapeIndex() const noexcept { return m_shape; }

private:
    std::unique_ptr<ParamRegistry> m_registry;
    int m_shape, m_mode, m_sync, m_division, m_rate, m_phase, m_delay, m_rise, m_smooth;
};

// --- Macro (Macro0..7) -------------------------------------------------------------------------------
namespace macro_keys {
inline constexpr const char* value = "value";
inline constexpr const char* name  = "name";
}

class MacroModule {
public:
    MacroModule(std::shared_ptr<ConfigManager> config, int index, ModTargets& targets);
    float read(const float* v) const noexcept { return v[m_value]; }
    int valueIndex() const noexcept { return m_value; }
    ParamRegistry& registry() noexcept { return *m_registry; }

private:
    std::unique_ptr<ParamRegistry> m_registry;
    int m_value;
};

// --- Mod matrix (ModSlot0..63) -----------------------------------------------------------------------
namespace slot_keys {
inline constexpr const char* source      = "source";
inline constexpr const char* destination = "destination";   // string: namespaced key, e.g. "Filter0.cutoff"
inline constexpr const char* amount      = "amount";
inline constexpr const char* bipolar     = "bipolar";
inline constexpr const char* curve       = "curve";
inline constexpr const char* aux         = "aux";
inline constexpr const char* auxAmount   = "auxAmount";
inline constexpr const char* auxInvert   = "auxInvert";
inline constexpr const char* output      = "output";
inline constexpr const char* bypass      = "bypass";
}

/** Live (per-tick) values of one matrix slot. The destination index comes from the snapshot. */
struct SlotParams {
    modulation::Source source = modulation::Source::None;
    modulation::Source aux    = modulation::Source::None;
    float amount = 0.0f;      // -1..1, in normalized destination units
    float curve = 0.0f;       // -1..1 source curve (power law)
    float auxAmount = 1.0f;   // 0..1
    float output = 1.0f;      // 0..1 output scale
    bool  bipolar = false;
    bool  auxInvert = false;
    bool  bypass = false;
};

class MatrixModule {
public:
    explicit MatrixModule(std::shared_ptr<ConfigManager> config);
    void read(std::array<SlotParams, modulation::kSlotCount>& out) const noexcept;   // realtime
    std::string destination(int slot) const;                                        // message thread
    ParamRegistry& slot(int index) noexcept { return *m_slots[static_cast<std::size_t>(index)]; }

private:
    struct Handles { ParamHandle source, amount, bipolar, curve, aux, auxAmount, auxInvert, output, bypass; };
    std::array<std::unique_ptr<ParamRegistry>, modulation::kSlotCount> m_slots;
    std::array<Handles, modulation::kSlotCount> m_handles {};
};

} // namespace winerose::modules
