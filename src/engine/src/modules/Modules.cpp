#include "engine/modules/Modules.h"

#include <cmath>
#include <vector>

namespace winerose::modules {

namespace {

// Envelope times: 0..32 s on a cubic knob (SPEC §5.5 hypothesis t = 32·x³, TODO-MEASURE).
const ParamOpts kTimeOpts { NumericMeta::Curve::Power, 3.0, "s" };

template<std::size_t N>
std::vector<EnumChoice> choices(const char* const (&names)[N])
{
    std::vector<EnumChoice> out;
    for (std::size_t i = 0; i < N; ++i) out.push_back({static_cast<int>(i), names[i]});
    return out;
}

int asInt(const ParamHandle& h) noexcept { return static_cast<int>(std::lround(h.load())); }

} // namespace

// --- Wavetable oscillator ----------------------------------------------------------------------------

OscillatorModule::OscillatorModule(std::shared_ptr<ConfigManager> config, int index)
    : m_registry(std::make_unique<ParamRegistry>(std::move(config), "Oscillator" + std::to_string(index)))
{
    auto& r = *m_registry;
    const std::string g = "Osc", gu = "Unison", gw = "Warp";
    r.registerBool (osc_keys::enabled, index == 0, g, "Oscillator on/off (only A is on in the init patch)");
    r.registerFloat(osc_keys::level, 0.75f, 0.0f, 1.0f, g, "Level (linear; TODO-MEASURE dB law)");
    r.registerFloat(osc_keys::pan, 0.0f, -1.0f, 1.0f, g, "Pan (constant power)");
    r.registerInt  (osc_keys::octave, 0, -4, 4, g, "Octave");
    r.registerInt  (osc_keys::semi, 0, -12, 12, g, "Semitones");
    r.registerFloat(osc_keys::fine, 0.0f, -100.0f, 100.0f, g, "Fine tune", ParamOpts{.unit = "cents"});
    r.registerFloat(osc_keys::wtPos, 0.0f, 0.0f, 1.0f, g, "Wavetable position");
    r.registerBool (osc_keys::wtSmooth, true, g, "Smooth interpolation between frames (off: nearest frame)");
    r.registerFloat(osc_keys::phase, 0.0f, 0.0f, 1.0f, g, "Start phase (100% = Mem: continue from the last note)");
    r.registerFloat(osc_keys::random, 1.0f, 0.0f, 1.0f, g, "Random start-phase amount (TODO-MEASURE default)");

    r.registerInt  (osc_keys::unison, 1, 1, dsp::unison::kMaxVoices, gu, "Unison voices");
    r.registerFloat(osc_keys::uniDetune, 0.25f, 0.0f, 1.0f, gu, "Unison detune (fraction of range)");
    r.registerFloat(osc_keys::uniBlend, 0.75f, 0.0f, 1.0f, gu, "Centre vs side voices (75% = even)");
    r.registerFloat(osc_keys::uniWidth, 1.0f, 0.0f, 1.0f, gu, "Unison stereo width");
    r.registerFloat(osc_keys::uniRange, 2.0f, 0.0f, 48.0f, gu, "Detune range", ParamOpts{.unit = "st"});
    r.registerEnum (osc_keys::uniStack, choices(dsp::unison::kStackNames), 0, gu, "Octave/fifth stacking (TODO-MEASURE order)");
    r.registerEnum (osc_keys::uniMode, choices(dsp::unison::kModeNames), 0, gu, "Detune curve");
    r.registerFloat(osc_keys::uniSpan, 0.0f, 0.0f, 1.0f, gu, "Wavetable-position spread across voices (INFERRED)");
    r.registerFloat(osc_keys::uniRandStart, 1.0f, 0.0f, 1.0f, gu, "Independent random phase per voice (INFERRED)");
    r.registerFloat(osc_keys::uniWarp, 0.0f, 0.0f, 1.0f, gu, "Warp-amount spread across voices");

    r.registerEnum (osc_keys::warp1Mode, choices(dsp::kWarpNames), 0, gw, "Warp 1 (TODO-MEASURE menu order)");
    r.registerFloat(osc_keys::warp1Amount, 0.0f, 0.0f, 1.0f, gw, "Warp 1 amount");
    r.registerEnum (osc_keys::warp2Mode, choices(dsp::kWarpNames), 0, gw, "Warp 2 (applied after warp 1)");
    r.registerFloat(osc_keys::warp2Amount, 0.0f, 0.0f, 1.0f, gw, "Warp 2 amount");

    m_enabled = r.handle(osc_keys::enabled);       m_level = r.handle(osc_keys::level);
    m_pan = r.handle(osc_keys::pan);               m_octave = r.handle(osc_keys::octave);
    m_semi = r.handle(osc_keys::semi);             m_fine = r.handle(osc_keys::fine);
    m_wtPos = r.handle(osc_keys::wtPos);           m_wtSmooth = r.handle(osc_keys::wtSmooth);
    m_phase = r.handle(osc_keys::phase);           m_random = r.handle(osc_keys::random);
    m_unison = r.handle(osc_keys::unison);         m_uniDetune = r.handle(osc_keys::uniDetune);
    m_uniBlend = r.handle(osc_keys::uniBlend);     m_uniWidth = r.handle(osc_keys::uniWidth);
    m_uniRange = r.handle(osc_keys::uniRange);     m_uniStack = r.handle(osc_keys::uniStack);
    m_uniMode = r.handle(osc_keys::uniMode);       m_uniSpan = r.handle(osc_keys::uniSpan);
    m_uniRandStart = r.handle(osc_keys::uniRandStart); m_uniWarp = r.handle(osc_keys::uniWarp);
    m_warp1Mode = r.handle(osc_keys::warp1Mode);   m_warp1Amount = r.handle(osc_keys::warp1Amount);
    m_warp2Mode = r.handle(osc_keys::warp2Mode);   m_warp2Amount = r.handle(osc_keys::warp2Amount);
}

OscillatorModule::Values OscillatorModule::read() const noexcept
{
    Values v;
    v.enabled      = m_enabled.load() >= 0.5f;
    v.level        = m_level.load();
    v.pan          = m_pan.load();
    v.pitchSemis   = m_octave.load() * 12.0f + m_semi.load() + m_fine.load() / 100.0f;
    v.wtPos        = m_wtPos.load();
    v.wtSmooth     = m_wtSmooth.load() >= 0.5f;
    v.phase        = m_phase.load();
    v.random       = m_random.load();
    v.unison       = std::clamp(asInt(m_unison), 1, dsp::unison::kMaxVoices);
    v.uniDetune    = m_uniDetune.load();
    v.uniBlend     = m_uniBlend.load();
    v.uniWidth     = m_uniWidth.load();
    v.uniRange     = m_uniRange.load();
    v.stack        = static_cast<dsp::unison::Stack>(std::clamp(asInt(m_uniStack), 0, static_cast<int>(dsp::unison::Stack::Count) - 1));
    v.mode         = static_cast<dsp::unison::Mode>(std::clamp(asInt(m_uniMode), 0, static_cast<int>(dsp::unison::Mode::Count) - 1));
    v.uniSpan      = m_uniSpan.load();
    v.uniRandStart = m_uniRandStart.load();
    v.uniWarp      = m_uniWarp.load();
    v.warp1        = static_cast<dsp::WarpMode>(std::clamp(asInt(m_warp1Mode), 0, static_cast<int>(dsp::WarpMode::Count) - 1));
    v.warp1Amount  = m_warp1Amount.load();
    v.warp2        = static_cast<dsp::WarpMode>(std::clamp(asInt(m_warp2Mode), 0, static_cast<int>(dsp::WarpMode::Count) - 1));
    v.warp2Amount  = m_warp2Amount.load();
    return v;
}

// --- Noise -------------------------------------------------------------------------------------------

NoiseModule::NoiseModule(std::shared_ptr<ConfigManager> config)
    : m_registry(std::make_unique<ParamRegistry>(std::move(config), "Oscillator" + std::to_string(kIndex)))
{
    auto& r = *m_registry;
    const std::string g = "Noise";
    r.registerBool (noise_keys::enabled, false, g, "Noise oscillator on/off");
    r.registerEnum (noise_keys::type, choices(dsp::NoiseTables::kTypeNames), 0, g, "Noise colour (built-in sources)");
    r.registerFloat(noise_keys::level, 0.75f, 0.0f, 1.0f, g, "Level");
    r.registerFloat(noise_keys::pan, 0.0f, -1.0f, 1.0f, g, "Pan");
    r.registerBool (noise_keys::keytrack, false, g, "Playback rate follows the note");
    r.registerFloat(noise_keys::pitch, 0.0f, -48.0f, 48.0f, g, "Playback-rate offset", ParamOpts{.unit = "st"});
    m_enabled = r.handle(noise_keys::enabled);  m_type = r.handle(noise_keys::type);
    m_level = r.handle(noise_keys::level);      m_pan = r.handle(noise_keys::pan);
    m_keytrack = r.handle(noise_keys::keytrack); m_pitch = r.handle(noise_keys::pitch);
}

NoiseModule::Values NoiseModule::read() const noexcept
{
    return Values{
        m_enabled.load() >= 0.5f,
        static_cast<dsp::NoiseTables::Type>(std::clamp(asInt(m_type), 0, static_cast<int>(dsp::NoiseTables::Type::Count) - 1)),
        m_level.load(),
        m_pan.load(),
        m_keytrack.load() >= 0.5f,
        m_pitch.load(),
    };
}

// --- Sub ---------------------------------------------------------------------------------------------

SubOscModule::SubOscModule(std::shared_ptr<ConfigManager> config)
    : m_registry(std::make_unique<ParamRegistry>(std::move(config), "Oscillator" + std::to_string(kIndex)))
{
    auto& r = *m_registry;
    const std::string g = "Sub";
    r.registerBool (sub_keys::enabled, false, g, "Sub oscillator on/off");
    r.registerEnum (sub_keys::shape, choices(dsp::kSubShapeNames), 0, g, "Shape (TODO-MEASURE order)");
    r.registerInt  (sub_keys::octave, 0, -4, 4, g, "Octave (TODO-MEASURE default)");
    r.registerFloat(sub_keys::level, 0.75f, 0.0f, 1.0f, g, "Level");
    r.registerFloat(sub_keys::pan, 0.0f, -1.0f, 1.0f, g, "Pan");
    m_enabled = r.handle(sub_keys::enabled);  m_shape = r.handle(sub_keys::shape);
    m_octave = r.handle(sub_keys::octave);    m_level = r.handle(sub_keys::level);
    m_pan = r.handle(sub_keys::pan);
}

SubOscModule::Values SubOscModule::read() const noexcept
{
    return Values{
        m_enabled.load() >= 0.5f,
        static_cast<dsp::SubShape>(std::clamp(asInt(m_shape), 0, static_cast<int>(dsp::SubShape::Count) - 1)),
        m_octave.load() * 12.0f,
        m_level.load(),
        m_pan.load(),
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
