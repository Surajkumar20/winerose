#include "engine/fx/Effects.h"

#include "engine/modulation/Lfo.h"

#include "hiir/PolyphaseIir2Designer.h"

#include <cmath>

namespace winerose::fx {

using namespace dsp;

namespace {
constexpr double kTwoPi = 2.0 * kPi;
float sine01(double phase) noexcept { return 0.5f + 0.5f * static_cast<float>(std::sin(kTwoPi * phase)); }
void advance(double& phase, double inc) noexcept { phase += inc; phase -= std::floor(phase); }
}

// --- Flanger ---------------------------------------------------------------------------------------------

void Flanger::prepare(double sr)
{
    m_sr = sr;
    for (auto& d : m_delay) d.allocate(static_cast<int>(0.02 * sr));
    reset();
}

void Flanger::reset() noexcept { for (auto& d : m_delay) d.reset(); m_last = {}; m_phase = 0; }

void Flanger::setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept
{
    m_rate = expo(p[0], 0.01f, 20.0f);
    m_depth = p[1];
    m_feedback = lin(p[2], -0.95f, 0.95f);
    m_stereo = 0.5f * p[3];
}

void Flanger::process(float* l, float* r, int n) noexcept
{
    float* ch[2] = {l, r};
    const double inc = m_rate / m_sr;
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 2; ++c) {
            const float lfo = sine01(m_phase + c * m_stereo);
            const float delay = static_cast<float>((0.0001 + m_depth * 0.005 * lfo) * m_sr);
            auto& line = m_delay[static_cast<std::size_t>(c)];
            const float x = ch[c][i];
            const float d = line.read(delay);
            line.write(x + m_feedback * d);
            ch[c][i] = 0.5f * (x + d);   // the comb comes from dry + delayed; the slot mix blends with dry
        }
        advance(m_phase, inc);
    }
}

// --- Phaser ----------------------------------------------------------------------------------------------

void Phaser::prepare(double sr) { m_sr = sr; reset(); }
void Phaser::reset() noexcept { for (auto& s : m_state) s.fill(0.0f); m_last = {}; m_phase = 0; }

void Phaser::setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept
{
    m_rate = expo(p[0], 0.01f, 10.0f);
    m_depth = p[1];
    m_center = expo(p[2], 100.0f, 8000.0f);
    m_feedback = lin(p[3], -0.95f, 0.95f);
    m_stereo = 0.5f * p[4];
    m_stages = 2 * (1 + choice(p[5], 6));   // 2..12
}

void Phaser::process(float* l, float* r, int n) noexcept
{
    float* ch[2] = {l, r};
    const double inc = m_rate / m_sr;
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 2; ++c) {
            const float lfo = sine01(m_phase + c * m_stereo);
            const double f = m_center * std::exp2((lfo - 0.5) * 4.0 * m_depth);   // ±2 octaves at full depth
            const double g = std::tan(kPi * std::clamp(f, 10.0, 0.45 * m_sr) / m_sr);
            const float G = static_cast<float>(g / (1.0 + g));
            auto& st = m_state[static_cast<std::size_t>(c)];
            const float x = ch[c][i];
            float u = x + m_feedback * m_last[static_cast<std::size_t>(c)];
            for (int s = 0; s < m_stages; ++s) {
                const float v = (u - st[static_cast<std::size_t>(s)]) * G;
                const float lp = v + st[static_cast<std::size_t>(s)];
                st[static_cast<std::size_t>(s)] = lp + v;
                u = 2.0f * lp - u;   // first-order allpass
            }
            m_last[static_cast<std::size_t>(c)] = u;
            ch[c][i] = 0.5f * (x + u);
        }
        advance(m_phase, inc);
    }
}

// --- Chorus (4 voices: 2 per side) -----------------------------------------------------------------------

void Chorus::prepare(double sr)
{
    m_sr = sr;
    for (auto& d : m_delay) d.allocate(static_cast<int>(0.06 * sr));
    reset();
}

void Chorus::reset() noexcept { for (auto& d : m_delay) d.reset(); for (auto& f : m_lpf) f.reset(); m_last = {}; m_phase = 0; }

void Chorus::setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept
{
    m_rate = expo(p[0], 0.05f, 5.0f);
    m_depth = p[1];
    m_delay1 = lin(p[2], 1.0f, 30.0f);
    m_delay2 = lin(p[3], 1.0f, 30.0f);
    m_feedback = lin(p[4], 0.0f, 0.9f);
    for (auto& f : m_lpf) f.setCutoff(expo(p[5], 200.0f, 20000.0f), m_sr);
}

void Chorus::process(float* l, float* r, int n) noexcept
{
    float* ch[2] = {l, r};
    const double inc = m_rate / m_sr;
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 2; ++c) {
            auto& line = m_delay[static_cast<std::size_t>(c)];
            // Two voices per side, LFOs in quadrature, sides offset by half a cycle.
            const double base = c * 0.5;
            const float d1 = static_cast<float>(m_delay1 * 0.001 * m_sr * (1.0 + 0.5 * m_depth * (sine01(m_phase + base) - 0.5)));
            const float d2 = static_cast<float>(m_delay2 * 0.001 * m_sr * (1.0 + 0.5 * m_depth * (sine01(m_phase + base + 0.25) - 0.5)));
            const float wet = 0.5f * (line.read(d1) + line.read(d2));
            line.write(ch[c][i] + m_feedback * m_last[static_cast<std::size_t>(c)]);
            m_last[static_cast<std::size_t>(c)] = wet;
            ch[c][i] = m_lpf[static_cast<std::size_t>(c)].lowpass(wet);
        }
        advance(m_phase, inc);
    }
}

// --- Delay -----------------------------------------------------------------------------------------------

void Delay::prepare(double sr)
{
    m_sr = sr;
    for (auto& d : m_delay) d.allocate(static_cast<int>(kMaxSeconds * sr) + 4);
    reset();
}

void Delay::reset() noexcept
{
    for (auto& d : m_delay) d.reset();
    for (auto& b : m_bp) b.reset();
    m_snapTime = true;
}

void Delay::setParams(const std::array<float, kParamCount>& p, const FxContext& ctx) noexcept
{
    m_mode = static_cast<Mode>(choice(p[0], static_cast<int>(Mode::Count)));
    double seconds;
    if (p[1] >= 0.5f) {
        const int d = choice(p[2], modulation::kSyncDivisionCount);
        seconds = modulation::kSyncDivisions[d].beats * 60.0 / std::max(ctx.bpm, 1.0);
    } else {
        seconds = expo(p[2], 1.0f, 2000.0f) * 0.001;
    }
    const double maxS = kMaxSeconds - 0.01;
    m_timeL = static_cast<float>(std::clamp(seconds, 0.0001, maxS) * m_sr);
    m_timeR = static_cast<float>(std::clamp(seconds * expo(p[3], 0.25f, 4.0f), 0.0001, maxS) * m_sr);
    if (m_snapTime) {
        m_curL = m_timeL;
        m_curR = m_timeR;
        m_snapTime = false;
    }
    m_feedback = lin(p[4], 0.0f, 0.98f);
    m_filterOn = p[6] > 0.01f;
    if (m_filterOn)
        for (auto& b : m_bp) b.set(Biquad::Kind::Bandpass, expo(p[5], 100.0f, 10000.0f), expo(p[6], 0.3f, 4.0f), 0.0, m_sr);
}

void Delay::process(float* l, float* r, int n) noexcept
{
    constexpr float kGlide = 0.0015f;   // ~15 ms time smoothing: changing the time glides like tape
    for (int i = 0; i < n; ++i) {
        m_curL += (m_timeL - m_curL) * kGlide;
        m_curR += (m_timeR - m_curR) * kGlide;
        auto filt = [&](int c, float x) { return m_filterOn ? m_bp[static_cast<std::size_t>(c)].process(x) : x; };
        const float inL = l[i], inR = r[i];
        float outL, outR;
        switch (m_mode) {
            case Mode::PingPong: {
                outL = m_delay[0].read(m_curL);
                outR = m_delay[1].read(m_curL);   // same time on both sides: the echo alternates L, R, L, ...
                m_delay[0].write(0.5f * (inL + inR) + m_feedback * filt(1, outR));
                m_delay[1].write(m_feedback * filt(0, outL));
                break;
            }
            case Mode::Tap: {
                outL = m_delay[0].read(m_curL);
                outR = m_delay[0].read(m_curL * 0.75f);
                m_delay[0].write(0.5f * (inL + inR) + m_feedback * filt(0, outL));
                break;
            }
            case Mode::Normal:
            case Mode::Count:
            default: {
                outL = m_delay[0].read(m_curL);
                outR = m_delay[1].read(m_curR);
                m_delay[0].write(inL + m_feedback * filt(0, outL));
                m_delay[1].write(inR + m_feedback * filt(1, outR));
                break;
            }
        }
        l[i] = outL;
        r[i] = outR;
    }
}

// --- Hyper / Dimension -----------------------------------------------------------------------------------

void Hyper::prepare(double sr)
{
    m_sr = sr;
    for (auto& d : m_delay) d.allocate(static_cast<int>(0.08 * sr));
    reset();
}

void Hyper::reset() noexcept { for (auto& d : m_delay) d.reset(); m_phase = m_dimPhase = 0; }

void Hyper::setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept
{
    m_rate = expo(p[0], 0.1f, 5.0f);
    m_detune = p[1];
    m_voices = 1 + choice(p[2], 7);
    m_hyperMix = p[3];
    m_dimSize = lin(p[5], 0.2f, 1.0f);
    m_dimMix = p[6];
}

void Hyper::process(float* l, float* r, int n) noexcept
{
    static constexpr float kDimMs[4] = {7.0f, 11.0f, 13.0f, 17.0f};
    const double inc = m_rate / m_sr, dimInc = 0.2 / m_sr;
    for (int i = 0; i < n; ++i) {
        const float inL = l[i], inR = r[i];
        m_delay[0].write(inL);
        m_delay[1].write(inR);
        // Hyper: N detuned micro-delays alternating sides.
        float hyper[2] = {0.0f, 0.0f};
        for (int v = 0; v < m_voices; ++v) {
            const int c = v % 2;
            const double ph = m_phase * (1.0 + 0.13 * v) + 0.37 * v;
            const float ms = (8.0f + 2.5f * static_cast<float>(v)) * (1.0f + m_detune * 0.3f * static_cast<float>(std::sin(kTwoPi * ph)));
            hyper[c] += m_delay[static_cast<std::size_t>(c)].read(ms * 0.001f * static_cast<float>(m_sr));
        }
        const float norm = 1.0f / std::sqrt(static_cast<float>(std::max(1, (m_voices + 1) / 2)));
        // Dimension: four lines summed out of phase between sides, slow amplitude modulation.
        const float am = 0.85f + 0.15f * static_cast<float>(std::sin(kTwoPi * m_dimPhase));
        float dim[4];
        for (int k = 0; k < 4; ++k)
            dim[k] = m_delay[static_cast<std::size_t>(k % 2)].read(kDimMs[k] * m_dimSize * 0.001f * static_cast<float>(m_sr));
        const float dimL = 0.5f * (dim[0] - dim[2]) * am, dimR = 0.5f * (dim[1] - dim[3]) * am;

        l[i] = inL * (1.0f - m_hyperMix) + hyper[0] * norm * m_hyperMix + dimL * m_dimMix;
        r[i] = inR * (1.0f - m_hyperMix) + hyper[1] * norm * m_hyperMix + dimR * m_dimMix;
        advance(m_phase, inc);
        advance(m_dimPhase, dimInc);
    }
}

// --- Bode frequency shifter ------------------------------------------------------------------------------

void Bode::prepare(double sr)
{
    m_sr = sr;
    double coefs[8];
    // Hilbert pair: 90° apart from ~0.2% of fs (≈ 100 Hz at 48 kHz) to just below Nyquist.
    hiir::PolyphaseIir2Designer::compute_coefs_spec_order_tbw(coefs, 8, 0.002);
    for (auto& h : m_hilbert) h.set_coefs(coefs);
    reset();
}

void Bode::reset() noexcept { for (auto& h : m_hilbert) h.clear_buffers(); m_last = {}; m_phase = 0; }

void Bode::setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept
{
    const float s = 2.0f * p[0] - 1.0f;   // p0 = 0.5: no shift
    m_shiftHz = (s < 0.0f ? -1.0f : 1.0f) * 5000.0f * std::abs(s) * std::abs(s) * std::abs(s);
    m_side = p[1];                        // 0 = upper sideband (shift as set), 1 = lower (mirror)
    m_feedback = lin(p[2], 0.0f, 0.9f);
}

void Bode::process(float* l, float* r, int n) noexcept
{
    float* ch[2] = {l, r};
    const double inc = m_shiftHz / m_sr;
    for (int i = 0; i < n; ++i) {
        const float c = static_cast<float>(std::cos(kTwoPi * m_phase));
        const float s = static_cast<float>(std::sin(kTwoPi * m_phase));
        for (int k = 0; k < 2; ++k) {
            float a, b;
            m_hilbert[static_cast<std::size_t>(k)].process_sample(a, b, ch[k][i] + m_feedback * m_last[static_cast<std::size_t>(k)]);
            // Single-sideband modulation: a·cos ∓ b·sin selects the upper / lower sideband.
            const float up = a * c - b * s, down = a * c + b * s;
            const float y = up + (down - up) * m_side;
            m_last[static_cast<std::size_t>(k)] = y;
            ch[k][i] = y;
        }
        m_phase += inc;
        m_phase -= std::floor(m_phase);
    }
}

} // namespace winerose::fx
