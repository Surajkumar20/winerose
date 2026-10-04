#include "engine/dsp/filters/FilterUnit.h"

#include "hiir/PolyphaseIir2Designer.h"

#include <algorithm>
#include <cmath>

namespace winerose::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

// 2× oversampling for nonlinear families: 8 coefficients, transition band 0.1 (≈ 90 dB stopband).
constexpr int kOsCoefs = 8;
double g_osCoefs[kOsCoefs] {};
bool   g_osCoefsReady = false;

void initOsCoefs() noexcept
{
    if (g_osCoefsReady) return;
    hiir::PolyphaseIir2Designer::compute_coefs_spec_order_tbw(g_osCoefs, kOsCoefs, 0.1);
    g_osCoefsReady = true;
}

// Vowel formants F1-F3 in Hz (Peterson & Barney 1952 averages), A E I O U.
constexpr float kMaleFormants[5][3]   = {{730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240}};
constexpr float kFemaleFormants[5][3] = {{850, 1220, 2810}, {610, 2330, 2990}, {310, 2790, 3310}, {590, 920, 2710}, {370, 950, 2670}};
constexpr float kFormantGain[3] = {1.0f, 0.7f, 0.5f};

// FDN / diffuser base lengths at 48 kHz for a 1 kHz cutoff (mutually prime).
constexpr int kFdnLines[4]      = {1031, 1327, 1523, 1801};
constexpr int kDiffuserLines[4] = {142, 107, 379, 277};
constexpr int kSegment = FilterUnit::kDelaySize / 4;

double onePoleG(double fc, double sr) noexcept
{
    const double g = std::tan(kPi * std::clamp(fc, 1.0, 0.49 * sr) / sr);
    return g / (1.0 + g);
}

// TPT SVF coefficients from an explicit damping k (Svf::compute maps resonance → k for the common case).
Svf::Coefs svfFromDamping(double fc, double k, double sr) noexcept
{
    const double g = std::tan(kPi * std::clamp(fc, 1.0, 0.49 * sr) / sr);
    Svf::Coefs c;
    c.k  = static_cast<float>(k);
    c.a1 = static_cast<float>(1.0 / (1.0 + g * (g + k)));
    c.a2 = static_cast<float>(g) * c.a1;
    c.a3 = static_cast<float>(g) * c.a2;
    return c;
}

float lerp(float a, float b, float t) noexcept { return a + (b - a) * t; }

float threeWay(float a, float b, float c, float m) noexcept
{
    return m < 0.5f ? lerp(a, b, 2.0f * m) : lerp(b, c, 2.0f * m - 1.0f);
}

float tpLowpass(float& s, float x, float G) noexcept   // TPT one-pole, returns LP
{
    const float v = (x - s) * G;
    const float y = v + s;
    s = y + v;
    return y;
}

} // namespace

FilterUnit::FilterUnit()
{
    initOsCoefs();
    for (auto& ch : m_ch) {
        ch.buffer.assign(kDelaySize, 0.0f);
        ch.up.set_coefs(g_osCoefs);
        ch.down.set_coefs(g_osCoefs);
    }
}

void FilterUnit::prepare(double sampleRate) noexcept
{
    m_sampleRate = sampleRate;
    reset();
    updateCoefs();
}

void FilterUnit::reset() noexcept
{
    for (auto& ch : m_ch) {
        ch.svf.reset();
        ch.svf2.reset();
        ch.onePole = ch.onePole2 = 0.0f;
        ch.ladder.fill(0.0f);
        ch.lastOut = 0.0f;
        ch.ap.fill(0.0f);
        for (auto& s : ch.disperser) s.reset();
        for (auto& s : ch.formant) s.reset();
        ch.hold = 0.0f;
        ch.holdPhase = 1.0;
        ch.ringPhase = 0.0;
        std::fill(ch.buffer.begin(), ch.buffer.end(), 0.0f);
        ch.writePos = 0;
        ch.segPos.fill(0);
        ch.combDamp = 0.0f;
        ch.fdnDamp.fill(0.0f);
        ch.up.clear_buffers();
        ch.down.clear_buffers();
    }
}

void FilterUnit::set(const FilterSettings& settings) noexcept
{
    const bool typeChanged = settings.type != m_settings.type;
    m_settings = settings;
    m_info = &filterInfo(settings.type);
    if (typeChanged) reset();   // states of one family are meaningless (or explosive) in another
    if (settings.drive > 0.0f) {
        m_preGain  = std::pow(10.0f, settings.drive * 24.0f / 20.0f);
        m_postGain = 1.0f / std::sqrt(m_preGain);
    } else {
        m_preGain = m_postGain = 1.0f;
    }
    updateCoefs();
}

void FilterUnit::setCutoff(float cutoffHz) noexcept
{
    m_settings.cutoffHz = cutoffHz;
    updateCoefs();
}

void FilterUnit::updateCoefs() noexcept
{
    const double sr = m_info->nonlinear ? 2.0 * m_sampleRate : m_sampleRate;
    const double spread = std::exp2(0.5 * std::clamp(m_settings.stereo, 0.0f, 1.0f));
    m_coefs[0] = computeCoefs(m_settings, m_settings.cutoffHz / spread, sr);
    m_coefs[1] = computeCoefs(m_settings, m_settings.cutoffHz * spread, sr);
}

FilterCoefs FilterUnit::computeCoefs(const FilterSettings& s, double cutoffHz, double sr) noexcept
{
    const FilterInfo& info = filterInfo(s.type);
    const double fc  = std::clamp(cutoffHz, 8.0, 0.49 * sr);
    const double res = std::clamp(static_cast<double>(s.resonance), 0.0, 1.0);
    const double var = std::clamp(static_cast<double>(s.var), 0.0, 1.0);
    FilterCoefs c;

    switch (info.family) {
        case FilterFamily::OnePole:
            c.G = static_cast<float>(onePoleG(fc, sr));
            break;
        case FilterFamily::Svf:
        case FilterFamily::SvfMorph:
        case FilterFamily::PzMorph:
            c.svf = Svf::compute(fc, res, sr);
            c.svfFlat = Svf::compute(fc, 0.0, sr);
            c.G = static_cast<float>(onePoleG(fc, sr));
            break;
        case FilterFamily::SvfDual:
            c.svf  = Svf::compute(fc, res, sr);
            c.svf2 = Svf::compute(fc * std::exp2((var - 0.5) * 8.0), res, sr);
            break;
        case FilterFamily::Ladder:
        case FilterFamily::DrivenLadder:
        case FilterFamily::Acid:
            c.G = static_cast<float>(onePoleG(fc, sr));
            // Linear ladder stays just below self-oscillation (k = 4). The driven kinds go past it and are
            // amplitude-limited by their saturator; Acid's asymmetric curve has a small-signal slope of
            // sech²(0.3) ≈ 0.92, so it needs a larger k to reach the same effective loop gain.
            c.k = static_cast<float>(res * (info.family == FilterFamily::Ladder ? 3.98
                                          : info.family == FilterFamily::DrivenLadder ? 4.3 : 4.7));
            c.comp = 1.0f + 0.5f * c.k;
            break;
        case FilterFamily::SallenKey:
            c.svf = svfFromDamping(fc, 2.0 - 2.1 * res, sr);   // slightly negative damping at max: the
            break;                                             // saturated integrator then self-oscillates
        case FilterFamily::Comb:
        case FilterFamily::Flange: {
            const double minFc = sr / (kDelaySize - 4);
            c.delay = static_cast<float>(sr / std::max(fc, minFc));
            const bool negative = (info.variant % 2) == 1;
            c.feedback = static_cast<float>((negative ? -1.0 : 1.0) * res * (info.family == FilterFamily::Comb ? 0.98 : 0.9));
            c.damp = info.variant >= 2 ? static_cast<float>(onePoleG(20000.0 * std::exp2(-var * 7.0), sr)) : 1.0f;
            break;
        }
        case FilterFamily::Phaser:
            c.stages = info.poles;
            for (int i = 0; i < c.stages; ++i) {
                const double t = c.stages > 1 ? static_cast<double>(i) / (c.stages - 1) - 0.5 : 0.0;
                c.apG[static_cast<std::size_t>(i)] = static_cast<float>(onePoleG(fc * std::exp2(t * var * 4.0), sr));
            }
            c.feedback = static_cast<float>((info.variant == 1 ? -1.0 : 1.0) * res * 0.9);
            break;
        case FilterFamily::Formant: {
            const auto& table = info.variant == 0 ? kMaleFormants : kFemaleFormants;
            const double pos = var * 4.0;
            const int v0 = std::min(static_cast<int>(pos), 3);
            const double t = pos - v0;
            const double shift = std::clamp(fc / 1000.0, 0.25, 4.0);
            const double k = 0.35 - 0.3 * res;   // narrow band-passes: Q ≈ 3 .. 20
            for (int f = 0; f < 3; ++f) {
                const double hz = (table[v0][f] + (table[v0 + 1][f] - table[v0][f]) * t) * shift;
                c.formant[static_cast<std::size_t>(f)] = svfFromDamping(hz, k, sr);
                c.formantGain[static_cast<std::size_t>(f)] = kFormantGain[f];
            }
            break;
        }
        case FilterFamily::SampleHold:
        case FilterFamily::RingMod:
            c.rate = fc / sr;
            break;
        case FilterFamily::TwinLp:
            c.svf  = Svf::compute(fc, res * 0.7, sr);
            c.svf2 = Svf::compute(fc * std::exp2(var * 2.0 - 1.0), res * 0.7, sr);
            break;
        case FilterFamily::Reverb:
            for (int i = 0; i < 4; ++i)
                c.lines[static_cast<std::size_t>(i)] = std::clamp(static_cast<int>(kFdnLines[i] * (1000.0 / fc) * (sr / 48000.0)), 8, kSegment - 1);
            c.feedback = static_cast<float>(0.3 + 0.68 * res);
            c.damp = static_cast<float>(onePoleG(20000.0 * std::exp2(-var * 6.0), sr));
            break;
        case FilterFamily::Disperser:
            c.svf = Svf::compute(fc, res * 0.5, sr);
            c.stages = 4 + static_cast<int>(std::lround(var * 28.0));
            break;
        case FilterFamily::Diffuser:
            for (int i = 0; i < 4; ++i)
                c.lines[static_cast<std::size_t>(i)] = std::clamp(static_cast<int>(kDiffuserLines[i] * (1000.0 / fc) * (sr / 48000.0)), 4, kSegment - 1);
            c.feedback = static_cast<float>(0.5 + 0.4 * res);
            break;
    }
    return c;
}

float FilterUnit::readDelay(const Channel& ch, float delay) const noexcept
{
    constexpr int mask = kDelaySize - 1;
    const float pos = static_cast<float>(ch.writePos) - delay;
    const float fl  = std::floor(pos);
    const float t   = pos - fl;
    const int   i   = static_cast<int>(fl);
    const float xm1 = ch.buffer[static_cast<std::size_t>((i - 1) & mask)];
    const float x0  = ch.buffer[static_cast<std::size_t>(i & mask)];
    const float x1  = ch.buffer[static_cast<std::size_t>((i + 1) & mask)];
    const float x2  = ch.buffer[static_cast<std::size_t>((i + 2) & mask)];
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

float FilterUnit::processCore(Channel& ch, const FilterCoefs& c, float x) noexcept
{
    const FilterInfo& info = *m_info;
    constexpr int mask = kDelaySize - 1;

    auto svfSelect = [](const Svf::Outputs& o, const Svf::Coefs& k, FilterMode mode, float in) {
        switch (mode) {
            case FilterMode::Lp:      return o.low;
            case FilterMode::Hp:      return o.high;
            case FilterMode::Bp:      return k.k * o.band;            // unity gain at the centre
            case FilterMode::Notch:   return o.low + o.high;
            case FilterMode::Peak:    return o.low - o.high;
            case FilterMode::Allpass: return in - 2.0f * k.k * o.band;
            case FilterMode::None:    break;
        }
        return o.low;
    };

    switch (info.family) {
        case FilterFamily::OnePole: {
            const float lp = tpLowpass(ch.onePole, x, c.G);
            return info.mode == FilterMode::Hp ? x - lp : lp;
        }
        case FilterFamily::Svf: {
            const float y1 = svfSelect(ch.svf.process(x, c.svf), c.svf, info.mode, x);
            if (info.poles == 4) return svfSelect(ch.svf2.process(y1, c.svfFlat), c.svfFlat, info.mode, y1);
            if (info.poles == 3) {
                const float lp = tpLowpass(ch.onePole, y1, c.G);
                return info.mode == FilterMode::Hp ? y1 - lp : lp;
            }
            return y1;
        }
        case FilterFamily::SvfMorph: {
            const auto o = ch.svf.process(x, c.svf);
            const float bp = c.svf.k * o.band, notch = o.low + o.high, peak = o.low - o.high;
            const float m = m_settings.var;
            switch (info.variant) {
                case 0:  return threeWay(o.low, bp, o.high, m);
                case 1:  return threeWay(o.low, peak, o.high, m);
                case 2:  return threeWay(o.low, notch, o.high, m);
                default: return threeWay(bp, peak, notch, m);
            }
        }
        case FilterFamily::PzMorph: {
            const auto o = ch.svf.process(x, c.svf);
            const float bp = c.svf.k * o.band;
            const float morph = threeWay(o.low, bp, o.high, std::clamp(m_settings.x, 0.0f, 1.0f));
            return morph + (std::clamp(m_settings.y, 0.0f, 1.0f) - 0.5f) * 2.0f * bp;
        }
        case FilterFamily::SvfDual: {
            if (info.variant == 1) {   // LP at f1 into HP at f2
                const float lp = ch.svf.process(x, c.svf).low;
                return ch.svf2.process(lp, c.svf2).high;
            }
            if (info.mode == FilterMode::Notch) {   // notches in series
                const float n1 = svfSelect(ch.svf.process(x, c.svf), c.svf, FilterMode::Notch, x);
                return svfSelect(ch.svf2.process(n1, c.svf2), c.svf2, FilterMode::Notch, n1);
            }
            const float a = svfSelect(ch.svf.process(x, c.svf), c.svf, info.mode, x);
            const float b = svfSelect(ch.svf2.process(x, c.svf2), c.svf2, info.mode, x);
            return 0.5f * (a + b);
        }
        case FilterFamily::Ladder:
        case FilterFamily::DrivenLadder:
        case FilterFamily::Acid: {
            // Zero-delay feedback: solve the loop for y4 linearly, then (driven kinds) saturate the input.
            const float G = c.G, oneMinusG = 1.0f - G;
            auto& s = ch.ladder;
            const float G2 = G * G, G3 = G2 * G, G4 = G3 * G;
            const float S = G3 * oneMinusG * s[0] + G2 * oneMinusG * s[1] + G * oneMinusG * s[2] + oneMinusG * s[3];
            const float uIn = x * c.comp;
            const float y4 = (G4 * uIn + S) / (1.0f + c.k * G4);
            float u = uIn - c.k * y4;
            if (info.family == FilterFamily::DrivenLadder) u = std::tanh(u);
            else if (info.family == FilterFamily::Acid)    u = std::tanh(u + 0.3f) - std::tanh(0.3f);
            float in = u, taps[4];
            for (int i = 0; i < 4; ++i) {
                const float v = (in - s[static_cast<std::size_t>(i)]) * G;
                const float y = v + s[static_cast<std::size_t>(i)];
                s[static_cast<std::size_t>(i)] = y + v;
                taps[i] = y;
                in = y;
            }
            if (info.mode == FilterMode::Hp) return u - 4.0f * taps[0] + 6.0f * taps[1] - 4.0f * taps[2] + taps[3];
            return taps[std::clamp(info.poles, 1, 4) - 1];
        }
        case FilterFamily::SallenKey: {
            // TPT SVF whose band integrator is saturated: resonance is amplitude-limited, so maximum
            // resonance (negative damping) self-oscillates at a stable level.
            float& ic1 = ch.onePole;
            float& ic2 = ch.onePole2;
            const auto& k = c.svf;
            const float v3 = x - ic2;
            const float v1 = k.a1 * ic1 + k.a2 * v3;
            const float v2 = ic2 + k.a2 * ic1 + k.a3 * v3;
            ic1 = std::tanh(2.0f * v1 - ic1);
            ic2 = 2.0f * v2 - ic2;
            if (info.mode == FilterMode::Hp) return x - k.k * v1 - v2;
            if (info.mode == FilterMode::Bp) return v1;
            return v2;
        }
        case FilterFamily::Comb: {
            float d = readDelay(ch, c.delay);
            if (info.variant >= 2) d = tpLowpass(ch.combDamp, d, c.damp);
            const float y = x + c.feedback * d;
            ch.buffer[static_cast<std::size_t>(ch.writePos)] = y;
            ch.writePos = (ch.writePos + 1) & mask;
            return y * (1.0f - std::abs(c.feedback));
        }
        case FilterFamily::Flange: {
            const float d = readDelay(ch, c.delay);
            const float sign = info.variant == 1 ? -1.0f : 1.0f;
            ch.buffer[static_cast<std::size_t>(ch.writePos)] = x + c.feedback * d;
            ch.writePos = (ch.writePos + 1) & mask;
            return 0.5f * (x + sign * d);
        }
        case FilterFamily::Phaser: {
            float u = x + c.feedback * ch.lastOut;
            for (int i = 0; i < c.stages; ++i) {
                const float lp = tpLowpass(ch.ap[static_cast<std::size_t>(i)], u, c.apG[static_cast<std::size_t>(i)]);
                u = 2.0f * lp - u;   // TPT first-order allpass
            }
            ch.lastOut = u;
            return 0.5f * (x + u);
        }
        case FilterFamily::Formant: {
            float acc = 0.0f;
            for (int f = 0; f < 3; ++f) {
                const auto& k = c.formant[static_cast<std::size_t>(f)];
                acc += k.k * ch.formant[static_cast<std::size_t>(f)].process(x, k).band * c.formantGain[static_cast<std::size_t>(f)];
            }
            return acc;
        }
        case FilterFamily::SampleHold:
            ch.holdPhase += c.rate;
            if (ch.holdPhase >= 1.0) {
                ch.hold = x;
                ch.holdPhase -= std::floor(ch.holdPhase);
            }
            return ch.hold;
        case FilterFamily::RingMod: {
            const float y = x * static_cast<float>(std::sin(2.0 * kPi * ch.ringPhase));
            ch.ringPhase += c.rate;
            ch.ringPhase -= std::floor(ch.ringPhase);
            return y;
        }
        case FilterFamily::TwinLp: {
            const float a = ch.svf.process(x, c.svf).low;
            return ch.svf2.process(a, c.svf2).low;
        }
        case FilterFamily::Reverb: {
            float o[4], sum = 0.0f;
            for (int i = 0; i < 4; ++i) {
                const int base = i * kSegment;
                const int readPos = (ch.segPos[static_cast<std::size_t>(i)] - c.lines[static_cast<std::size_t>(i)] + kSegment) % kSegment;
                o[i] = tpLowpass(ch.fdnDamp[static_cast<std::size_t>(i)], ch.buffer[static_cast<std::size_t>(base + readPos)], c.damp);
                sum += o[i];
            }
            for (int i = 0; i < 4; ++i) {
                const int base = i * kSegment;
                const float f = o[i] - 0.5f * sum;   // Householder reflection: energy-preserving mix
                ch.buffer[static_cast<std::size_t>(base + ch.segPos[static_cast<std::size_t>(i)])] = 0.5f * x + c.feedback * f;
                ch.segPos[static_cast<std::size_t>(i)] = (ch.segPos[static_cast<std::size_t>(i)] + 1) % kSegment;
            }
            return 0.35f * sum;
        }
        case FilterFamily::Disperser: {
            float u = x;
            for (int i = 0; i < c.stages; ++i) {
                const auto o = ch.disperser[static_cast<std::size_t>(i)].process(u, c.svf);
                u = u - 2.0f * c.svf.k * o.band;   // 2nd-order allpass
            }
            return u;
        }
        case FilterFamily::Diffuser: {
            float u = x;
            for (int i = 0; i < 4; ++i) {
                const int base = i * kSegment;
                auto& pos = ch.segPos[static_cast<std::size_t>(i)];
                const int readPos = (pos - c.lines[static_cast<std::size_t>(i)] + kSegment) % kSegment;
                const float d = ch.buffer[static_cast<std::size_t>(base + readPos)];
                const float y = -c.feedback * u + d;   // Schroeder allpass
                ch.buffer[static_cast<std::size_t>(base + pos)] = u + c.feedback * y;
                pos = (pos + 1) % kSegment;
                u = y;
            }
            return u;
        }
    }
    return x;
}

float FilterUnit::processChannel(Channel& ch, const FilterCoefs& c, float x) noexcept
{
    if (!m_info->nonlinear) return processCore(ch, c, x);
    float a, b;
    ch.up.process_sample(a, b, x);
    const float pair[2] = {processCore(ch, c, a), processCore(ch, c, b)};
    return ch.down.process_sample(pair);
}

void FilterUnit::process(float& left, float& right) noexcept
{
    const float dryL = left, dryR = right;
    float inL = left, inR = right;
    if (m_settings.drive > 0.0f) {
        // Drive raises the level into the filter. Normal mode adds a tanh saturator in front; Clean mode
        // (Serum 2's addition, INFERRED meaning) skips it, so linear types stay clean and only the
        // nonlinear types saturate, inside their own stages.
        inL *= m_preGain;
        inR *= m_preGain;
        if (!m_settings.clean) {
            inL = std::tanh(inL);
            inR = std::tanh(inR);
        }
    }
    const float wetL = processChannel(m_ch[0], m_coefs[0], inL) * m_postGain;
    const float wetR = processChannel(m_ch[1], m_coefs[1], inR) * m_postGain;
    const float mix = m_settings.mix;
    left  = (dryL + (wetL - dryL) * mix) * m_settings.level;
    right = (dryR + (wetR - dryR) * mix) * m_settings.level;
}

} // namespace winerose::dsp
