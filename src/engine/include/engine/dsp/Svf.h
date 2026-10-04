#pragma once

#include <algorithm>
#include <cmath>

namespace winerose::dsp {

/**
 * @brief Trapezoidal (TPT / zero-delay-feedback) state-variable filter, 12 dB/oct.
 *
 * Published topology: Zavalishin, "The Art of VA Filter Design" ch. 4; Simper/Cytomic SVF notes (SPEC §1.5).
 * g = tan(pi·fc/fs), k = 2 - 2·r for resonance r in [0,1). Coefficients are computed per control block;
 * process() is per sample and allocation-free. r = 0 gives k = 2 (critically damped);
 * r = 1 - √2/2 ≈ 0.293 gives k = √2 (Butterworth, exactly -3 dB at the cutoff).
 */
class Svf {
public:
    struct Coefs {
        float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, k = 2.0f;
    };

    /** cutoff is clamped below 0.49·fs (tan() blows up at Nyquist); resonance is clamped to [0, 0.995]. */
    static Coefs compute(double cutoffHz, double resonance, double sampleRate) noexcept
    {
        constexpr double kPi = 3.14159265358979323846;
        const double fc = std::clamp(cutoffHz, 1.0, 0.49 * sampleRate);
        const double g  = std::tan(kPi * fc / sampleRate);
        const double k  = 2.0 - 2.0 * std::clamp(resonance, 0.0, 0.995);
        Coefs c;
        c.k  = static_cast<float>(k);
        c.a1 = static_cast<float>(1.0 / (1.0 + g * (g + k)));
        c.a2 = static_cast<float>(g) * c.a1;
        c.a3 = static_cast<float>(g) * c.a2;
        return c;
    }

    struct Outputs { float low, band, high; };

    Outputs process(float x, const Coefs& c) noexcept
    {
        const float v3 = x - m_ic2;
        const float v1 = c.a1 * m_ic1 + c.a2 * v3;
        const float v2 = m_ic2 + c.a2 * m_ic1 + c.a3 * v3;
        m_ic1 = 2.0f * v1 - m_ic1;
        m_ic2 = 2.0f * v2 - m_ic2;
        return {v2, v1, x - c.k * v1 - v2};
    }

    float processLow(float x, const Coefs& c) noexcept { return process(x, c).low; }

    void reset() noexcept { m_ic1 = m_ic2 = 0.0f; }

private:
    float m_ic1 = 0.0f, m_ic2 = 0.0f;
};

} // namespace winerose::dsp
