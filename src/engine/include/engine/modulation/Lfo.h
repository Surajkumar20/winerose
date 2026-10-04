#pragma once

#include "engine/dsp/WavetableBank.h"

#include <cmath>
#include <cstdint>

namespace winerose::modulation {

/**
 * @brief LFO (SPEC §1.6, §5.5): Path / fixed shapes / random / chaos, Free / Trig / Env modes, Hz or BPM
 *        rate (0.01 Hz - 1 kHz), delay, rise, smoothing. Output in [0,1].
 *
 * Periodic shapes (the drawable Path and the fixed shapes) are read from band-limited WavetableBanks with a
 * mip level chosen from the LFO's own rate, so an audio-rate LFO (up to 1 kHz) is alias-free when evaluated
 * per sample (SPEC Phase 3 acceptance). Lorenz/Rossler are integrated with RK4 (SPEC constants) and
 * normalized by running min/max; they are control-rate only.
 */
enum class LfoShape : int {
    Path = 0, Sine, Triangle, SawUp, SawDown, Square, SampleHold, SmoothRandom, Lorenz, Rossler, Count
};
inline constexpr const char* kLfoShapeNames[] = {
    "Path", "Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H", "Smooth Random", "Lorenz", "Rossler",
};

// Free: one phase shared by every voice (free-running). Trig: restarts at `phase` on each note.
// Env: one-shot from `phase` to the end of the cycle, then holds.
enum class LfoMode : int { Free = 0, Trig, Env, Count };
inline constexpr const char* kLfoModeNames[] = {"Free", "Trig", "Env"};

// BPM divisions: name and length in quarter notes.
struct SyncDivision { const char* name; double beats; };
inline constexpr SyncDivision kSyncDivisions[] = {
    {"8 bars", 32.0}, {"4 bars", 16.0}, {"2 bars", 8.0}, {"1 bar", 4.0},
    {"1/2", 2.0}, {"1/2 D", 3.0}, {"1/2 T", 4.0 / 3.0},
    {"1/4", 1.0}, {"1/4 D", 1.5}, {"1/4 T", 2.0 / 3.0},
    {"1/8", 0.5}, {"1/8 D", 0.75}, {"1/8 T", 1.0 / 3.0},
    {"1/16", 0.25}, {"1/16 D", 0.375}, {"1/16 T", 1.0 / 6.0},
    {"1/32", 0.125}, {"1/32 D", 0.1875}, {"1/32 T", 1.0 / 12.0},
    {"1/64", 0.0625},
};
inline constexpr int kSyncDivisionCount = static_cast<int>(sizeof(kSyncDivisions) / sizeof(kSyncDivisions[0]));
inline constexpr int kDefaultSyncDivision = 7;   // 1/4

constexpr bool isPeriodicTableShape(LfoShape s) noexcept { return s <= LfoShape::Square; }
constexpr bool isChaos(LfoShape s) noexcept { return s == LfoShape::Lorenz || s == LfoShape::Rossler; }

/** Plain per-tick settings of one LFO (from its module, possibly modulated). */
struct LfoSettings {
    LfoShape shape = LfoShape::Path;
    LfoMode  mode  = LfoMode::Trig;
    bool     sync  = true;
    int      division = kDefaultSyncDivision;
    float    rateHz = 1.0f;
    float    phase = 0.0f;
    float    delaySeconds = 0.0f;
    float    riseSeconds = 0.0f;
    float    smooth = 0.0f;   // 0..1 → one-pole time constant 0..100 ms (INFERRED)

    double frequency(double bpm) const noexcept
    {
        if (!sync) return rateHz;
        const int d = division < 0 ? 0 : (division >= kSyncDivisionCount ? kSyncDivisionCount - 1 : division);
        return (bpm / 60.0) / kSyncDivisions[d].beats;
    }
};

/** Shape tables for one LFO: the fixed shapes bank (frames in LfoShape order Sine..Square) and its path. */
struct LfoTables {
    const dsp::WavetableBank* shapes = nullptr;
    const dsp::WavetableBank* path   = nullptr;
};

/** Per-voice LFO state. Allocation-free. */
class LfoState {
public:
    /** Note-on. freePhase is the shared phase used in Free mode. seed decorrelates random/chaos per voice. */
    void start(const LfoSettings& s, double freePhase, std::uint64_t seed) noexcept;

    /** Per-tick setup: increments and smoothing coefficient for the coming samples. */
    void configure(const LfoSettings& s, double sampleRate, double bpm) noexcept;

    /** Advance by n samples (control-rate path); n = 0 just refreshes value(). */
    void advance(int n, const LfoTables& tables) noexcept;

    /** Advance one sample and return the new output (audio-rate path). */
    float tick(const LfoTables& tables) noexcept;

    /** Current output in [0,1] (after delay, rise and smoothing). */
    float value() const noexcept { return m_output; }

private:
    float raw(const LfoTables& tables) const noexcept;
    void  onWrap() noexcept;
    void  integrateChaos(double seconds) noexcept;
    void  initChaos() noexcept;
    float shaped(const LfoTables& tables) noexcept;   // raw · rise gain, held during delay
    double nextRandom() noexcept;

    LfoSettings m_settings;
    double m_sampleRate = 48000.0;
    double m_phase = 0.0;
    double m_inc = 0.0;           // cycles per sample
    double m_time = 0.0;          // seconds since note-on
    bool   m_finished = false;    // Env mode reached the end of its cycle
    float  m_output = 0.0f;
    float  m_smoothCoef = 0.0f;   // per-sample one-pole coefficient (0 = no smoothing)
    bool   m_smoothPrimed = false;
    dsp::WavetableBank::LevelChoice m_levels { 0, 0.0f };

    // Random shapes
    std::uint64_t m_rng = 1;
    float m_randPrev = 0.5f, m_randNext = 0.5f;

    // Chaos (x, y, z) and running output range
    double m_cx = 0.1, m_cy = 0.0, m_cz = 0.0;
    double m_min = 0.0, m_max = 0.0;
};

} // namespace winerose::modulation
