#pragma once

#include <algorithm>

namespace winerose {

// Linear ramp toward a target over a fixed time (SPEC §1.9: smooth every host parameter, 5-20 ms).
class LinearSmoother {
public:
    void prepare(double sampleRate, double rampSeconds) noexcept
    {
        m_rampSamples = static_cast<int>(std::max(1.0, sampleRate * rampSeconds));
    }

    /** Exact ramp length in samples (used for control-block ramps, which must not depend on rounding). */
    void setRampSamples(int samples) noexcept { m_rampSamples = std::max(1, samples); }

    void reset(float value) noexcept
    {
        m_current = m_target = value;
        m_remaining = 0;
    }

    void setTarget(float target) noexcept
    {
        if (target == m_target) return;
        m_target = target;
        m_remaining = m_rampSamples;
        m_step = (m_target - m_current) / static_cast<float>(m_remaining);
    }

    float next() noexcept
    {
        if (m_remaining <= 0) return m_current;
        if (--m_remaining == 0) m_current = m_target;
        else                    m_current += m_step;
        return m_current;
    }

    bool  isSmoothing() const noexcept { return m_remaining > 0; }
    float current() const noexcept { return m_current; }

private:
    float m_current = 0.0f, m_target = 0.0f, m_step = 0.0f;
    int   m_remaining = 0, m_rampSamples = 1;
};

} // namespace winerose
