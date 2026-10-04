#pragma once

#include "engine/dsp/filters/FilterUnit.h"
#include "engine/fx/Effect.h"
#include "engine/fx/FxDsp.h"

#include "hiir/Downsampler2xFpu.h"
#include "hiir/PhaseHalfPiFpu.h"
#include "hiir/Upsampler2xFpu.h"

#include <array>
#include <memory>
#include <vector>

namespace winerose::fx {

// Generic parameter meanings live in EffectRegistry.cpp (paramNames()). All formulas are published
// textbook algorithms (SPEC §1.8 table); tuning constants are INFERRED.

class Utility final : public Effect {
public:
    Utility() : Effect(FxType::Utility) {}
    void prepare(double sr) override;
    void reset() noexcept override;
    void setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept override;
    void process(float* l, float* r, int n) noexcept override;
private:
    double m_sr = 48000;
    float m_gain = 1, m_pan = 0, m_width = 1;
    bool m_invL = false, m_invR = false, m_swap = false, m_bassMono = false;
    std::array<dsp::Lr4, 2> m_bassSplit;   // [channel]
    float m_panL = 1, m_panR = 1;
};

class Eq final : public Effect {
public:
    Eq() : Effect(FxType::Eq) {}
    void prepare(double sr) override;
    void reset() noexcept override;
    void setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept override;
    void process(float* l, float* r, int n) noexcept override;
private:
    double m_sr = 48000;
    std::array<dsp::Biquad, 2> m_low, m_high;   // [channel]
};

class FilterFx final : public Effect {
public:
    FilterFx() : Effect(FxType::Filter) {}
    void prepare(double sr) override;
    void reset() noexcept override;
    void setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept override;
    void process(float* l, float* r, int n) noexcept override;
private:
    winerose::dsp::FilterUnit m_filter;
};

class Distortion final : public Effect {
public:
    Distortion() : Effect(FxType::Distortion) {}
    void prepare(double sr) override;
    void reset() noexcept override;
    void setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept override;
    void process(float* l, float* r, int n) noexcept override;
    enum class Mode { Tube = 0, SoftClip, HardClip, Diode, LinearFold, SineFold, ZeroSquare, Downsample, Count };
private:
    float shape(float x) const noexcept;
    double m_sr = 48000;
    Mode m_mode = Mode::SoftClip;
    float m_drive = 1, m_bias = 0, m_biasOffset = 0, m_out = 1;
    int m_filterPos = 0;   // 0 off, 1 pre, 2 post
    std::array<dsp::Biquad, 2> m_filter;
    std::array<hiir::Upsampler2xFpu<6>, 4> m_up;       // [ch·2 + stage]
    std::array<hiir::Downsampler2xFpu<6>, 4> m_down;
    std::array<float, 2> m_hold {};
    std::array<float, 2> m_holdPhase {};
    float m_holdStep = 1;
};

class Compressor final : public Effect {
public:
    Compressor() : Effect(FxType::Compressor) {}
    void prepare(double sr) override;
    void reset() noexcept override;
    void setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept override;
    void process(float* l, float* r, int n) noexcept override;
    /** Static gain computer (dB in → dB gain change), exposed for tests. */
    static float gainComputer(float inDb, float thresholdDb, float ratio, float kneeDb) noexcept;
private:
    double m_sr = 48000;
    float m_threshold = -20, m_ratio = 4, m_knee = 0, m_makeup = 1;
    float m_attackCoef = 0, m_releaseCoef = 0;
    float m_envDb = -120;   // smoothed gain change in dB (≤ 0)
};

class Multiband final : public Effect {
public:
    Multiband() : Effect(FxType::Multiband) {}
    void prepare(double sr) override;
    void reset() noexcept override;
    void setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept override;
    void process(float* l, float* r, int n) noexcept override;
private:
    double m_sr = 48000;
    std::array<dsp::Lr4, 2> m_xLow, m_xHigh, m_lowAllpass;   // [channel]
    std::array<float, 3> m_env {};                            // band envelopes (linear peak)
    std::array<float, 3> m_bandGain {1, 1, 1};
    float m_depth = 0.5f, m_up = 1, m_down = 1, m_out = 1;
    float m_attackCoef = 0, m_releaseCoef = 0;
};

class Flanger final : public Effect {
public:
    Flanger() : Effect(FxType::Flanger) {}
    void prepare(double sr) override;
    void reset() noexcept override;
    void setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept override;
    void process(float* l, float* r, int n) noexcept override;
private:
    double m_sr = 48000, m_phase = 0;
    float m_rate = 0.2f, m_depth = 0.5f, m_feedback = 0, m_stereo = 0.25f;
    std::array<dsp::DelayLine, 2> m_delay;
    std::array<float, 2> m_last {};
};

class Phaser final : public Effect {
public:
    Phaser() : Effect(FxType::Phaser) {}
    void prepare(double sr) override;
    void reset() noexcept override;
    void setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept override;
    void process(float* l, float* r, int n) noexcept override;
private:
    double m_sr = 48000, m_phase = 0;
    float m_rate = 0.3f, m_depth = 0.6f, m_center = 800, m_feedback = 0, m_stereo = 0.25f;
    int m_stages = 4;
    std::array<std::array<float, 12>, 2> m_state {};
    std::array<float, 2> m_last {};
};

class Chorus final : public Effect {
public:
    Chorus() : Effect(FxType::Chorus) {}
    void prepare(double sr) override;
    void reset() noexcept override;
    void setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept override;
    void process(float* l, float* r, int n) noexcept override;
private:
    double m_sr = 48000, m_phase = 0;
    float m_rate = 0.5f, m_depth = 0.4f, m_delay1 = 8, m_delay2 = 14, m_feedback = 0;
    std::array<dsp::DelayLine, 2> m_delay;
    std::array<dsp::OnePole, 2> m_lpf;
    std::array<float, 2> m_last {};
};

class Delay final : public Effect {
public:
    Delay() : Effect(FxType::Delay) {}
    void prepare(double sr) override;
    void reset() noexcept override;
    void setParams(const std::array<float, kParamCount>& p, const FxContext& ctx) noexcept override;
    void process(float* l, float* r, int n) noexcept override;
    enum class Mode { Normal = 0, PingPong, Tap, Count };
    static constexpr double kMaxSeconds = 4.0;
private:
    double m_sr = 48000;
    Mode m_mode = Mode::Normal;
    float m_timeL = 9600, m_timeR = 9600;           // samples (targets)
    float m_curL = 9600, m_curR = 9600;             // smoothed
    float m_feedback = 0.4f;
    bool  m_filterOn = false;
    bool  m_snapTime = true;   // first update after reset/prepare jumps straight to the new time
    std::array<dsp::DelayLine, 2> m_delay;
    std::array<dsp::Biquad, 2> m_bp;
};

class Hyper final : public Effect {
public:
    Hyper() : Effect(FxType::Hyper) {}
    void prepare(double sr) override;
    void reset() noexcept override;
    void setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept override;
    void process(float* l, float* r, int n) noexcept override;
private:
    double m_sr = 48000, m_phase = 0, m_dimPhase = 0;
    float m_rate = 0.6f, m_detune = 0.3f, m_hyperMix = 0.5f, m_dimSize = 0.5f, m_dimMix = 0.0f;
    int m_voices = 4;
    std::array<dsp::DelayLine, 2> m_delay;
};

class Bode final : public Effect {
public:
    Bode() : Effect(FxType::Bode) {}
    void prepare(double sr) override;
    void reset() noexcept override;
    void setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept override;
    void process(float* l, float* r, int n) noexcept override;
private:
    double m_sr = 48000, m_phase = 0;
    float m_shiftHz = 0, m_side = 0, m_feedback = 0;
    std::array<hiir::PhaseHalfPiFpu<8>, 2> m_hilbert;   // 8 coefficients
    std::array<float, 2> m_last {};
};

class Reverb final : public Effect {
public:
    Reverb() : Effect(FxType::Reverb) {}
    void prepare(double sr) override;
    void reset() noexcept override;
    void setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept override;
    void process(float* l, float* r, int n) noexcept override;
    enum class Mode { Plate = 0, Hall, Count };
private:
    void processHall(float inL, float inR, float& outL, float& outR) noexcept;
    void processPlate(float inL, float inR, float& outL, float& outR) noexcept;
    double m_sr = 48000;
    Mode m_mode = Mode::Hall;
    float m_rt60 = 2.0f, m_size = 0.5f, m_damp = 0.0f, m_width = 1.0f;
    dsp::DelayLine m_preL, m_preR;
    float m_preDelay = 1;
    dsp::Biquad m_lowCutL, m_lowCutR, m_highCutL, m_highCutR;
    // Hall: 8-line FDN (Hadamard mixing), 4 input diffusers.
    std::array<dsp::DelayLine, 8> m_lines;
    std::array<int, 8> m_lineLen {};
    std::array<float, 8> m_lineGain {};
    std::array<dsp::OnePole, 8> m_lineDamp;
    std::array<dsp::DelayLine, 4> m_diffuser;
    std::array<int, 4> m_diffLen {};
    // Plate: Dattorro tank.
    std::array<dsp::DelayLine, 4> m_plateIn;      // input diffusers
    std::array<int, 4> m_plateInLen {};
    std::array<dsp::DelayLine, 2> m_tankAp1, m_tankDelay1, m_tankAp2, m_tankDelay2;
    std::array<int, 2> m_ap1Len {}, m_d1Len {}, m_ap2Len {}, m_d2Len {};
    std::array<dsp::OnePole, 2> m_tankDamp;
    std::array<float, 2> m_tankOut {};
    std::array<float, 2> m_apInterp {};   // first-order allpass interpolator state (modulated taps)
    std::array<float, 4> m_plateInGain {};
    std::array<float, 2> m_ap1Gain {}, m_ap2Gain {};   // diffusion, reduced for short decays
    float m_plateDecay = 0.5f;
    double m_lfo = 0;
};

class Convolve final : public Effect {
public:
    Convolve();
    ~Convolve() override;
    void prepare(double sr) override;
    void reset() noexcept override;
    void setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept override;
    void process(float* l, float* r, int n) noexcept override;

    static constexpr int kHead = 64;        // direct-form taps: zero latency
    static constexpr int kSmallBlock = 64;  // partitions for taps [64, 256)
    static constexpr int kLargeBlock = 256; // partitions for taps [256, length)
    static constexpr double kMaxSeconds = 2.5;

    /** The built-in impulse response for one channel (decaying noise, original content); for tests. */
    const std::vector<float>& impulse(int channel) const noexcept { return m_ir[static_cast<std::size_t>(channel)]; }

private:
    struct Stage;   // uniform-partitioned overlap-save stage (pffft)
    float processSample(int ch, float x) noexcept;

    double m_sr = 48000;
    std::array<std::vector<float>, 2> m_ir;
    std::array<std::vector<float>, 2> m_headHistory;
    std::array<int, 2> m_headPos {};
    std::array<std::unique_ptr<Stage>, 4> m_stages;   // [ch·2 + {small, large}]
    int m_activeLength = 0;                           // taps in use (from the length parameter)
    dsp::DelayLine m_preL, m_preR;
    float m_preDelay = 1;
    bool  m_lowCutOn = false, m_highCutOn = false;   // off at the knob extremes
    dsp::Biquad m_lowCutL, m_lowCutR, m_highCutL, m_highCutR;
};

} // namespace winerose::fx
