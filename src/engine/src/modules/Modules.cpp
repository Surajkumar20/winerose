#include "engine/modules/Modules.h"

namespace winerose::modules {

namespace {
// Envelope times: 0..32 s on a cubic knob (SPEC §5.5 hypothesis t = 32·x³, TODO-MEASURE).
const ParamOpts kTimeOpts { NumericMeta::Curve::Power, 3.0, "s" };
}

// --- Oscillator --------------------------------------------------------------------------------------

OscillatorModule::OscillatorModule(std::shared_ptr<ConfigManager> config, int index)
    : m_registry(std::make_unique<ParamRegistry>(std::move(config), "Oscillator" + std::to_string(index)))
{
    auto& r = *m_registry;
    const std::string g = "Osc";
    r.registerBool (osc_keys::enabled, index == 0, g, "Oscillator on/off (A is on in the init patch)");
    r.registerFloat(osc_keys::level, 0.75f, 0.0f, 1.0f, g, "Level (linear; TODO-MEASURE dB law)");
    r.registerFloat(osc_keys::pan, 0.0f, -1.0f, 1.0f, g, "Pan (constant power)");
    r.registerInt  (osc_keys::octave, 0, -4, 4, g, "Octave");
    r.registerInt  (osc_keys::semi, 0, -12, 12, g, "Semitones");
    r.registerFloat(osc_keys::fine, 0.0f, -100.0f, 100.0f, g, "Fine tune", ParamOpts{.unit = "cents"});
    r.registerFloat(osc_keys::wtPos, 0.0f, 0.0f, 1.0f, g, "Wavetable position (smooth morph between frames)");
    r.registerFloat(osc_keys::phase, 0.0f, 0.0f, 1.0f, g, "Start phase");
    r.registerFloat(osc_keys::random, 1.0f, 0.0f, 1.0f, g, "Random start-phase amount (TODO-MEASURE default)");

    m_enabled = r.handle(osc_keys::enabled);
    m_level   = r.handle(osc_keys::level);
    m_pan     = r.handle(osc_keys::pan);
    m_octave  = r.handle(osc_keys::octave);
    m_semi    = r.handle(osc_keys::semi);
    m_fine    = r.handle(osc_keys::fine);
    m_wtPos   = r.handle(osc_keys::wtPos);
    m_phase   = r.handle(osc_keys::phase);
    m_random  = r.handle(osc_keys::random);
}

OscillatorModule::Values OscillatorModule::read() const noexcept
{
    return Values{
        m_enabled.load() >= 0.5f,
        m_level.load(),
        m_pan.load(),
        m_octave.load() * 12.0f + m_semi.load() + m_fine.load() / 100.0f,
        m_wtPos.load(),
        m_phase.load(),
        m_random.load(),
    };
}

// --- Filter ------------------------------------------------------------------------------------------

FilterModule::FilterModule(std::shared_ptr<ConfigManager> config, int index)
    : m_registry(std::make_unique<ParamRegistry>(std::move(config), "Filter" + std::to_string(index)))
{
    auto& r = *m_registry;
    const std::string g = "Filter";
    r.registerBool (filter_keys::enabled, false, g, "Filter on/off (Filter 1 starts disabled, SPEC §1.1)");
    r.registerFloat(filter_keys::cutoff, 425.0f, 8.18f, 22050.0f, g,
                    "Cutoff (exponential knob, INFERRED range 8.18 Hz - 22.05 kHz)",
                    ParamOpts{NumericMeta::Curve::Exp, 1.0, "Hz"});
    r.registerFloat(filter_keys::resonance, 0.1f, 0.0f, 1.0f, g, "Resonance (TODO-MEASURE default)");

    m_enabled   = r.handle(filter_keys::enabled);
    m_cutoff    = r.handle(filter_keys::cutoff);
    m_resonance = r.handle(filter_keys::resonance);
}

FilterModule::Values FilterModule::read() const noexcept
{
    return Values{m_enabled.load() >= 0.5f, m_cutoff.load(), m_resonance.load()};
}

// --- Envelope ----------------------------------------------------------------------------------------

EnvelopeModule::EnvelopeModule(std::shared_ptr<ConfigManager> config, int index)
    : m_registry(std::make_unique<ParamRegistry>(std::move(config), "Env" + std::to_string(index)))
{
    auto& r = *m_registry;
    const std::string g = "Env";
    const dsp::Envelope::Settings d;
    r.registerFloat(env_keys::attack, d.attackSeconds, 0.0f, 32.0f, g, "Attack time", kTimeOpts);
    r.registerFloat(env_keys::hold, d.holdSeconds, 0.0f, 32.0f, g, "Hold time", kTimeOpts);
    r.registerFloat(env_keys::decay, d.decaySeconds, 0.0f, 32.0f, g, "Decay time", kTimeOpts);
    r.registerFloat(env_keys::sustain, d.sustain, 0.0f, 1.0f, g, "Sustain level (linear; TODO-MEASURE dB law)");
    r.registerFloat(env_keys::release, d.releaseSeconds, 0.0f, 32.0f, g, "Release time", kTimeOpts);

    m_attack  = r.handle(env_keys::attack);
    m_hold    = r.handle(env_keys::hold);
    m_decay   = r.handle(env_keys::decay);
    m_sustain = r.handle(env_keys::sustain);
    m_release = r.handle(env_keys::release);
}

dsp::Envelope::Settings EnvelopeModule::read() const noexcept
{
    return {m_attack.load(), m_hold.load(), m_decay.load(), m_sustain.load(), m_release.load()};
}

} // namespace winerose::modules
