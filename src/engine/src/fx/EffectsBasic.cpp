#include "engine/fx/Effects.h"

#include "hiir/PolyphaseIir2Designer.h"

#include <cmath>

namespace winerose::fx {

using namespace dsp;

// --- Utility ---------------------------------------------------------------------------------------------

void Utility::prepare(double sr) { m_sr = sr; reset(); }
void Utility::reset() noexcept { for (auto& s : m_bassSplit) s.reset(); }

void Utility::setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept
{
    const float db = 60.0f * p[0] - 48.0f;                 // p0 = 0.8 → 0 dB
    m_gain  = std::abs(db) < 1e-4f ? 1.0f : dbToGain(db);
    m_pan   = 2.0f * p[1] - 1.0f;
    m_width = 2.0f * p[2];                                 // p2 = 0.5 → 1 (unchanged)
    m_invL  = p[3] >= 0.5f;
    m_invR  = p[4] >= 0.5f;
    m_swap  = p[5] >= 0.5f;
    m_bassMono = p[6] > 0.01f;
    if (m_bassMono)
        for (auto& s : m_bassSplit) s.setKeepState(expo(p[6], 20.0f, 500.0f), m_sr);
    // Balance law: the centre leaves both channels untouched (exact null at neutral settings).
    m_panL = m_pan > 0.0f ? 1.0f - m_pan : 1.0f;
    m_panR = m_pan < 0.0f ? 1.0f + m_pan : 1.0f;
}

void Utility::process(float* l, float* r, int n) noexcept
{
    for (int i = 0; i < n; ++i) {
        float a = l[i], b = r[i];
        if (m_swap) std::swap(a, b);
        if (m_invL) a = -a;
        if (m_invR) b = -b;
        if (m_bassMono) {
            // Mono below the crossover: low bands averaged, high bands kept (low + high = allpass).
            float la, ha, lb, hb;
            m_bassSplit[0].split(a, la, ha);
            m_bassSplit[1].split(b, lb, hb);
            const float mono = 0.5f * (la + lb);
            a = mono + ha;
            b = mono + hb;
        }
        if (m_width != 1.0f) {
            const float mid = 0.5f * (a + b), side = 0.5f * (a - b) * m_width;
            a = mid + side;
            b = mid - side;
        }
        l[i] = a * m_gain * m_panL;
        r[i] = b * m_gain * m_panR;
    }
}

// --- EQ (two RBJ bands) ----------------------------------------------------------------------------------

void Eq::prepare(double sr) { m_sr = sr; reset(); }
void Eq::reset() noexcept { for (auto& b : m_low) b.reset(); for (auto& b : m_high) b.reset(); }

void Eq::setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept
{
    static constexpr Biquad::Kind kLow[]  = {Biquad::Kind::LowShelf, Biquad::Kind::Peak, Biquad::Kind::Highpass};
    static constexpr Biquad::Kind kHigh[] = {Biquad::Kind::HighShelf, Biquad::Kind::Peak, Biquad::Kind::Lowpass};
    for (int ch = 0; ch < 2; ++ch) {
        m_low[static_cast<std::size_t>(ch)].set(kLow[choice(p[0], 3)], expo(p[1], 20.0f, 2000.0f), expo(p[3], 0.3f, 10.0f),
                                                bipolarDb(p[2], 24.0f), m_sr);
        m_high[static_cast<std::size_t>(ch)].set(kHigh[choice(p[4], 3)], expo(p[5], 500.0f, 20000.0f), expo(p[7], 0.3f, 10.0f),
                                                 bipolarDb(p[6], 24.0f), m_sr);
    }
}

void Eq::process(float* l, float* r, int n) noexcept
{
    for (int i = 0; i < n; ++i) {
        l[i] = m_high[0].process(m_low[0].process(l[i]));
        r[i] = m_high[1].process(m_low[1].process(r[i]));
    }
}

// --- Filter (any FilterType as an effect) ----------------------------------------------------------------

void FilterFx::prepare(double sr) { m_filter.prepare(sr); }
void FilterFx::reset() noexcept { m_filter.reset(); }

void FilterFx::setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept
{
    winerose::dsp::FilterSettings s;
    s.type      = static_cast<winerose::dsp::FilterType>(choice(p[0], static_cast<int>(winerose::dsp::FilterType::Count)));
    s.cutoffHz  = expo(p[1], 20.0f, 20000.0f);
    s.resonance = p[2];
    s.drive     = p[3];
    s.var       = p[4];
    s.stereo    = p[5];
    s.x         = p[6];
    s.y         = p[7];
    m_filter.set(s);
}

void FilterFx::process(float* l, float* r, int n) noexcept
{
    for (int i = 0; i < n; ++i) m_filter.process(l[i], r[i]);
}

// --- Distortion (4× oversampled waveshapers) -------------------------------------------------------------

namespace {
double g_distCoefs[6] {};
bool g_distCoefsReady = false;
void initDistCoefs() noexcept
{
    if (g_distCoefsReady) return;
    hiir::PolyphaseIir2Designer::compute_coefs_spec_order_tbw(g_distCoefs, 6, 0.1);
    g_distCoefsReady = true;
}
}

void Distortion::prepare(double sr)
{
    m_sr = sr;
    initDistCoefs();
    for (auto& u : m_up) u.set_coefs(g_distCoefs);
    for (auto& d : m_down) d.set_coefs(g_distCoefs);
    reset();
}

void Distortion::reset() noexcept
{
    for (auto& u : m_up) u.clear_buffers();
    for (auto& d : m_down) d.clear_buffers();
    for (auto& f : m_filter) f.reset();
    m_hold = {};
    m_holdPhase = {};
}

float Distortion::shape(float y) const noexcept
{
    switch (m_mode) {
        case Mode::Tube:       return y >= 0.0f ? std::tanh(y) : std::expm1(y);   // asymmetric: softer negative side
        case Mode::SoftClip:   return std::tanh(y);
        case Mode::HardClip:   return std::clamp(y, -1.0f, 1.0f);
        case Mode::Diode:      return y >= 0.0f ? 1.0f - std::exp(-y) : -0.2f * (1.0f - std::exp(y));
        case Mode::LinearFold: {
            float t = std::fmod(y + 1.0f, 4.0f);
            if (t < 0.0f) t += 4.0f;
            return 1.0f - std::abs(t - 2.0f);
        }
        case Mode::SineFold:   return std::sin(y);
        case Mode::ZeroSquare: return std::clamp(y * std::abs(y), -1.0f, 1.0f);
        case Mode::Downsample:
        case Mode::Count:      break;
    }
    return y;
}

void Distortion::setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept
{
    m_mode  = static_cast<Mode>(choice(p[1], static_cast<int>(Mode::Count)));
    m_drive = dbToGain(lin(p[0], 0.0f, 48.0f));
    m_bias  = lin(p[5], -1.0f, 1.0f);
    m_out   = dbToGain(bipolarDb(p[6], 24.0f));
    m_biasOffset = m_mode == Mode::Downsample ? 0.0f : shape(m_bias);   // remove the bias's static DC
    m_holdStep = 1.0f / (1.0f + 63.0f * p[0] * p[0]);                    // downsample factor 1..64

    const int f = choice(p[2], 7);   // off, pre LP/BP/HP, post LP/BP/HP
    m_filterPos = f == 0 ? 0 : (f <= 3 ? 1 : 2);
    if (m_filterPos != 0) {
        static constexpr Biquad::Kind kinds[] = {Biquad::Kind::Lowpass, Biquad::Kind::Bandpass, Biquad::Kind::Highpass};
        const Biquad::Kind kind = kinds[(f - 1) % 3];
        for (auto& b : m_filter) b.set(kind, expo(p[3], 20.0f, 20000.0f), expo(p[4], 0.5f, 10.0f), 0.0, m_sr);
    }
}

void Distortion::process(float* l, float* r, int n) noexcept
{
    float* ch[2] = {l, r};
    for (int c = 0; c < 2; ++c) {
        for (int i = 0; i < n; ++i) {
            float x = ch[c][i];
            if (m_filterPos == 1) x = m_filter[static_cast<std::size_t>(c)].process(x);
            float y;
            if (m_mode == Mode::Downsample) {
                m_holdPhase[static_cast<std::size_t>(c)] += m_holdStep;
                if (m_holdPhase[static_cast<std::size_t>(c)] >= 1.0f) {
                    m_holdPhase[static_cast<std::size_t>(c)] -= 1.0f;
                    m_hold[static_cast<std::size_t>(c)] = x;
                }
                y = m_hold[static_cast<std::size_t>(c)];
            } else {
                // 1 → 2 → 4 samples, shape, 4 → 2 → 1.
                float a, b, q[4];
                m_up[static_cast<std::size_t>(c * 2)].process_sample(a, b, x);
                m_up[static_cast<std::size_t>(c * 2 + 1)].process_sample(q[0], q[1], a);
                m_up[static_cast<std::size_t>(c * 2 + 1)].process_sample(q[2], q[3], b);
                for (float& s : q) s = shape(s * m_drive + m_bias) - m_biasOffset;
                const float half[2] = {m_down[static_cast<std::size_t>(c * 2 + 1)].process_sample(q),
                                       m_down[static_cast<std::size_t>(c * 2 + 1)].process_sample(q + 2)};
                y = m_down[static_cast<std::size_t>(c * 2)].process_sample(half);
            }
            if (m_filterPos == 2) y = m_filter[static_cast<std::size_t>(c)].process(y);
            ch[c][i] = y * m_out;
        }
    }
}

// --- Compressor ------------------------------------------------------------------------------------------

float Compressor::gainComputer(float x, float t, float ratio, float knee) noexcept
{
    // Soft-knee static curve (Giannoulis, Massberg & Reiss 2012). Returns output dB - input dB (≤ 0).
    const float over = x - t;
    float y;
    if (2.0f * over < -knee)                          y = x;
    else if (knee > 0.0f && 2.0f * std::abs(over) <= knee) y = x + (1.0f / ratio - 1.0f) * (over + knee / 2.0f) * (over + knee / 2.0f) / (2.0f * knee);
    else                                              y = t + over / ratio;
    return y - x;
}

void Compressor::prepare(double sr) { m_sr = sr; reset(); }
void Compressor::reset() noexcept { m_envDb = 0.0f; }

void Compressor::setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept
{
    m_threshold = lin(p[0], -60.0f, 0.0f);
    m_ratio     = p[1] > 0.99f ? 1e9f : expo(p[1], 1.0f, 20.0f);   // top of the knob = limiter
    const float attackMs = expo(p[2], 0.1f, 100.0f), releaseMs = expo(p[3], 5.0f, 1000.0f);
    m_attackCoef  = std::exp(-1.0f / (attackMs * 0.001f * static_cast<float>(m_sr)));
    m_releaseCoef = std::exp(-1.0f / (releaseMs * 0.001f * static_cast<float>(m_sr)));
    m_makeup = dbToGain(lin(p[4], 0.0f, 36.0f));
    m_knee   = lin(p[5], 0.0f, 12.0f);
}

void Compressor::process(float* l, float* r, int n) noexcept
{
    for (int i = 0; i < n; ++i) {
        const float peak = std::max(std::abs(l[i]), std::abs(r[i]));   // stereo-linked feed-forward detector
        const float target = gainComputer(gainToDb(peak), m_threshold, m_ratio, m_knee);
        const float coef = target < m_envDb ? m_attackCoef : m_releaseCoef;
        m_envDb = target + (m_envDb - target) * coef;
        const float g = (m_envDb == 0.0f ? 1.0f : dbToGain(m_envDb)) * m_makeup;
        l[i] *= g;
        r[i] *= g;
    }
}

// --- Multiband (OTT-style 3-band upward + downward) ------------------------------------------------------

void Multiband::prepare(double sr)
{
    m_sr = sr;
    for (int c = 0; c < 2; ++c) {
        m_xLow[static_cast<std::size_t>(c)].set(88.0, sr);
        m_xHigh[static_cast<std::size_t>(c)].set(2500.0, sr);
        m_lowAllpass[static_cast<std::size_t>(c)].set(2500.0, sr);
    }
    reset();
}

void Multiband::reset() noexcept
{
    for (auto* arr : {&m_xLow, &m_xHigh, &m_lowAllpass})
        for (auto& x : *arr) x.reset();
    m_env = {};
}

void Multiband::setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept
{
    m_depth = p[0];
    const float time = expo(p[1], 0.1f, 10.0f);
    m_attackCoef  = std::exp(-1.0f / (0.003f * time * static_cast<float>(m_sr)));
    m_releaseCoef = std::exp(-1.0f / (0.060f * time * static_cast<float>(m_sr)));
    m_up   = p[2];
    m_down = p[3];
    for (int b = 0; b < 3; ++b) m_bandGain[static_cast<std::size_t>(b)] = dbToGain(bipolarDb(p[static_cast<std::size_t>(4 + b)], 12.0f));
    m_out = dbToGain(bipolarDb(p[7], 12.0f));
}

void Multiband::process(float* l, float* r, int n) noexcept
{
    constexpr float kDownThreshold = -24.0f, kDownRatio = 10.0f;
    constexpr float kUpThreshold = -40.0f, kUpRatio = 3.0f, kUpMax = 24.0f, kFloor = -70.0f;
    for (int i = 0; i < n; ++i) {
        float band[3][2];
        float* ch[2] = {&l[i], &r[i]};
        for (int c = 0; c < 2; ++c) {
            float low, rest, mid, high;
            m_xLow[static_cast<std::size_t>(c)].split(*ch[c], low, rest);
            m_xHigh[static_cast<std::size_t>(c)].split(rest, mid, high);
            band[0][c] = m_lowAllpass[static_cast<std::size_t>(c)].allpass(low);   // phase-align with mid+high
            band[1][c] = mid;
            band[2][c] = high;
        }
        float outL = 0.0f, outR = 0.0f;
        for (int b = 0; b < 3; ++b) {
            const float peak = std::max(std::abs(band[b][0]), std::abs(band[b][1]));
            float& env = m_env[static_cast<std::size_t>(b)];
            env = peak + (env - peak) * (peak > env ? m_attackCoef : m_releaseCoef);
            const float levelDb = gainToDb(env);
            float g = 0.0f;
            if (levelDb > kDownThreshold) g -= (levelDb - kDownThreshold) * (1.0f - 1.0f / kDownRatio) * m_down;
            if (levelDb < kUpThreshold && levelDb > kFloor)
                g += std::min(kUpMax, (kUpThreshold - levelDb) * (1.0f - 1.0f / kUpRatio)) * m_up;
            const float gain = dbToGain(g * m_depth) * m_bandGain[static_cast<std::size_t>(b)];
            outL += band[b][0] * gain;
            outR += band[b][1] * gain;
        }
        l[i] = outL * m_out;
        r[i] = outR * m_out;
    }
}

} // namespace winerose::fx
