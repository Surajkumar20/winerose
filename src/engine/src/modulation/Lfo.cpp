#include "engine/modulation/Lfo.h"

#include <algorithm>

namespace winerose::modulation {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Chaos constants (SPEC §5.5) and time scaling: model time units per second at 1 Hz, chosen so one
// "orbit" takes about one LFO cycle (INFERRED).
constexpr double kLorenzSigma = 10.0, kLorenzRho = 28.0, kLorenzBeta = 8.0 / 3.0;
constexpr double kRosslerA = 0.2, kRosslerB = 0.2, kRosslerC = 5.7;
constexpr double kLorenzUnitsPerCycle  = 0.7;
constexpr double kRosslerUnitsPerCycle = 6.0;
constexpr int    kMaxChaosSteps = 64;

// Env mode holds just before the end of the cycle: exactly at phase 1 a band-limited table sits in the
// middle of its wrap-around edge (e.g. a saw's reset), not at the shape's final value.
constexpr double kEnvHoldPhase = 1.0 - 8.0 / dsp::WavetableBank::kFrameSize;

struct Vec3 { double x, y, z; };

Vec3 lorenz(const Vec3& v) noexcept
{
    return {kLorenzSigma * (v.y - v.x), v.x * (kLorenzRho - v.z) - v.y, v.x * v.y - kLorenzBeta * v.z};
}

Vec3 rossler(const Vec3& v) noexcept
{
    return {-v.y - v.z, v.x + kRosslerA * v.y, kRosslerB + v.z * (v.x - kRosslerC)};
}

template<typename F>
Vec3 rk4(const Vec3& v, double h, F f) noexcept
{
    const Vec3 k1 = f(v);
    const Vec3 k2 = f({v.x + 0.5 * h * k1.x, v.y + 0.5 * h * k1.y, v.z + 0.5 * h * k1.z});
    const Vec3 k3 = f({v.x + 0.5 * h * k2.x, v.y + 0.5 * h * k2.y, v.z + 0.5 * h * k2.z});
    const Vec3 k4 = f({v.x + h * k3.x, v.y + h * k3.y, v.z + h * k3.z});
    return {v.x + h / 6.0 * (k1.x + 2 * k2.x + 2 * k3.x + k4.x),
            v.y + h / 6.0 * (k1.y + 2 * k2.y + 2 * k3.y + k4.y),
            v.z + h / 6.0 * (k1.z + 2 * k2.z + 2 * k3.z + k4.z)};
}

} // namespace

double LfoState::nextRandom() noexcept
{
    m_rng ^= m_rng >> 12;
    m_rng ^= m_rng << 25;
    m_rng ^= m_rng >> 27;
    return static_cast<double>((m_rng * 0x2545F4914F6CDD1Dull) >> 11) * (1.0 / 9007199254740992.0);
}

void LfoState::start(const LfoSettings& s, double freePhase, std::uint64_t seed) noexcept
{
    m_settings = s;
    m_rng = seed | 1ull;
    m_phase = s.mode == LfoMode::Free ? freePhase : static_cast<double>(s.phase);
    m_phase -= std::floor(m_phase);
    m_time = 0.0;
    m_finished = false;
    m_smoothPrimed = false;
    m_randPrev = static_cast<float>(nextRandom());
    m_randNext = static_cast<float>(nextRandom());
    initChaos();
}

void LfoState::initChaos() noexcept
{
    // Start on (near) the attractor with a little per-voice jitter; seed the output range with its
    // typical extent so the first seconds aren't over-scaled.
    if (m_settings.shape == LfoShape::Lorenz) {
        m_cx = 0.1 + 0.01 * nextRandom(); m_cy = 0.0; m_cz = 20.0;
        m_min = -20.0; m_max = 20.0;
    } else {
        m_cx = 1.0 + 0.01 * nextRandom(); m_cy = 0.0; m_cz = 0.0;
        m_min = -10.0; m_max = 12.0;
    }
}

void LfoState::configure(const LfoSettings& s, double sampleRate, double bpm) noexcept
{
    const LfoShape previous = m_settings.shape;
    m_settings = s;
    m_sampleRate = sampleRate;
    m_inc = std::clamp(s.frequency(bpm), 0.0, 0.45 * sampleRate) / sampleRate;
    m_levels = dsp::WavetableBank::selectLevel(m_inc);
    const double tau = static_cast<double>(s.smooth) * 0.1;   // seconds
    m_smoothCoef = tau > 1e-6 ? static_cast<float>(std::exp(-1.0 / (tau * sampleRate))) : 0.0f;
    if (s.shape != previous && isChaos(s.shape)) initChaos();   // switched into a chaos shape: seed it
}

void LfoState::onWrap() noexcept
{
    m_randPrev = m_randNext;
    m_randNext = static_cast<float>(nextRandom());
}

void LfoState::integrateChaos(double seconds) noexcept
{
    const bool lor = m_settings.shape == LfoShape::Lorenz;
    const double units = seconds * m_inc * m_sampleRate * (lor ? kLorenzUnitsPerCycle : kRosslerUnitsPerCycle);
    if (units <= 0.0) return;
    const double hMax = lor ? 0.01 : 0.05;
    const int steps = std::clamp(static_cast<int>(std::ceil(units / hMax)), 1, kMaxChaosSteps);
    const double h = units / steps;
    Vec3 v {m_cx, m_cy, m_cz};
    for (int i = 0; i < steps; ++i) v = lor ? rk4(v, h, lorenz) : rk4(v, h, rossler);
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) v = {0.1, 0.0, lor ? 20.0 : 0.0};
    m_cx = v.x; m_cy = v.y; m_cz = v.z;
    m_min = std::min(m_min, m_cx);
    m_max = std::max(m_max, m_cx);
}

float LfoState::raw(const LfoTables& tables) const noexcept
{
    const LfoShape shape = m_settings.shape;
    if (shape == LfoShape::Path) {
        if (tables.path == nullptr) return 0.0f;
        return 0.5f + 0.5f * tables.path->read(m_phase, 0.0f, m_levels);
    }
    if (isPeriodicTableShape(shape)) {
        if (tables.shapes == nullptr) return 0.0f;
        const float frame = static_cast<float>(static_cast<int>(shape) - static_cast<int>(LfoShape::Sine));
        return 0.5f + 0.5f * tables.shapes->read(m_phase, frame, m_levels);
    }
    switch (shape) {
        case LfoShape::SampleHold:
            return m_randPrev;
        case LfoShape::SmoothRandom: {
            const float w = static_cast<float>(0.5 - 0.5 * std::cos(kPi * m_phase));
            return m_randPrev + (m_randNext - m_randPrev) * w;
        }
        case LfoShape::Lorenz:
        case LfoShape::Rossler:
            return m_max > m_min ? static_cast<float>((m_cx - m_min) / (m_max - m_min)) : 0.5f;
        default:
            return 0.0f;
    }
}

float LfoState::shaped(const LfoTables& tables) noexcept
{
    const float v = raw(tables);
    const double afterDelay = m_time - m_settings.delaySeconds;
    if (afterDelay <= 0.0) return v * (m_settings.riseSeconds > 0.0f ? 0.0f : 1.0f);
    if (m_settings.riseSeconds <= 0.0f) return v;
    return v * static_cast<float>(std::min(1.0, afterDelay / m_settings.riseSeconds));
}

void LfoState::advance(int n, const LfoTables& tables) noexcept
{
    if (n < 0) return;
    const double seconds = n / m_sampleRate;
    const bool delayed = m_time < m_settings.delaySeconds;
    m_time += seconds;

    if (n > 0 && !delayed && !m_finished) {
        if (isChaos(m_settings.shape)) {
            integrateChaos(seconds);
        } else {
            m_phase += m_inc * n;
            if (m_phase >= 1.0) {
                if (m_settings.mode == LfoMode::Env) {
                    m_phase = kEnvHoldPhase;   // hold at the end of the cycle
                    m_finished = true;
                } else {
                    const double wraps = std::floor(m_phase);
                    m_phase -= wraps;
                    onWrap();
                    if (wraps > 1.0) onWrap();   // skipped cycles: keep the random walk moving
                }
            }
        }
    }

    const float target = shaped(tables);
    if (m_smoothCoef > 0.0f && m_smoothPrimed) {
        const float a = static_cast<float>(std::pow(static_cast<double>(m_smoothCoef), n));
        m_output = target + (m_output - target) * a;
    } else {
        m_output = target;
        m_smoothPrimed = true;
    }
}

float LfoState::tick(const LfoTables& tables) noexcept
{
    m_time += 1.0 / m_sampleRate;
    if (m_time >= m_settings.delaySeconds && !m_finished && !isChaos(m_settings.shape)) {
        m_phase += m_inc;
        if (m_phase >= 1.0) {
            if (m_settings.mode == LfoMode::Env) { m_phase = kEnvHoldPhase; m_finished = true; }
            else                                 { m_phase -= 1.0; onWrap(); }
        }
    }
    const float target = shaped(tables);
    m_output = (m_smoothCoef > 0.0f && m_smoothPrimed) ? target + (m_output - target) * m_smoothCoef : target;
    m_smoothPrimed = true;
    return m_output;
}

} // namespace winerose::modulation
