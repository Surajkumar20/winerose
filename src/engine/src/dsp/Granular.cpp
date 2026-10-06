#include "engine/dsp/Granular.h"

#include <algorithm>
#include <cmath>

namespace winerose::dsp::granular {

namespace {

constexpr int kTable = 1024;
float g_cos[kTable + 1];      // 0.5 - 0.5·cos(π·x), x in [0,1]: a rising half-Hann
float g_gauss[kTable + 1];    // exp(-t), t in [0, 12]
bool  g_ready = false;

inline float riseHann(float x) noexcept   // x in [0,1]
{
    const float f = x * kTable;
    const int i = std::min(static_cast<int>(f), kTable - 1);
    return g_cos[i] + (g_cos[i + 1] - g_cos[i]) * (f - static_cast<float>(i));
}

inline float expNeg(float t) noexcept   // t >= 0
{
    const float f = std::min(t, 12.0f) * (kTable / 12.0f);
    const int i = std::min(static_cast<int>(f), kTable - 1);
    return g_gauss[i] + (g_gauss[i + 1] - g_gauss[i]) * (f - static_cast<float>(i));
}

inline float windowAt(Window w, float x, float shape) noexcept
{
    switch (w) {
        case Window::Hann:      return x < 0.5f ? riseHann(2.0f * x) : riseHann(2.0f * (1.0f - x));
        case Window::Tukey: {
            const float a = std::max(shape, 0.001f) * 0.5f;   // taper on each side
            if (x < a) return riseHann(x / a);
            if (x > 1.0f - a) return riseHann((1.0f - x) / a);
            return 1.0f;
        }
        case Window::Gaussian: {
            const float d = (x - 0.5f) / shape;   // shape = sigma
            return expNeg(0.5f * d * d);
        }
        case Window::Triangle:  return 1.0f - std::abs(2.0f * x - 1.0f);
        case Window::Rectangle: return 1.0f;
        case Window::Count:     break;
    }
    return 0.0f;
}

inline float hermite(const float* d, double pos) noexcept
{
    const double fl = std::floor(pos);
    const auto i = static_cast<std::int64_t>(fl);
    const float t = static_cast<float>(pos - fl);
    const float xm1 = d[i - 1], x0 = d[i], x1 = d[i + 1], x2 = d[i + 2];
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

} // namespace

void init() noexcept
{
    if (g_ready) return;
    for (int i = 0; i <= kTable; ++i) {
        const double x = static_cast<double>(i) / kTable;
        g_cos[i] = static_cast<float>(0.5 - 0.5 * std::cos(3.14159265358979323846 * x));
        g_gauss[i] = static_cast<float>(std::exp(-12.0 * x));
    }
    g_ready = true;
}

float Engine::random() noexcept
{
    m_rng ^= m_rng >> 12;
    m_rng ^= m_rng << 25;
    m_rng ^= m_rng >> 27;
    return static_cast<float>((m_rng * 0x2545F4914F6CDD1Dull) >> 40) * (1.0f / 16777216.0f);
}

void Engine::start(const Params&, std::uint64_t seed) noexcept
{
    m_active = 0;
    m_scan = 0.0;
    m_untilNext = 0.0;   // first grain immediately
    m_rng = seed | 1u;
}

void Engine::spawn(const SampleData& s, const Params& p) noexcept
{
    if (m_active >= kMaxGrains) return;   // pool full: skip this grain (bounded cost)
    const double frames = static_cast<double>(s.frames());
    if (frames < 4.0) return;

    double where = p.position + m_scan + (random() - 0.5f) * p.posRandom;
    where -= std::floor(where);
    const float semis = (random() * 2.0f - 1.0f) * 12.0f * p.pitchRandom;
    const float pan = std::clamp((random() * 2.0f - 1.0f) * p.panRandom, -1.0f, 1.0f);
    const double lengthSamples = std::max(1.0, static_cast<double>(p.sizeMs) * 0.001 * p.sampleRate);

    Grain& g = m_grains[static_cast<std::size_t>(m_active++)];
    g.pos = where * frames;
    g.inc = p.pitch * std::exp2(semis / 12.0) * s.sampleRate() / p.sampleRate;
    g.level = g.inc > 1.0 ? std::min(static_cast<int>(std::ceil(std::log2(g.inc))), s.levelCount() - 1) : 0;
    g.scale = 1.0 / static_cast<double>(1 << g.level);
    g.phase = 0.0f;
    g.phaseInc = static_cast<float>(1.0 / lengthSamples);
    const float angle = (pan + 1.0f) * 0.25f * 3.14159265f;
    g.gainL = std::cos(angle) * 1.41421356f * 0.5f;   // centre = 0.5 per side (overlapping grains sum up)
    g.gainR = std::sin(angle) * 1.41421356f * 0.5f;
    g.window = p.window;
    g.shape = p.window == Window::Gaussian ? 0.05f + 0.25f * p.windowAmount : p.windowAmount;
}

void Engine::render(const SampleData& s, const Params& p, float* outL, float* outR, int n) noexcept
{
    const double frames = static_cast<double>(s.frames());
    if (frames < 4.0) return;
    const double interval = p.sampleRate / std::clamp(static_cast<double>(p.density), 0.1, 1000.0);
    const double scanStep = static_cast<double>(p.scan) * s.sampleRate() / p.sampleRate / frames;
    const bool stereo = s.channels() == 2;

    for (int i = 0; i < n; ++i) {
        m_untilNext -= 1.0;
        while (m_untilNext <= 0.0) {
            spawn(s, p);
            m_untilNext += interval;
        }
        m_scan += scanStep;
        m_scan -= std::floor(m_scan);

        float l = 0.0f, r = 0.0f;
        for (int k = 0; k < m_active;) {
            Grain& g = m_grains[static_cast<std::size_t>(k)];
            // Each grain reads the mip level where its increment is <= 1 (no aliasing with Hermite reads).
            const int level = g.level;
            double q = g.pos * g.scale;
            const double lf = static_cast<double>(s.levelFrames(level));
            if (q >= lf) q -= lf * std::floor(q / lf);   // grains wrap around the sample
            const float w = windowAt(g.window, g.phase, g.shape);
            const float a = hermite(s.data(level, 0), q);
            const float b = stereo ? hermite(s.data(level, 1), q) : a;
            l += a * w * g.gainL;
            r += b * w * g.gainR;

            g.pos += g.inc;
            g.phase += g.phaseInc;
            if (g.phase >= 1.0f) g = m_grains[static_cast<std::size_t>(--m_active)];   // swap-remove; re-check k
            else ++k;
        }
        outL[i] += l;
        outR[i] += r;
    }
}

} // namespace winerose::dsp::granular
