#include "engine/modules/Modules.h"

#include <cmath>
#include <vector>

namespace winerose::modules {

namespace {

// Envelope / LFO times: 0..32 s on a cubic knob (SPEC §5.5 hypothesis t = 32·x³, TODO-MEASURE).
const ParamOpts kTimeOpts { NumericMeta::Curve::Power, 3.0, "s" };

template<std::size_t N>
std::vector<EnumChoice> choices(const char* const (&names)[N])
{
    std::vector<EnumChoice> out;
    for (std::size_t i = 0; i < N; ++i) out.push_back({static_cast<int>(i), names[i]});
    return out;
}

std::vector<EnumChoice> divisionChoices()
{
    std::vector<EnumChoice> out;
    for (int i = 0; i < modulation::kSyncDivisionCount; ++i) out.push_back({i, modulation::kSyncDivisions[i].name});
    return out;
}

int asInt(float v) noexcept { return static_cast<int>(std::lround(v)); }

template<typename E>
E asEnum(float v, E count) noexcept
{
    return static_cast<E>(std::clamp(asInt(v), 0, static_cast<int>(count) - 1));
}

bool asBool(float v) noexcept { return v >= 0.5f; }

std::vector<EnumChoice> filterTypeChoices()
{
    std::vector<EnumChoice> out;
    for (int i = 0; i < static_cast<int>(dsp::FilterType::Count); ++i) out.push_back({i, dsp::kFilterInfo[i].name});
    return out;
}

// Every source gets the same routing pair (SPEC §1.1). By default only oscillator A goes to the filters.
void registerRouting(ParamRegistry& r, Route defaultRoute, const std::string& group)
{
    r.registerEnum (route_keys::route, choices(kRouteNames), static_cast<int>(defaultRoute), group,
                    "Signal destination: Filter (see balance), Main (through FX), Direct (bypass filters/FX), None");
    r.registerFloat(route_keys::balance, 0.0f, 0.0f, 1.0f, group, "Filter balance: 0 = Filter 1, 1 = Filter 2");
    r.registerFloat(route_keys::bus1, 0.0f, 0.0f, 1.0f, group, "Send to FX Bus 1 (pre-filter, post amp envelope)");
    r.registerFloat(route_keys::bus2, 0.0f, 0.0f, 1.0f, group, "Send to FX Bus 2 (pre-filter, post amp envelope)");
}

} // namespace

// --- Wavetable oscillator ----------------------------------------------------------------------------

OscillatorModule::OscillatorModule(std::shared_ptr<ConfigManager> config, int index, ModTargets& t)
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
    r.registerFloat(osc_keys::coarse, 0.0f, -48.0f, 48.0f, g, "Continuous pitch (the usual pitch modulation target)",
                    ParamOpts{.unit = "st"});
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
    r.registerString(osc_keys::remapCurve, dsp::Curve::identity().serialize(), gw,
                     "Remap 1/2 curve: \"x,y,curve;...\" from x=0 to x=1");
    registerRouting(r, index == 0 ? Route::Filter : Route::Main, "Routing");

    m_i.enabled = t.add(r, osc_keys::enabled);       m_i.level = t.add(r, osc_keys::level);
    m_i.pan = t.add(r, osc_keys::pan);               m_i.octave = t.add(r, osc_keys::octave);
    m_i.semi = t.add(r, osc_keys::semi);             m_i.fine = t.add(r, osc_keys::fine);
    m_i.coarse = t.add(r, osc_keys::coarse);         m_i.wtPos = t.add(r, osc_keys::wtPos);
    m_i.wtSmooth = t.add(r, osc_keys::wtSmooth);     m_i.phase = t.add(r, osc_keys::phase);
    m_i.random = t.add(r, osc_keys::random);         m_i.unison = t.add(r, osc_keys::unison);
    m_i.uniDetune = t.add(r, osc_keys::uniDetune);   m_i.uniBlend = t.add(r, osc_keys::uniBlend);
    m_i.uniWidth = t.add(r, osc_keys::uniWidth);     m_i.uniRange = t.add(r, osc_keys::uniRange);
    m_i.uniStack = t.add(r, osc_keys::uniStack);     m_i.uniMode = t.add(r, osc_keys::uniMode);
    m_i.uniSpan = t.add(r, osc_keys::uniSpan);       m_i.uniRandStart = t.add(r, osc_keys::uniRandStart);
    m_i.uniWarp = t.add(r, osc_keys::uniWarp);       m_i.warp1Mode = t.add(r, osc_keys::warp1Mode);
    m_i.warp1Amount = t.add(r, osc_keys::warp1Amount);
    m_i.warp2Mode = t.add(r, osc_keys::warp2Mode);
    m_i.warp2Amount = t.add(r, osc_keys::warp2Amount);
    m_i.route = t.add(r, route_keys::route);
    m_i.balance = t.add(r, route_keys::balance);
    m_i.bus1 = t.add(r, route_keys::bus1);
    m_i.bus2 = t.add(r, route_keys::bus2);
}

OscillatorModule::Values OscillatorModule::read(const float* v) const noexcept
{
    Values o;
    o.enabled      = asBool(v[m_i.enabled]);
    o.level        = v[m_i.level];
    o.pan          = v[m_i.pan];
    o.pitchSemis   = v[m_i.octave] * 12.0f + v[m_i.semi] + v[m_i.coarse] + v[m_i.fine] / 100.0f;
    o.wtPos        = v[m_i.wtPos];
    o.wtSmooth     = asBool(v[m_i.wtSmooth]);
    o.phase        = v[m_i.phase];
    o.random       = v[m_i.random];
    o.unison       = std::clamp(asInt(v[m_i.unison]), 1, dsp::unison::kMaxVoices);
    o.uniDetune    = v[m_i.uniDetune];
    o.uniBlend     = v[m_i.uniBlend];
    o.uniWidth     = v[m_i.uniWidth];
    o.uniRange     = v[m_i.uniRange];
    o.stack        = asEnum(v[m_i.uniStack], dsp::unison::Stack::Count);
    o.mode         = asEnum(v[m_i.uniMode], dsp::unison::Mode::Count);
    o.uniSpan      = v[m_i.uniSpan];
    o.uniRandStart = v[m_i.uniRandStart];
    o.uniWarp      = v[m_i.uniWarp];
    o.warp1        = asEnum(v[m_i.warp1Mode], dsp::WarpMode::Count);
    o.warp1Amount  = v[m_i.warp1Amount];
    o.warp2        = asEnum(v[m_i.warp2Mode], dsp::WarpMode::Count);
    o.warp2Amount  = v[m_i.warp2Amount];
    o.route        = asEnum(v[m_i.route], Route::Count);
    o.filterBalance = v[m_i.balance];
    o.bus1Send     = v[m_i.bus1];
    o.bus2Send     = v[m_i.bus2];
    return o;
}

std::string OscillatorModule::remapCurve() const
{
    return m_registry->get<std::string>(osc_keys::remapCurve);
}

// --- Noise -------------------------------------------------------------------------------------------

NoiseModule::NoiseModule(std::shared_ptr<ConfigManager> config, ModTargets& t)
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
    registerRouting(r, Route::Main, "Routing");
    m_enabled = t.add(r, noise_keys::enabled);   m_type = t.add(r, noise_keys::type);
    m_level = t.add(r, noise_keys::level);       m_pan = t.add(r, noise_keys::pan);
    m_keytrack = t.add(r, noise_keys::keytrack); m_pitch = t.add(r, noise_keys::pitch);
    m_route = t.add(r, route_keys::route);       m_balance = t.add(r, route_keys::balance);
    m_bus1 = t.add(r, route_keys::bus1);         m_bus2 = t.add(r, route_keys::bus2);
}

NoiseModule::Values NoiseModule::read(const float* v) const noexcept
{
    return Values{asBool(v[m_enabled]), asEnum(v[m_type], dsp::NoiseTables::Type::Count), v[m_level], v[m_pan],
                  asBool(v[m_keytrack]), v[m_pitch], asEnum(v[m_route], Route::Count), v[m_balance], v[m_bus1], v[m_bus2]};
}

// --- Sub ---------------------------------------------------------------------------------------------

SubOscModule::SubOscModule(std::shared_ptr<ConfigManager> config, ModTargets& t)
    : m_registry(std::make_unique<ParamRegistry>(std::move(config), "Oscillator" + std::to_string(kIndex)))
{
    auto& r = *m_registry;
    const std::string g = "Sub";
    r.registerBool (sub_keys::enabled, false, g, "Sub oscillator on/off");
    r.registerEnum (sub_keys::shape, choices(dsp::kSubShapeNames), 0, g, "Shape (TODO-MEASURE order)");
    r.registerInt  (sub_keys::octave, 0, -4, 4, g, "Octave (TODO-MEASURE default)");
    r.registerFloat(sub_keys::level, 0.75f, 0.0f, 1.0f, g, "Level");
    r.registerFloat(sub_keys::pan, 0.0f, -1.0f, 1.0f, g, "Pan");
    registerRouting(r, Route::Main, "Routing");
    m_enabled = t.add(r, sub_keys::enabled);  m_shape = t.add(r, sub_keys::shape);
    m_octave = t.add(r, sub_keys::octave);    m_level = t.add(r, sub_keys::level);
    m_pan = t.add(r, sub_keys::pan);
    m_route = t.add(r, route_keys::route);    m_balance = t.add(r, route_keys::balance);
    m_bus1 = t.add(r, route_keys::bus1);      m_bus2 = t.add(r, route_keys::bus2);
}

SubOscModule::Values SubOscModule::read(const float* v) const noexcept
{
    return Values{asBool(v[m_enabled]), asEnum(v[m_shape], dsp::SubShape::Count), v[m_octave] * 12.0f, v[m_level], v[m_pan],
                  asEnum(v[m_route], Route::Count), v[m_balance], v[m_bus1], v[m_bus2]};
}

// --- Filter ------------------------------------------------------------------------------------------

FilterModule::FilterModule(std::shared_ptr<ConfigManager> config, int index, ModTargets& t)
    : m_registry(std::make_unique<ParamRegistry>(std::move(config), "Filter" + std::to_string(index)))
{
    auto& r = *m_registry;
    const std::string g = "Filter";
    r.registerBool (filter_keys::enabled, false, g, "Filter on/off; off passes the routed signal through (SPEC 1.1)");
    r.registerEnum (filter_keys::type, filterTypeChoices(), static_cast<int>(dsp::FilterType::Lp12), g,
                    "Filter type (Winerose's own list; Serum types are mapped by the preset importer)");
    r.registerFloat(filter_keys::cutoff, 425.0f, 8.18f, 22050.0f, g,
                    "Cutoff (exponential knob, INFERRED range 8.18 Hz - 22.05 kHz)",
                    ParamOpts{NumericMeta::Curve::Exp, 1.0, "Hz"});
    r.registerFloat(filter_keys::resonance, 0.1f, 0.0f, 1.0f, g, "Resonance (TODO-MEASURE default)");
    r.registerFloat(filter_keys::drive, 0.0f, 0.0f, 1.0f, g, "Drive into the filter (0..+24 dB with saturation)");
    r.registerBool (filter_keys::clean, false, g, "Clean drive: level into the filter without the input saturator");
    r.registerFloat(filter_keys::var, 0.5f, 0.0f, 1.0f, g, "Var: morph / second cutoff / damping / vowel, per type");
    r.registerFloat(filter_keys::x, 0.0f, 0.0f, 1.0f, g, "PZ Morph X: low-pass to band-pass to high-pass");
    r.registerFloat(filter_keys::y, 0.5f, 0.0f, 1.0f, g, "PZ Morph Y: notch / neutral / peak");
    r.registerFloat(filter_keys::stereo, 0.0f, 0.0f, 1.0f, g, "L/R cutoff spread (up to +/- half an octave, INFERRED)");
    r.registerFloat(filter_keys::mix, 1.0f, 0.0f, 1.0f, g, "Dry/wet");
    r.registerFloat(filter_keys::level, 1.0f, 0.0f, 2.0f, g, "Output level (linear)");
    r.registerFloat(filter_keys::keytrack, 0.0f, 0.0f, 1.0f, g, "Keytrack: 100% = one octave of cutoff per octave");
    r.registerEnum (filter_keys::output, choices(kFilterOutputNames), 0, g, "Output: Main (through FX) or Direct");
    m_enabled = t.add(r, filter_keys::enabled);     m_type = t.add(r, filter_keys::type);
    m_cutoff = t.add(r, filter_keys::cutoff);       m_resonance = t.add(r, filter_keys::resonance);
    m_drive = t.add(r, filter_keys::drive);         m_clean = t.add(r, filter_keys::clean);
    m_var = t.add(r, filter_keys::var);             m_x = t.add(r, filter_keys::x);
    m_y = t.add(r, filter_keys::y);                 m_stereo = t.add(r, filter_keys::stereo);
    m_mix = t.add(r, filter_keys::mix);             m_level = t.add(r, filter_keys::level);
    m_keytrack = t.add(r, filter_keys::keytrack);   m_output = t.add(r, filter_keys::output);
}

FilterModule::Values FilterModule::read(const float* v) const noexcept
{
    Values out;
    out.enabled = asBool(v[m_enabled]);
    auto& s = out.settings;
    s.type      = asEnum(v[m_type], dsp::FilterType::Count);
    s.cutoffHz  = v[m_cutoff];
    s.resonance = v[m_resonance];
    s.drive     = v[m_drive];
    s.clean     = asBool(v[m_clean]);
    s.var       = v[m_var];
    s.x         = v[m_x];
    s.y         = v[m_y];
    s.stereo    = v[m_stereo];
    s.mix       = v[m_mix];
    s.level     = v[m_level];
    out.keytrack = v[m_keytrack];
    out.output   = asEnum(v[m_output], FilterOutput::Count);
    return out;
}

// --- Routing -----------------------------------------------------------------------------------------

RoutingModule::RoutingModule(std::shared_ptr<ConfigManager> config, ModTargets& t)
    : m_registry(std::make_unique<ParamRegistry>(std::move(config), "Routing"))
{
    m_registry->registerEnum("filterRouting", choices(kFilterRoutingNames), 0, "Routing",
                             "Serial: Filter 1 feeds Filter 2. Parallel: independent (TODO-MEASURE default)");
    m_filterRouting = t.add(*m_registry, "filterRouting");
}

FilterRouting RoutingModule::read(const float* v) const noexcept
{
    return asEnum(v[m_filterRouting], FilterRouting::Count);
}

// --- Envelope ----------------------------------------------------------------------------------------

EnvelopeModule::EnvelopeModule(std::shared_ptr<ConfigManager> config, int index, ModTargets& t)
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
    r.registerFloat(env_keys::attackCurve, d.attackCurve, -10.0f, 10.0f, g, "Attack curve (0 = linear)");
    r.registerFloat(env_keys::decayCurve, d.decayCurve, -10.0f, 10.0f, g, "Decay curve (negative = fast start)");
    r.registerFloat(env_keys::releaseCurve, d.releaseCurve, -10.0f, 10.0f, g, "Release curve (negative = fast start)");
    m_attack = t.add(r, env_keys::attack);     m_hold = t.add(r, env_keys::hold);
    m_decay = t.add(r, env_keys::decay);       m_sustain = t.add(r, env_keys::sustain);
    m_release = t.add(r, env_keys::release);   m_attackCurve = t.add(r, env_keys::attackCurve);
    m_decayCurve = t.add(r, env_keys::decayCurve);
    m_releaseCurve = t.add(r, env_keys::releaseCurve);
}

dsp::Envelope::Settings EnvelopeModule::read(const float* v) const noexcept
{
    dsp::Envelope::Settings s;
    s.attackSeconds  = v[m_attack];
    s.holdSeconds    = v[m_hold];
    s.decaySeconds   = v[m_decay];
    s.sustain        = v[m_sustain];
    s.releaseSeconds = v[m_release];
    s.attackCurve    = v[m_attackCurve];
    s.decayCurve     = v[m_decayCurve];
    s.releaseCurve   = v[m_releaseCurve];
    return s;
}

// --- LFO ---------------------------------------------------------------------------------------------

LfoModule::LfoModule(std::shared_ptr<ConfigManager> config, int index, ModTargets& t)
    : m_registry(std::make_unique<ParamRegistry>(std::move(config), "LFO" + std::to_string(index)))
{
    auto& r = *m_registry;
    const std::string g = "LFO";
    r.registerEnum (lfo_keys::shape, choices(modulation::kLfoShapeNames), 0, g, "Shape (Path = the drawable curve)");
    r.registerString(lfo_keys::path, dsp::Curve::triangle().serialize(), g, "Drawable shape: \"x,y,curve;...\"");
    r.registerEnum (lfo_keys::mode, choices(modulation::kLfoModeNames), static_cast<int>(modulation::LfoMode::Trig), g,
                    "Free: shared free-running phase; Trig: restart per note; Env: one-shot (TODO-MEASURE default)");
    r.registerBool (lfo_keys::sync, true, g, "Tempo sync (TODO-MEASURE default)");
    r.registerEnum (lfo_keys::division, divisionChoices(), modulation::kDefaultSyncDivision, g, "Synced rate");
    r.registerFloat(lfo_keys::rate, 1.0f, 0.01f, 1000.0f, g, "Free rate (up to audio rate)",
                    ParamOpts{NumericMeta::Curve::Exp, 1.0, "Hz"});
    r.registerFloat(lfo_keys::phase, 0.0f, 0.0f, 1.0f, g, "Start phase (Trig / Env)");
    r.registerFloat(lfo_keys::delay, 0.0f, 0.0f, 32.0f, g, "Delay before the LFO starts", kTimeOpts);
    r.registerFloat(lfo_keys::rise, 0.0f, 0.0f, 32.0f, g, "Fade-in time after the delay", kTimeOpts);
    r.registerFloat(lfo_keys::smooth, 0.0f, 0.0f, 1.0f, g, "Output smoothing (0-100 ms, INFERRED)");
    m_shape = t.add(r, lfo_keys::shape);       m_mode = t.add(r, lfo_keys::mode);
    m_sync = t.add(r, lfo_keys::sync);         m_division = t.add(r, lfo_keys::division);
    m_rate = t.add(r, lfo_keys::rate);         m_phase = t.add(r, lfo_keys::phase);
    m_delay = t.add(r, lfo_keys::delay);       m_rise = t.add(r, lfo_keys::rise);
    m_smooth = t.add(r, lfo_keys::smooth);
}

modulation::LfoSettings LfoModule::read(const float* v) const noexcept
{
    modulation::LfoSettings s;
    s.shape        = asEnum(v[m_shape], modulation::LfoShape::Count);
    s.mode         = asEnum(v[m_mode], modulation::LfoMode::Count);
    s.sync         = asBool(v[m_sync]);
    s.division     = std::clamp(asInt(v[m_division]), 0, modulation::kSyncDivisionCount - 1);
    s.rateHz       = v[m_rate];
    s.phase        = v[m_phase];
    s.delaySeconds = v[m_delay];
    s.riseSeconds  = v[m_rise];
    s.smooth       = v[m_smooth];
    return s;
}

std::string LfoModule::path() const
{
    return m_registry->get<std::string>(lfo_keys::path);
}

// --- Macro -------------------------------------------------------------------------------------------

MacroModule::MacroModule(std::shared_ptr<ConfigManager> config, int index, ModTargets& t)
    : m_registry(std::make_unique<ParamRegistry>(std::move(config), "Macro" + std::to_string(index)))
{
    auto& r = *m_registry;
    r.registerFloat(macro_keys::value, 0.0f, 0.0f, 1.0f, "Macro", "Macro value (also a modulation destination)");
    r.registerString(macro_keys::name, "Macro " + std::to_string(index + 1), "Macro", "Display name");
    m_value = t.add(r, macro_keys::value);
}

// --- Mod matrix --------------------------------------------------------------------------------------

MatrixModule::MatrixModule(std::shared_ptr<ConfigManager> config)
{
    const ParamOpts fixed { .automatable = false };
    for (int k = 0; k < modulation::kSlotCount; ++k) {
        auto reg = std::make_unique<ParamRegistry>(config, "ModSlot" + std::to_string(k));
        auto& r = *reg;
        const std::string g = "Matrix";
        // Topology (source, destination, switches) is patch state, not automation (SPEC §5.3); only the
        // amount is host-automatable, like Serum 2's "Mod N" parameters.
        r.registerEnum  (slot_keys::source, choices(modulation::kSourceNames), 0, g, "Source", fixed);
        r.registerString(slot_keys::destination, "", g, "Destination parameter, e.g. \"Filter0.cutoff\"");
        r.registerFloat (slot_keys::amount, 0.0f, -1.0f, 1.0f, g, "Amount (fraction of the destination's range)");
        r.registerBool  (slot_keys::bipolar, false, g, "Bipolar: source 0..1 becomes -1..1", fixed);
        r.registerFloat (slot_keys::curve, 0.0f, -1.0f, 1.0f, g, "Source curve (0 = linear)", fixed);
        r.registerEnum  (slot_keys::aux, choices(modulation::kSourceNames), 0, g, "Aux source (multiplies the output)", fixed);
        r.registerFloat (slot_keys::auxAmount, 1.0f, 0.0f, 1.0f, g, "Aux depth", fixed);
        r.registerBool  (slot_keys::auxInvert, false, g, "Use 1 - aux", fixed);
        r.registerFloat (slot_keys::output, 1.0f, 0.0f, 1.0f, g, "Output scale", fixed);
        r.registerBool  (slot_keys::bypass, false, g, "Bypass this slot", fixed);

        auto& h = m_handles[static_cast<std::size_t>(k)];
        h.source = r.handle(slot_keys::source);       h.amount = r.handle(slot_keys::amount);
        h.bipolar = r.handle(slot_keys::bipolar);     h.curve = r.handle(slot_keys::curve);
        h.aux = r.handle(slot_keys::aux);             h.auxAmount = r.handle(slot_keys::auxAmount);
        h.auxInvert = r.handle(slot_keys::auxInvert); h.output = r.handle(slot_keys::output);
        h.bypass = r.handle(slot_keys::bypass);
        m_slots[static_cast<std::size_t>(k)] = std::move(reg);
    }
}

void MatrixModule::read(std::array<SlotParams, modulation::kSlotCount>& out) const noexcept
{
    for (std::size_t k = 0; k < out.size(); ++k) {
        const auto& h = m_handles[k];
        auto& s = out[k];
        s.source    = asEnum(h.source.load(), modulation::Source::Count);
        s.aux       = asEnum(h.aux.load(), modulation::Source::Count);
        s.amount    = h.amount.load();
        s.curve     = h.curve.load();
        s.auxAmount = h.auxAmount.load();
        s.output    = h.output.load();
        s.bipolar   = asBool(h.bipolar.load());
        s.auxInvert = asBool(h.auxInvert.load());
        s.bypass    = asBool(h.bypass.load());
    }
}

std::string MatrixModule::destination(int slot) const
{
    return m_slots[static_cast<std::size_t>(slot)]->get<std::string>(slot_keys::destination);
}

} // namespace winerose::modules
