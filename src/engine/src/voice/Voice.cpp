#include "engine/voice/Voice.h"

#include <algorithm>
#include <cmath>

namespace winerose::voice {

namespace {

constexpr float kQuarterPi = 0.78539816339744831f;
constexpr float kSqrt2     = 1.41421356237309505f;

// Constant-power pan, scaled so the centre is unity gain on both sides.
void panGains(float pan, float& left, float& right) noexcept
{
    const float theta = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * kQuarterPi;
    left  = std::cos(theta) * kSqrt2;
    right = std::sin(theta) * kSqrt2;
}

} // namespace

void Voice::prepare(double sampleRate) noexcept
{
    m_env.setSampleRate(sampleRate);
    for (auto* s : {&m_wtPos, &m_gain, &m_panL, &m_panR}) s->setRampSamples(kControlBlock);
    m_env.reset();
    m_svf.reset();
    m_released = true;
    m_sustained = false;
    m_note = -1;
}

void Voice::updatePitch(const VoiceControl& control) noexcept
{
    const double semis = static_cast<double>(m_note) - 69.0 + control.osc.pitchSemis;
    const double hz = 440.0 * std::exp2(semis / 12.0);
    m_increment = std::min(hz / control.sampleRate, 0.5);
    m_levels = dsp::WavetableBank::selectLevel(m_increment);
}

void Voice::start(int note, std::uint64_t order, double startPhase, const VoiceControl& control) noexcept
{
    m_note = note;
    m_order = order;
    m_released = false;
    m_sustained = false;
    m_phase = startPhase - std::floor(startPhase);
    m_svf.reset();

    // Fresh voice: no ramps — start exactly at the current control values.
    float l, r;
    panGains(control.osc.pan, l, r);
    m_wtPos.reset(control.osc.wtPos);
    m_gain.reset(control.osc.level);
    m_panL.reset(l);
    m_panR.reset(r);
    m_filterOn = control.filter.enabled;
    m_oscOn = control.osc.enabled;
    m_coefs = control.filterCoefs;
    updatePitch(control);

    m_env.setSettings(control.env);
    m_env.reset();
    m_env.noteOn();
}

void Voice::steal(int note, std::uint64_t order, const VoiceControl& control) noexcept
{
    m_note = note;
    m_order = order;
    m_released = false;
    m_sustained = false;
    updatePitch(control);
    m_env.setSettings(control.env);
    m_env.noteOn();   // attacks from the current level
}

void Voice::control(const VoiceControl& control) noexcept
{
    float l, r;
    panGains(control.osc.pan, l, r);
    m_wtPos.setTarget(control.osc.wtPos);
    m_gain.setTarget(control.osc.level);
    m_panL.setTarget(l);
    m_panR.setTarget(r);
    if (control.filter.enabled != m_filterOn) m_svf.reset();
    m_filterOn = control.filter.enabled;
    m_oscOn = control.osc.enabled;
    m_coefs = control.filterCoefs;
    m_env.setSettings(control.env);
    updatePitch(control);
}

void Voice::render(float* left, float* right, int numSamples, const dsp::WavetableBank* bank) noexcept
{
    if (!m_env.isActive()) return;
    const bool sounding = m_oscOn && bank != nullptr;
    const float lastFrame = sounding ? static_cast<float>(bank->frameCount() - 1) : 0.0f;

    for (int i = 0; i < numSamples; ++i) {
        const float env  = m_env.next();
        const float wt   = m_wtPos.next();
        const float gain = m_gain.next();
        const float gl   = m_panL.next();
        const float gr   = m_panR.next();

        float x = 0.0f;
        if (sounding) {
            x = bank->read(m_phase, wt * lastFrame, m_levels);
            if (m_filterOn) x = m_svf.processLow(x, m_coefs);
        }
        m_phase += m_increment;
        if (m_phase >= 1.0) m_phase -= 1.0;

        const float y = x * env * gain;
        left[i]  += y * gl;
        right[i] += y * gr;
    }
}

} // namespace winerose::voice
