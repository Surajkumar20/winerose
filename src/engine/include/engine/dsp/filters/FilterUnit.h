#pragma once

#include "engine/dsp/Svf.h"
#include "engine/dsp/filters/FilterTypes.h"

#include "hiir/Downsampler2xFpu.h"
#include "hiir/Upsampler2xFpu.h"

#include <array>
#include <cstdint>
#include <vector>

namespace winerose::dsp {

/** Plain settings of one filter for one control tick (keytrack already folded into cutoffHz). */
struct FilterSettings {
    FilterType type = FilterType::Lp12;
    float cutoffHz  = 425.0f;
    float resonance = 0.1f;    // 0..1
    float drive     = 0.0f;    // 0..1 → 0..+24 dB into a saturator (0 = fully linear path)
    bool  clean     = false;   // drive without the input saturator (Serum 2's "Clean" mode, INFERRED)
    float var       = 0.5f;    // family-specific (morph, second cutoff, damping, vowel, ...)
    float x         = 0.0f;    // PZ morph
    float y         = 0.5f;
    float stereo    = 0.0f;    // 0..1: L/R cutoff offset up to ±½ octave (INFERRED)
    float mix       = 1.0f;    // dry/wet
    float level     = 1.0f;    // output gain
};

/** Per-channel coefficients computed from FilterSettings (and a per-channel cutoff). */
struct FilterCoefs {
    Svf::Coefs svf {}, svf2 {}, svfFlat {};
    float G = 0.0f, G2 = 0.0f;           // one-pole / ladder stage gain, second one-pole
    float k = 0.0f;                      // ladder feedback
    float comp = 1.0f;                   // ladder passband compensation
    float delay = 1.0f;                  // comb / flange delay in samples
    float feedback = 0.0f;
    float damp = 0.0f;                   // one-pole G of the comb / FDN damping filter
    std::array<float, 12> apG {};        // phaser stage gains
    std::array<Svf::Coefs, 3> formant {};
    std::array<float, 3> formantGain {};
    double rate = 0.0;                   // S&H / ring-mod cycles per sample
    std::array<int, 4> lines {};         // FDN / diffuser delay lengths
    int stages = 0;                      // phaser / disperser stage count
};

/**
 * @class FilterUnit
 * @brief One stereo filter of one voice: every FilterType, drive, mix, level, stereo spread, and 2×
 *        oversampling for the nonlinear families. Allocation-free after construction.
 *
 * Linear families are exact TPT / bilinear discretizations of their analog prototypes, which is what the
 * Phase 4 acceptance test compares against ("linear responses within 0.5 dB of analytic").
 */
class FilterUnit {
public:
    static constexpr int kDelaySize = 8192;   // per channel: combs down to sr/8192 Hz; 4 × 2048 for FDN/diffuser

    FilterUnit();

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    /** Control tick: new settings; recomputes both channels' coefficients. */
    void set(const FilterSettings& settings) noexcept;

    /** Per-sample cutoff override (audio-rate modulation); keeps the other settings. */
    void setCutoff(float cutoffHz) noexcept;

    /** Process one stereo sample in place. */
    void process(float& left, float& right) noexcept;

    const FilterSettings& settings() const noexcept { return m_settings; }

    /** Pure coefficient computation (exposed for tests). sampleRate is the rate the core runs at. */
    static FilterCoefs computeCoefs(const FilterSettings& s, double cutoffHz, double sampleRate) noexcept;

private:
    struct Channel {
        Svf svf, svf2;
        float onePole = 0.0f, onePole2 = 0.0f;
        std::array<float, 4> ladder {};
        float lastOut = 0.0f;
        std::array<float, 12> ap {};
        std::array<Svf, 32> disperser {};
        std::array<Svf, 3> formant {};
        float hold = 0.0f;
        double holdPhase = 1.0, ringPhase = 0.0;
        std::vector<float> buffer;
        int writePos = 0;
        std::array<int, 4> segPos {};
        float combDamp = 0.0f;
        std::array<float, 4> fdnDamp {};
        hiir::Upsampler2xFpu<8>   up;
        hiir::Downsampler2xFpu<8> down;
    };

    void  updateCoefs() noexcept;
    float processCore(Channel& ch, const FilterCoefs& c, float x) noexcept;
    float processChannel(Channel& ch, const FilterCoefs& c, float x) noexcept;
    float readDelay(const Channel& ch, float delay) const noexcept;

    FilterSettings m_settings;
    double m_sampleRate = 48000.0;
    std::array<Channel, 2> m_ch;
    std::array<FilterCoefs, 2> m_coefs {};
    const FilterInfo* m_info = &kFilterInfo[1];
    float m_preGain = 1.0f, m_postGain = 1.0f;
};

} // namespace winerose::dsp
