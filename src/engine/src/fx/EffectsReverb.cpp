#include "engine/fx/Effects.h"

#include <cmath>

namespace winerose::fx {

using namespace dsp;

namespace {

// Hall: FDN line lengths (ms) at size 0.5 — mutually incommensurate.
constexpr float kHallMs[8] = {29.7f, 37.1f, 41.1f, 43.7f, 47.9f, 53.3f, 59.0f, 67.6f};
constexpr float kHallDiffMs[4] = {4.7f, 3.6f, 12.7f, 9.3f};
constexpr float kHallSign[8] = {1, -1, 1, -1, -1, 1, -1, 1};

// Plate: Dattorro (JAES 1997) delay lengths at his 29761 Hz reference rate.
constexpr double kDattorroRate = 29761.0;
constexpr int kPlateIn[4] = {142, 107, 379, 277};
constexpr float kPlateInGain[4] = {0.75f, 0.75f, 0.625f, 0.625f};
constexpr int kAp1[2] = {672, 908}, kD1[2] = {4453, 4217}, kAp2[2] = {1800, 2656}, kD2[2] = {3720, 3163};
constexpr int kExcursion = 16;

// In-place 8-point fast Walsh-Hadamard transform, normalized (orthogonal: energy preserving).
void hadamard8(float* v) noexcept
{
    for (int len = 1; len < 8; len <<= 1)
        for (int i = 0; i < 8; i += len << 1)
            for (int j = i; j < i + len; ++j) {
                const float a = v[j], b = v[j + len];
                v[j] = a + b;
                v[j + len] = a - b;
            }
    constexpr float norm = 0.35355339059327373f;   // 1/sqrt(8)
    for (int i = 0; i < 8; ++i) v[i] *= norm;
}

float schroederAllpass(DelayLine& line, int length, float g, float x) noexcept
{
    const float d = line.readInt(length);
    const float y = -g * x + d;
    line.write(x + g * y);
    return y;
}

} // namespace

void Reverb::prepare(double sr)
{
    m_sr = sr;
    m_preL.allocate(static_cast<int>(0.26 * sr));
    m_preR.allocate(static_cast<int>(0.26 * sr));
    for (int i = 0; i < 8; ++i) m_lines[static_cast<std::size_t>(i)].allocate(static_cast<int>(kHallMs[i] * 0.001 * 2.2 * sr));
    for (int i = 0; i < 4; ++i) m_diffuser[static_cast<std::size_t>(i)].allocate(static_cast<int>(kHallDiffMs[i] * 0.001 * 2.2 * sr));
    const double scale = sr / kDattorroRate * 1.6;   // room for size up to 1.5
    for (int i = 0; i < 4; ++i) m_plateIn[static_cast<std::size_t>(i)].allocate(static_cast<int>(kPlateIn[i] * scale));
    for (int b = 0; b < 2; ++b) {
        m_tankAp1[static_cast<std::size_t>(b)].allocate(static_cast<int>((kAp1[b] + kExcursion) * scale));
        m_tankDelay1[static_cast<std::size_t>(b)].allocate(static_cast<int>(kD1[b] * scale));
        m_tankAp2[static_cast<std::size_t>(b)].allocate(static_cast<int>(kAp2[b] * scale));
        m_tankDelay2[static_cast<std::size_t>(b)].allocate(static_cast<int>(kD2[b] * scale));
    }
    reset();
}

void Reverb::reset() noexcept
{
    m_preL.reset(); m_preR.reset();
    for (auto& d : m_lines) d.reset();
    for (auto& d : m_diffuser) d.reset();
    for (auto& d : m_lineDamp) d.reset();
    for (auto& d : m_plateIn) d.reset();
    for (auto* arr : {&m_tankAp1, &m_tankDelay1, &m_tankAp2, &m_tankDelay2})
        for (auto& d : *arr) d.reset();
    for (auto& d : m_tankDamp) d.reset();
    m_tankOut = {};
    m_apInterp = {};
    for (auto* b : {&m_lowCutL, &m_lowCutR, &m_highCutL, &m_highCutR}) b->reset();
    m_lfo = 0;
}

void Reverb::setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept
{
    m_mode = static_cast<Mode>(choice(p[0], static_cast<int>(Mode::Count)));
    m_rt60 = expo(p[1], 0.1f, 20.0f);
    m_size = p[2];
    m_preDelay = std::max(1.0f, static_cast<float>(lin(p[3], 0.0f, 0.25f) * m_sr));
    m_lowCutL.set(Biquad::Kind::Highpass, expo(p[4], 20.0f, 1000.0f), 0.707, 0.0, m_sr);
    m_lowCutR = m_lowCutL;
    m_highCutL.set(Biquad::Kind::Lowpass, expo(p[5], 1000.0f, 20000.0f), 0.707, 0.0, m_sr);
    m_highCutR = m_highCutL;
    m_damp = p[6];
    m_width = p[7];

    // Hall: per-line loop gain for an exact broadband RT60: g = 10^(-3·L / (RT60·fs)).
    const float sizeScale = 0.4f + 1.2f * m_size;
    for (int i = 0; i < 8; ++i) {
        const int len = std::max(8, static_cast<int>(kHallMs[i] * 0.001f * sizeScale * static_cast<float>(m_sr)));
        m_lineLen[static_cast<std::size_t>(i)] = len;
        m_lineGain[static_cast<std::size_t>(i)] = static_cast<float>(std::pow(10.0, -3.0 * len / (m_rt60 * m_sr)));
        m_lineDamp[static_cast<std::size_t>(i)].setCutoff(20000.0 * std::exp2(-m_damp * 6.0), m_sr);
    }
    for (int i = 0; i < 4; ++i)
        m_diffLen[static_cast<std::size_t>(i)] = std::max(4, static_cast<int>(kHallDiffMs[i] * 0.001f * sizeScale * static_cast<float>(m_sr)));

    // Plate: tank lengths scaled to this rate and size. Each half has two decay multipliers (after the
    // damping, and on the cross-feed into the other half), so one trip around the tank passes four:
    // decay = 10^(-3·loop / (4·RT60·fs)).
    const double scale = m_sr / kDattorroRate * (0.5 + m_size);
    double loop = 0.0;
    for (int b = 0; b < 2; ++b) {
        m_ap1Len[static_cast<std::size_t>(b)] = static_cast<int>(kAp1[b] * scale);
        m_d1Len[static_cast<std::size_t>(b)]  = static_cast<int>(kD1[b] * scale);
        m_ap2Len[static_cast<std::size_t>(b)] = static_cast<int>(kAp2[b] * scale);
        m_d2Len[static_cast<std::size_t>(b)]  = static_cast<int>(kD2[b] * scale);
        loop += m_ap1Len[static_cast<std::size_t>(b)] + m_d1Len[static_cast<std::size_t>(b)]
              + m_ap2Len[static_cast<std::size_t>(b)] + m_d2Len[static_cast<std::size_t>(b)];
        m_tankDamp[static_cast<std::size_t>(b)].setCutoff(20000.0 * std::exp2(-m_damp * 6.0), m_sr);
    }
    for (int i = 0; i < 4; ++i)
        m_plateInLen[static_cast<std::size_t>(i)] = std::max(4, static_cast<int>(kPlateIn[i] * m_sr / kDattorroRate));

    // A Schroeder allpass of length L and coefficient g rings for its own RT60 of 3·L / (-log10 g) samples,
    // independent of the tank decay — at Dattorro's fixed coefficients that floor (~0.4 s at mid size) makes
    // short decays impossible. Cap every diffusion coefficient so its ringing stays under half the target.
    auto diffusion = [&](float nominal, int length) {
        const double cap = std::pow(10.0, -3.0 * length / (0.5 * m_rt60 * m_sr));
        return static_cast<float>(std::min(static_cast<double>(nominal), cap));
    };
    for (int i = 0; i < 4; ++i)
        m_plateInGain[static_cast<std::size_t>(i)] = diffusion(kPlateInGain[i], m_plateInLen[static_cast<std::size_t>(i)]);
    for (int b = 0; b < 2; ++b) {
        m_ap1Gain[static_cast<std::size_t>(b)] = diffusion(0.7f, m_ap1Len[static_cast<std::size_t>(b)]);
        m_ap2Gain[static_cast<std::size_t>(b)] = diffusion(0.5f, m_ap2Len[static_cast<std::size_t>(b)]);
    }
    m_plateDecay = static_cast<float>(std::min(0.9999, std::pow(10.0, -3.0 * loop / (4.0 * m_rt60 * m_sr))));
}

void Reverb::processHall(float inL, float inR, float& outL, float& outR) noexcept
{
    float x = 0.5f * (inL + inR);
    for (int i = 0; i < 4; ++i) x = schroederAllpass(m_diffuser[static_cast<std::size_t>(i)], m_diffLen[static_cast<std::size_t>(i)], 0.6f, x);

    float out[8], v[8];
    const bool damp = m_damp > 0.001f;
    for (int i = 0; i < 8; ++i) {
        out[i] = m_lines[static_cast<std::size_t>(i)].readInt(m_lineLen[static_cast<std::size_t>(i)]);
        const float y = damp ? m_lineDamp[static_cast<std::size_t>(i)].lowpass(out[i]) : out[i];
        v[i] = y * m_lineGain[static_cast<std::size_t>(i)];
    }
    hadamard8(v);
    for (int i = 0; i < 8; ++i) m_lines[static_cast<std::size_t>(i)].write(v[i] + kHallSign[i] * 0.35f * x);

    outL = 0.5f * (out[0] - out[2] + out[4] - out[6]);
    outR = 0.5f * (out[1] - out[3] + out[5] - out[7]);
}

void Reverb::processPlate(float inL, float inR, float& outL, float& outR) noexcept
{
    float x = 0.5f * (inL + inR);
    for (int i = 0; i < 4; ++i)
        x = schroederAllpass(m_plateIn[static_cast<std::size_t>(i)], m_plateInLen[static_cast<std::size_t>(i)], m_plateInGain[static_cast<std::size_t>(i)], x);

    const bool damp = m_damp > 0.001f;
    const float mod = static_cast<float>(std::sin(2.0 * kPi * m_lfo)) * kExcursion * static_cast<float>(m_sr / kDattorroRate);
    m_lfo += 1.0 / m_sr;
    m_lfo -= std::floor(m_lfo);

    float halfOut[2];
    for (int b = 0; b < 2; ++b) {
        const std::size_t k = static_cast<std::size_t>(b);
        // Each half takes the input plus the OTHER half's output (the figure-eight tank).
        float s = x + m_plateDecay * m_tankOut[1 - k];
        // Modulated decay-diffusion allpass (g = -0.7). The moving tap uses first-order allpass
        // interpolation: unity gain at every frequency, so modulation adds no loss per trip around the
        // tank (a polynomial interpolator would, shortening long decays).
        {
            auto& line = m_tankAp1[k];
            const float delay = static_cast<float>(m_ap1Len[k]) + (b == 0 ? mod : -mod);
            const int   whole = static_cast<int>(delay);
            const float frac  = delay - static_cast<float>(whole);
            const float a = (1.0f - frac) / (1.0f + frac);
            const float d = a * line.readInt(whole) + line.readInt(whole + 1) - a * m_apInterp[k];
            m_apInterp[k] = d;
            const float g1 = m_ap1Gain[k];
            const float y = g1 * s + d;
            line.write(s - g1 * y);
            s = y;
        }
        m_tankDelay1[k].write(s);
        s = m_tankDelay1[k].readInt(m_d1Len[k]);
        if (damp) s = m_tankDamp[k].lowpass(s);
        s *= m_plateDecay;
        s = schroederAllpass(m_tankAp2[k], m_ap2Len[k], m_ap2Gain[k], s);
        m_tankDelay2[k].write(s);
        halfOut[b] = m_tankDelay2[k].readInt(m_d2Len[k]);
    }
    m_tankOut[0] = halfOut[0];
    m_tankOut[1] = halfOut[1];

    // Output taps (Dattorro's table, scaled), left from both halves with alternating signs.
    const double sc = m_sr / kDattorroRate * (0.5 + m_size);
    auto tap = [&](DelayLine& d, int at) { return d.readInt(std::max(1, static_cast<int>(at * sc))); };
    outL = 0.6f * (tap(m_tankDelay1[1], 266) + tap(m_tankDelay1[1], 2974) - tap(m_tankAp2[1], 1913) + tap(m_tankDelay2[1], 1996)
                 - tap(m_tankDelay1[0], 1990) - tap(m_tankAp2[0], 187) - tap(m_tankDelay2[0], 1066));
    outR = 0.6f * (tap(m_tankDelay1[0], 353) + tap(m_tankDelay1[0], 3627) - tap(m_tankAp2[0], 1228) + tap(m_tankDelay2[0], 2673)
                 - tap(m_tankDelay1[1], 2111) - tap(m_tankAp2[1], 335) - tap(m_tankDelay2[1], 121));
}

void Reverb::process(float* l, float* r, int n) noexcept
{
    for (int i = 0; i < n; ++i) {
        m_preL.write(l[i]);
        m_preR.write(r[i]);
        float inL = m_highCutL.process(m_lowCutL.process(m_preL.readInt(static_cast<int>(m_preDelay))));
        float inR = m_highCutR.process(m_lowCutR.process(m_preR.readInt(static_cast<int>(m_preDelay))));
        float outL, outR;
        if (m_mode == Mode::Plate) processPlate(inL, inR, outL, outR);
        else                       processHall(inL, inR, outL, outR);
        const float mid = 0.5f * (outL + outR), side = 0.5f * (outL - outR) * m_width;
        l[i] = mid + side;
        r[i] = mid - side;
    }
}

} // namespace winerose::fx
