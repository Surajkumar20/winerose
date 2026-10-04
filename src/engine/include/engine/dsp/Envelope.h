#pragma once

#include <algorithm>
#include <cmath>

namespace winerose::dsp {

/**
 * @brief Attack / hold / decay / sustain / release envelope (SPEC §1.6, §5.5), per sample.
 *
 * Stage shapes use the SPEC curve y = (exp(c·x) - 1) / (exp(c) - 1) over the stage's progress x in [0,1]
 * (c = 0 is linear, c in [-10,10]). Attack and release start from the CURRENT level, so retriggers and steals
 * don't click. Defaults: attack linear, decay/release c = -5 (fast start, slow tail). TODO-MEASURE: Serum's.
 */
class Envelope {
public:
    struct Settings {
        float attackSeconds  = 0.0005f;
        float holdSeconds    = 0.0f;
        float decaySeconds   = 1.0f;
        float sustain        = 1.0f;   // linear 0..1
        float releaseSeconds = 0.015f;
        float attackCurve    = 0.0f;
        float decayCurve     = -5.0f;
        float releaseCurve   = -5.0f;
    };

    enum class Stage { Idle, Attack, Hold, Decay, Sustain, Release };

    void setSampleRate(double sr) noexcept { m_sampleRate = sr; }
    void setSettings(const Settings& s) noexcept { m_settings = s; }

    void noteOn() noexcept { enter(Stage::Attack); }
    void noteOff() noexcept
    {
        if (m_stage != Stage::Idle && m_stage != Stage::Release) enter(Stage::Release);
    }
    void reset() noexcept { m_stage = Stage::Idle; m_level = 0.0f; }

    bool  isActive() const noexcept { return m_stage != Stage::Idle; }
    Stage stage() const noexcept { return m_stage; }
    float level() const noexcept { return m_level; }

    float next() noexcept
    {
        switch (m_stage) {
            case Stage::Idle:    return 0.0f;
            case Stage::Sustain: m_level = m_settings.sustain; return m_level;
            case Stage::Attack:
            case Stage::Hold:
            case Stage::Decay:
            case Stage::Release: break;
        }
        m_progress += m_increment;
        if (m_progress >= 1.0) {
            m_level = m_to;
            advance();
            return m_level;
        }
        m_level = m_from + (m_to - m_from) * shape(static_cast<float>(m_progress), m_curve);
        return m_level;
    }

    static float shape(float x, float c) noexcept
    {
        if (std::abs(c) < 1e-3f) return x;
        return (std::exp(c * x) - 1.0f) / (std::exp(c) - 1.0f);
    }

private:
    void enter(Stage s) noexcept
    {
        m_stage = s;
        m_progress = 0.0;
        m_from = m_level;
        double seconds = 0.0;
        switch (s) {
            case Stage::Attack:  m_to = 1.0f;               seconds = m_settings.attackSeconds;  m_curve = m_settings.attackCurve;  break;
            case Stage::Hold:    m_to = 1.0f;               seconds = m_settings.holdSeconds;    m_curve = 0.0f;                    break;
            case Stage::Decay:   m_to = m_settings.sustain; seconds = m_settings.decaySeconds;   m_curve = m_settings.decayCurve;   break;
            case Stage::Release: m_to = 0.0f;               seconds = m_settings.releaseSeconds; m_curve = m_settings.releaseCurve; break;
            case Stage::Sustain: m_level = m_settings.sustain; return;
            case Stage::Idle:    m_level = 0.0f; return;
        }
        const double samples = seconds * m_sampleRate;
        if (samples < 1.0) {               // zero-length stage: jump straight to its target
            m_level = m_to;
            advance();
            return;
        }
        m_increment = 1.0 / samples;
    }

    void advance() noexcept
    {
        switch (m_stage) {
            case Stage::Attack:  enter(Stage::Hold); break;
            case Stage::Hold:    enter(Stage::Decay); break;
            case Stage::Decay:   enter(Stage::Sustain); break;
            case Stage::Release: enter(Stage::Idle); break;
            case Stage::Sustain:
            case Stage::Idle:    break;
        }
    }

    Settings m_settings;
    double   m_sampleRate = 48000.0;
    Stage    m_stage = Stage::Idle;
    double   m_progress = 0.0, m_increment = 0.0;
    float    m_level = 0.0f, m_from = 0.0f, m_to = 0.0f, m_curve = 0.0f;
};

} // namespace winerose::dsp
