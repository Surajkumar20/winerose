#include "engine/voice/Voice.h"

#include "hiir/PolyphaseIir2Designer.h"

#include <algorithm>
#include <cmath>

namespace winerose::voice {

namespace {

constexpr float kQuarterPi = 0.78539816339744831f;
constexpr float kSqrt2     = 1.41421356237309505f;

// Paired oscillator for FM/AM/RM warps: A←B, B←A, C←A (SPEC §1.2: Serum's "from B" / "from A").
constexpr int kPairedOsc[kOscCount] = {1, 0, 0};

double g_downCoefs[kDownsamplerCoefs] {};

// Constant-power pan, scaled so the centre is unity gain on both sides.
void panGains(float pan, float& left, float& right) noexcept
{
    const float theta = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * kQuarterPi;
    left  = std::cos(theta) * kSqrt2;
    right = std::sin(theta) * kSqrt2;
}

double hzForSemis(double semisFromA4) noexcept { return 440.0 * std::exp2(semisFromA4 / 12.0); }

float warpInputValue(dsp::WarpMode m, float paired, float noise, float sub) noexcept
{
    switch (dsp::warpInput(m)) {
        case dsp::WarpInput::PairedOsc: return paired;
        case dsp::WarpInput::Noise:     return noise;
        case dsp::WarpInput::Sub:       return sub;
        case dsp::WarpInput::None:      break;
    }
    return 0.0f;
}

// Slot source shaping: power-law curve (curve > 0 bends up: x^(2^(-3c))), then optional bipolar mapping.
float shapeSource(float x, float curve, bool bipolar) noexcept
{
    x = std::clamp(x, 0.0f, 1.0f);
    if (curve != 0.0f) x = std::pow(x, std::exp2(-3.0f * curve));
    return bipolar ? 2.0f * x - 1.0f : x;
}

} // namespace

Voice::Voice() : m_gen(std::make_unique<std::array<Generator, kOscCount>>()) {}
Voice::~Voice() = default;
Voice::Voice(Voice&&) noexcept = default;
Voice& Voice::operator=(Voice&&) noexcept = default;

void Voice::initDownsamplerCoefs() noexcept
{
    dsp::sinc::init();
    dsp::granular::init();
    dsp::spectral::init();
    // 12 coefficients, transition band 0.04 of the half-band: > 100 dB stopband (HIIR designer).
    hiir::PolyphaseIir2Designer::compute_coefs_spec_order_tbw(g_downCoefs, kDownsamplerCoefs, 0.04);
}

void Voice::prepare(double sampleRate) noexcept
{
    m_sampleRate = sampleRate;
    for (auto& e : m_env) e.setSampleRate(sampleRate);
    for (auto& o : m_osc) {
        o.wtPos.setRampSamples(kControlBlock);
        o.level.setRampSamples(kControlBlock);
    }
    m_subLevel.setRampSamples(kControlBlock);
    m_noiseLevel.setRampSamples(kControlBlock);
    for (auto& d : m_down) d.set_coefs(g_downCoefs);
    resetDownsamplers();
    for (auto& e : m_env) e.reset();
    for (auto& f : m_filters) f.prepare(sampleRate);
    m_released = true;
    m_sustained = false;
    m_note = -1;
}

void Voice::resetDownsamplers() noexcept
{
    for (auto& d : m_down) d.clear_buffers();
}

void Voice::release() noexcept
{
    for (auto& e : m_env) e.noteOff();
    m_released = true;
    for (auto& g : *m_gen)
        for (int p = 0; p < g.playerCount; ++p) g.players[static_cast<std::size_t>(p)].released = true;
    m_releasePending = true;
}

void Voice::kill() noexcept
{
    for (auto& e : m_env) e.reset();
    m_released = true;
}

// --- Modulation ---------------------------------------------------------------------------------------

float Voice::sourceAtTick(modulation::Source s) const noexcept
{
    using modulation::Source;
    if (modulation::isEnv(s))   return m_env[static_cast<std::size_t>(modulation::envIndex(s))].level();
    if (modulation::isLfo(s))   return m_lfo[static_cast<std::size_t>(modulation::lfoIndex(s))].value();
    if (modulation::isMacro(s)) return m_macro[static_cast<std::size_t>(modulation::macroIndex(s))];
    switch (s) {
        case Source::Velocity:    return m_velocity;
        case Source::Note:        return m_noteNorm;
        case Source::ModWheel:    return m_global.modWheel;
        case Source::PitchBend:   return m_global.pitchBend;
        case Source::Aftertouch:  return m_global.aftertouch;
        case Source::NoteRandom1: return m_random1;
        case Source::NoteRandom2: return m_random2;
        case Source::Fixed:       return 1.0f;
        default:                  return 0.0f;
    }
}

float Voice::sourceValue(modulation::Source s) const noexcept
{
    return sourceAtTick(s);
}

void Voice::evaluateModulation(const ControlContext& ctx) noexcept
{
    const auto& mods = *ctx.modules;
    const int n = mods.targets.count();
    std::copy(ctx.base, ctx.base + n, m_plain.begin());
    m_global = ctx.global;
    m_fastCount = 0;
    m_lfoUsed.fill(false);
    m_lfoFast.fill(false);
    for (int m = 0; m < modulation::kMacroCount; ++m)
        m_macro[static_cast<std::size_t>(m)] = ctx.base[mods.macroTarget[static_cast<std::size_t>(m)]];

    const auto& slots = *ctx.slots;
    const auto& dests = *ctx.slotDest;
    auto isMacroTarget = [&](int d) {
        for (int t : mods.macroTarget) if (t == d) return true;
        return false;
    };
    auto auxFactor = [&](const modules::SlotParams& s) {
        if (s.aux == modulation::Source::None) return 1.0f;
        if (modulation::isLfo(s.aux)) m_lfoUsed[static_cast<std::size_t>(modulation::lfoIndex(s.aux))] = true;
        float a = std::clamp(sourceAtTick(s.aux), 0.0f, 1.0f);
        if (s.auxInvert) a = 1.0f - a;
        return 1.0f - s.auxAmount + s.auxAmount * a;
    };

    int touched = 0;
    auto accumulate = [&](int d, float value) {
        if (m_delta[static_cast<std::size_t>(d)] == 0.0f) {
            bool seen = false;
            for (int i = 0; i < touched; ++i) seen |= (m_touched[static_cast<std::size_t>(i)] == d);
            if (!seen && touched < modulation::kSlotCount) m_touched[static_cast<std::size_t>(touched++)] = static_cast<std::int16_t>(d);
        }
        m_delta[static_cast<std::size_t>(d)] += value;
    };
    auto applyDeltas = [&](int from) {
        for (int i = from; i < touched; ++i) {
            const int d = m_touched[static_cast<std::size_t>(i)];
            const auto& meta = mods.targets.at(d).meta;
            const double norm = toNormalized(meta, ctx.base[d]) + m_delta[static_cast<std::size_t>(d)];
            m_plain[static_cast<std::size_t>(d)] = static_cast<float>(fromNormalized(meta, std::clamp(norm, 0.0, 1.0)));
            m_delta[static_cast<std::size_t>(d)] = 0.0f;
        }
    };
    auto active = [&](int k) {
        const auto& s = slots[static_cast<std::size_t>(k)];
        return dests[static_cast<std::size_t>(k)] >= 0 && !s.bypass && s.source != modulation::Source::None && s.amount != 0.0f;
    };

    // Pass A: slots targeting macros (macros are destinations as well as sources; a macro feeding a macro
    // reads the unmodulated value, so the result doesn't depend on slot order).
    for (int k = 0; k < modulation::kSlotCount; ++k) {
        const int d = dests[static_cast<std::size_t>(k)];
        if (!active(k) || !isMacroTarget(d)) continue;
        const auto& s = slots[static_cast<std::size_t>(k)];
        if (modulation::isLfo(s.source)) m_lfoUsed[static_cast<std::size_t>(modulation::lfoIndex(s.source))] = true;
        accumulate(d, shapeSource(sourceAtTick(s.source), s.curve, s.bipolar) * s.amount * auxFactor(s) * s.output);
    }
    applyDeltas(0);
    const int macroTouched = touched;
    for (int m = 0; m < modulation::kMacroCount; ++m)
        m_macro[static_cast<std::size_t>(m)] = m_plain[static_cast<std::size_t>(mods.macroTarget[static_cast<std::size_t>(m)])];

    // Pass B: everything else. LFO → fast destination slots go to the per-sample path.
    for (int k = 0; k < modulation::kSlotCount; ++k) {
        const int d = dests[static_cast<std::size_t>(k)];
        if (!active(k) || isMacroTarget(d)) continue;
        const auto& s = slots[static_cast<std::size_t>(k)];
        if (modulation::isLfo(s.source)) {
            const int l = modulation::lfoIndex(s.source);
            m_lfoUsed[static_cast<std::size_t>(l)] = true;
            const auto shape = static_cast<modulation::LfoShape>(std::lround(
                ctx.base[mods.lfo[static_cast<std::size_t>(l)]->shapeIndex()]));
            const auto& fd = mods.fastDest[static_cast<std::size_t>(d)];
            if (fd.kind != modules::FastDest::Kind::None && !modulation::isChaos(shape) && m_fastCount < modulation::kSlotCount) {
                auto& f = m_fast[static_cast<std::size_t>(m_fastCount++)];
                f.lfo = l;
                f.scale = s.amount * auxFactor(s) * s.output;
                f.curve = s.curve;
                f.bipolar = s.bipolar;
                f.dest = fd;
                m_lfoFast[static_cast<std::size_t>(l)] = true;
                continue;
            }
        }
        accumulate(d, shapeSource(sourceAtTick(s.source), s.curve, s.bipolar) * s.amount * auxFactor(s) * s.output);
    }
    applyDeltas(macroTouched);

    mods.build(m_plain.data(), m_sampleRate, m_settings);
    m_settings.control.oversample = ctx.oversample;
    m_settings.control.bendSemis  = ctx.global.bendSemis;
    for (int f = 0; f < modules::FilterModule::kCount; ++f) {
        const auto& meta = mods.targets.at(mods.filter[static_cast<std::size_t>(f)]->cutoffIndex()).meta;
        m_cutoffNorm[static_cast<std::size_t>(f)] = static_cast<float>(
            toNormalized(meta, m_settings.control.filter[static_cast<std::size_t>(f)].settings.cutoffHz));
    }
}

void Voice::configureModSources(const ControlContext& ctx) noexcept
{
    for (int e = 0; e < modulation::kEnvCount; ++e)
        m_env[static_cast<std::size_t>(e)].setSettings(m_settings.env[static_cast<std::size_t>(e)]);
    for (int l = 0; l < modulation::kLfoCount; ++l)
        m_lfo[static_cast<std::size_t>(l)].configure(m_settings.lfo[static_cast<std::size_t>(l)], m_sampleRate, ctx.global.bpm);
}

// --- Note lifecycle -----------------------------------------------------------------------------------

void Voice::start(int note, int velocity, std::uint64_t order, const ControlContext& ctx, Rng& rng) noexcept
{
    m_note = note;
    m_order = order;
    m_released = false;
    m_sustained = false;
    m_modules = ctx.modules;
    const auto& mods = *ctx.modules;

    // Randomness is drawn in a fixed order and amount per note (independent of settings), so renders stay
    // deterministic even when parameters change between notes. Oscillator phases first (unchanged since
    // Phase 2, keeping its golden renders), then the per-note mod sources.
    for (int o = 0; o < kOscCount; ++o) {
        const auto v = mods.osc[static_cast<std::size_t>(o)]->read(ctx.base);
        const bool mem = v.phase >= modules::OscillatorModule::kPhaseMem;
        const double shared = rng.next();
        for (auto& u : m_osc[static_cast<std::size_t>(o)].voices) {
            const double independent = rng.next();
            u.randomDetune = static_cast<float>(rng.next() * 2.0 - 1.0);
            if (mem) continue;   // "Mem": keep running from where this voice slot left off
            const double r = shared + (independent - shared) * v.uniRandStart;
            const double p = v.phase + v.random * r;
            u.phase = p - std::floor(p);
        }
        m_osc[static_cast<std::size_t>(o)].last = 0.0f;
    }
    m_subPhase = 0.0;
    m_noisePos = rng.next() * dsp::NoiseTables::kLength;
    m_random1 = static_cast<float>(rng.next());
    m_random2 = static_cast<float>(rng.next());
    m_velocity = static_cast<float>(std::clamp(velocity, 0, 127)) / 127.0f;
    m_velocityRaw = std::clamp(velocity, 0, 127);
    m_noteNorm = static_cast<float>(std::clamp(note, 0, 127)) / 127.0f;
    for (auto& g : *m_gen) g.started = false;   // generators start at the first render (they need the tables)
    m_releasePending = false;

    // Mod sources start from their unmodulated settings so the first evaluation sees t = 0 values.
    for (int l = 0; l < modulation::kLfoCount; ++l) {
        auto& lfo = m_lfo[static_cast<std::size_t>(l)];
        const auto s = mods.lfo[static_cast<std::size_t>(l)]->read(ctx.base);
        lfo.start(s, ctx.lfoFreePhase[static_cast<std::size_t>(l)], rng.nextSeed());
        lfo.configure(s, m_sampleRate, ctx.global.bpm);
        lfo.advance(0, ctx.tables.lfo(l));
    }
    for (int e = 0; e < modulation::kEnvCount; ++e) {
        auto& env = m_env[static_cast<std::size_t>(e)];
        env.setSettings(mods.env[static_cast<std::size_t>(e)]->read(ctx.base));
        env.reset();
        env.noteOn();
    }

    evaluateModulation(ctx);
    configureModSources(ctx);

    for (auto& f : m_filters) f.reset();
    resetDownsamplers();
    m_oversample = ctx.oversample;
    layout(m_settings.control);

    // Fresh voice: no ramps — start exactly at the current control values.
    for (int o = 0; o < kOscCount; ++o) {
        auto& osc = m_osc[static_cast<std::size_t>(o)];
        osc.wtPos.reset(m_settings.control.osc[static_cast<std::size_t>(o)].wtPos);
        osc.level.reset(m_settings.control.osc[static_cast<std::size_t>(o)].level);
    }
    m_subLevel.reset(m_settings.control.sub.level);
    m_noiseLevel.reset(m_settings.control.noise.level);
}

void Voice::steal(int note, int velocity, std::uint64_t order, const ControlContext& ctx) noexcept
{
    m_note = note;
    m_order = order;
    m_released = false;
    m_sustained = false;
    m_velocity = static_cast<float>(std::clamp(velocity, 0, 127)) / 127.0f;
    m_velocityRaw = std::clamp(velocity, 0, 127);
    m_noteNorm = static_cast<float>(std::clamp(note, 0, 127)) / 127.0f;
    for (auto& g : *m_gen) g.started = false;   // a stolen voice restarts its sample sources for the new note
    m_releasePending = false;
    evaluateModulation(ctx);
    configureModSources(ctx);
    layout(m_settings.control);
    for (auto& e : m_env) e.noteOn();   // attack from the current level
}

void Voice::control(const ControlContext& ctx) noexcept
{
    if (ctx.oversample != m_oversample) {
        resetDownsamplers();
        m_oversample = ctx.oversample;
    }
    evaluateModulation(ctx);
    configureModSources(ctx);
    layout(m_settings.control);
    const auto& c = m_settings.control;
    for (int o = 0; o < kOscCount; ++o) {
        auto& osc = m_osc[static_cast<std::size_t>(o)];
        osc.wtPos.setTarget(c.osc[static_cast<std::size_t>(o)].wtPos);
        osc.level.setTarget(c.osc[static_cast<std::size_t>(o)].level);
    }
    m_subLevel.setTarget(c.sub.level);
    m_noiseLevel.setTarget(c.noise.level);
}

void Voice::layout(const VoiceControl& control) noexcept
{
    const double os = static_cast<double>(m_oversample);
    const double sr = control.sampleRate;

    for (int o = 0; o < kOscCount; ++o) {
        const auto& v = control.osc[static_cast<std::size_t>(o)];
        auto& osc = m_osc[static_cast<std::size_t>(o)];
        osc.on     = v.enabled;
        osc.count  = v.unison;
        osc.smooth = v.wtSmooth;
        osc.span   = v.uniSpan;
        osc.warp1  = v.warp1;
        osc.warp2  = v.warp2;
        osc.type   = v.type;
        osc.mapped = m_note >= v.keyLo && m_note <= v.keyHi && m_velocityRaw >= v.velLo && m_velocityRaw <= v.velHi;
        if (!osc.on) continue;
        if (osc.type != modules::OscType::Wavetable) {
            // Sample-based sources: no unison/warps; level, pan and pitch apply.
            panGains(v.pan, osc.genGainL, osc.genGainR);
            osc.genOffsetSemis = v.pitchSemis + control.bendSemis;
            osc.smpLoop = v.smpLoop;
            osc.smpStart = v.smpStart;
            osc.smpEnd = v.smpEnd;
            osc.smpLoopStart = v.smpLoopStart;
            osc.smpLoopEnd = v.smpLoopEnd;
            osc.smpXfade = v.smpXfade;
            osc.smpFileLoop = v.smpFileLoop;
            auto& g = osc.grn;
            g.position = v.grnPos;
            g.scan = v.grnScan;
            g.sizeMs = v.grnSize;
            g.density = v.grnDensity;
            g.posRandom = v.grnPosRand;
            g.pitchRandom = v.grnPitchRand;
            g.panRandom = v.grnPanRand;
            g.window = v.grnWindow;
            g.windowAmount = v.grnWindowAmt;
            g.sampleRate = sr;
            auto& sp = osc.spc;
            sp.position = v.spcPos;
            sp.scan = v.spcScan;
            sp.timbreSemis = v.spcTimbre;
            sp.formant = v.spcFormant;
            sp.lowCutHz = v.spcLowCut;
            sp.highCutHz = std::max(v.spcHighCut, v.spcLowCut);
            sp.transients = v.spcTransients;
            sp.sampleRate = sr;
            continue;
        }

        float sumSquares = 0.0f;
        for (int u = 0; u < osc.count; ++u) {
            const float w = dsp::unison::blendWeight(v.uniBlend, u, osc.count);
            sumSquares += w * w;
        }
        const float norm = sumSquares > 0.0f ? 1.0f / std::sqrt(sumSquares) : 0.0f;

        for (int u = 0; u < osc.count; ++u) {
            auto& uv = osc.voices[static_cast<std::size_t>(u)];
            const float t = dsp::unison::position(u, osc.count);
            const float shape = v.mode == dsp::unison::Mode::Random ? uv.randomDetune : dsp::unison::curve(v.mode, t);
            const double semis = static_cast<double>(m_note) - 69.0 + v.pitchSemis + control.bendSemis
                               + dsp::unison::stackSemis(v.stack, u, osc.count)
                               + static_cast<double>(shape * v.uniDetune * v.uniRange);
            const double incBase = std::min(hzForSemis(semis) / sr, 0.5);
            uv.inc    = incBase / os;
            uv.spread = t;
            uv.warp1  = std::clamp(v.warp1Amount + v.uniWarp * t * 0.5f, 0.0f, 1.0f);
            uv.warp2  = std::clamp(v.warp2Amount + v.uniWarp * t * 0.5f, 0.0f, 1.0f);
            const double pitchFactor = dsp::warpPitchFactor(v.warp1, uv.warp1) * dsp::warpPitchFactor(v.warp2, uv.warp2);
            uv.levels = dsp::WavetableBank::selectLevel(uv.inc * pitchFactor);

            float gl, gr;
            panGains(v.pan + v.uniWidth * t, gl, gr);
            const float w = dsp::unison::blendWeight(v.uniBlend, u, osc.count) * norm;
            uv.gainL = gl * w;
            uv.gainR = gr * w;
        }
    }

    // Which non-audible sources the warps still need as modulators.
    bool warpNeedsSub = false, warpNeedsNoise = false;
    for (const auto& v : control.osc) {
        if (!v.enabled || v.type != modules::OscType::Wavetable) continue;
        for (auto m : {v.warp1, v.warp2}) {
            warpNeedsSub   |= dsp::warpInput(m) == dsp::WarpInput::Sub;
            warpNeedsNoise |= dsp::warpInput(m) == dsp::WarpInput::Noise;
        }
    }

    // Sub
    m_subOn    = control.sub.enabled;
    m_subNeeded = m_subOn || warpNeedsSub;
    m_subShape = static_cast<int>(control.sub.shape);
    m_subInc   = std::min(hzForSemis(static_cast<double>(m_note) - 69.0 + control.sub.pitchSemis + control.bendSemis) / sr, 0.5) / os;
    m_subLevels = dsp::WavetableBank::selectLevel(m_subInc);
    panGains(control.sub.pan, m_subGainL, m_subGainR);

    // Noise: rate in table samples per output sample (the tables are "recorded" at the engine rate).
    m_noiseOn   = control.noise.enabled;
    m_noiseNeeded = m_noiseOn || warpNeedsNoise;
    m_noiseType = control.noise.type;
    const double noiseSemis = (control.noise.keytrack ? static_cast<double>(m_note) - 60.0 : 0.0) + control.noise.pitchSemis;
    m_noiseRate = std::exp2(noiseSemis / 12.0) / os;
    panGains(control.noise.pan, m_noiseGainL, m_noiseGainR);

    // Routing weights per source and bus.
    auto routeOf = [&](int src) -> std::pair<modules::Route, float> {
        if (src < kOscCount) return {control.osc[static_cast<std::size_t>(src)].route, control.osc[static_cast<std::size_t>(src)].filterBalance};
        if (src == kNoiseSource) return {control.noise.route, control.noise.filterBalance};
        return {control.sub.route, control.sub.filterBalance};
    };
    // Where each filter bus really ends up when its filter is bypassed (disabled filters pass through):
    // serial F1 → F2's input; otherwise straight to the filter's output target.
    const bool serial = control.filterRouting == modules::FilterRouting::Serial;
    auto targetBus = [&](int f) {
        return control.filter[static_cast<std::size_t>(f)].output == modules::FilterOutput::Direct ? kBusDirect : kBusMain;
    };
    const bool f1On = control.filter[0].enabled, f2On = control.filter[1].enabled;
    const int f2Bus = f2On ? kBusF2 : targetBus(1);
    const int f1Bus = f1On ? kBusF1 : (serial ? f2Bus : targetBus(0));

    m_busUsed.fill(false);
    for (int src = 0; src < kSourceCount; ++src) {
        auto& send = m_sends[static_cast<std::size_t>(src)];
        send.count = 0;
        auto add = [&](int bus, float gain) {
            if (gain == 0.0f) return;
            for (int i = 0; i < send.count; ++i)
                if (send.bus[i] == bus) { send.gain[i] += gain; return; }
            send.bus[send.count] = bus;
            send.gain[send.count] = gain;
            ++send.count;
            m_busUsed[static_cast<std::size_t>(bus)] = true;
        };
        const auto [route, balance] = routeOf(src);
        // FX bus sends (pre-filter; the amp envelope applies to every bus).
        const float b1 = src < kOscCount ? control.osc[static_cast<std::size_t>(src)].bus1Send
                       : (src == kNoiseSource ? control.noise.bus1Send : control.sub.bus1Send);
        const float b2 = src < kOscCount ? control.osc[static_cast<std::size_t>(src)].bus2Send
                       : (src == kNoiseSource ? control.noise.bus2Send : control.sub.bus2Send);
        if (route != modules::Route::None) {
            add(kBusB1, b1);
            add(kBusB2, b2);
        }
        switch (route) {
            case modules::Route::Filter: {
                const float b = std::clamp(balance, 0.0f, 1.0f);
                add(f1Bus, 1.0f - b);
                add(f2Bus, b);
                break;
            }
            case modules::Route::Main:   add(kBusMain, 1.0f); break;
            case modules::Route::Direct: add(kBusDirect, 1.0f); break;
            case modules::Route::None:
            case modules::Route::Count:  break;
        }
    }

    // Filters: keytrack folds into the cutoff (100% = one octave per octave from C4).
    m_filterRouting = control.filterRouting;
    for (int f = 0; f < modules::FilterModule::kCount; ++f) {
        const auto& fv = control.filter[static_cast<std::size_t>(f)];
        m_filterOn[static_cast<std::size_t>(f)]  = fv.enabled;
        m_filterOut[static_cast<std::size_t>(f)] = fv.output;
        const float mul = std::exp2(fv.keytrack * (static_cast<float>(m_note) - 60.0f) / 12.0f);
        m_keytrackMul[static_cast<std::size_t>(f)] = mul;
        dsp::FilterSettings fs = fv.settings;
        fs.cutoffHz *= mul;
        m_filters[static_cast<std::size_t>(f)].set(fs);
    }
}

// --- Audio --------------------------------------------------------------------------------------------

// --- Phase 7 sources ---------------------------------------------------------------------------------------

namespace {

// Region setup shared by the Sample oscillator and SFZ regions.
void setupPlayer(dsp::SamplePlayer& pl, const dsp::SampleData& s, std::int64_t start, std::int64_t end,
                 dsp::SamplePlayer::Loop loop, std::int64_t loopStart, std::int64_t loopEnd, double xfade,
                 double engineRate) noexcept
{
    const std::int64_t frames = s.frames();
    pl.start = std::clamp<std::int64_t>(start, 0, std::max<std::int64_t>(0, frames - 1));
    pl.end = std::clamp<std::int64_t>(end, pl.start + 1, frames);
    pl.loop = loop;
    pl.loopStart = std::clamp<std::int64_t>(loopStart, pl.start, pl.end - 1);
    pl.loopEnd = std::clamp<std::int64_t>(loopEnd, pl.loopStart + 1, pl.end);
    pl.xfade = xfade;
    pl.rate = s.sampleRate() / engineRate;
    pl.begin(static_cast<double>(pl.start));
}

bool regionMatches(const dsp::Multisample::Region& r, int note, int velocity, std::uint32_t rr, dsp::Multisample::Trigger trigger) noexcept
{
    if (r.trigger != trigger || r.sample == nullptr) return false;
    if (note < r.loKey || note > r.hiKey || velocity < r.loVel || velocity > r.hiVel) return false;
    if (r.seqLength > 1 && static_cast<int>(rr % static_cast<std::uint32_t>(r.seqLength)) + 1 != r.seqPosition) return false;
    return true;
}

void setupRegion(dsp::SamplePlayer& pl, const dsp::Multisample::Region& r, int note, int velocity, double engineRate) noexcept
{
    const auto& s = *r.sample;
    const std::int64_t ls = r.loopStart >= 0 ? r.loopStart : (s.loopStart() >= 0 ? s.loopStart() : 0);
    const std::int64_t le = r.loopEnd >= 0 ? r.loopEnd : (s.loopEnd() >= 0 ? s.loopEnd() : s.frames());
    setupPlayer(pl, s, r.offset, r.end >= 0 ? r.end : s.frames(), r.loop, ls, le, r.loopXfadeSeconds * s.sampleRate(), engineRate);
    pl.rate *= std::exp2(((note - r.keyCenter) * r.keytrack / 100.0 + r.tuneCents / 100.0) / 12.0);
    float gl, gr;
    panGains(r.pan, gl, gr);
    const float g = r.gain * r.velCurve[static_cast<std::size_t>(std::clamp(velocity, 0, 127))];
    pl.gainL = gl * g;
    pl.gainR = gr * g;
}

} // namespace

void Voice::startGenerator(int o, const VoiceTables& tables) noexcept
{
    auto& osc = m_osc[static_cast<std::size_t>(o)];
    auto& gen = (*m_gen)[static_cast<std::size_t>(o)];
    gen.started = true;
    gen.startedType = osc.type;
    gen.playerCount = 0;
    gen.assetId = 0;
    const double sr = m_sampleRate;

    switch (osc.type) {
        case modules::OscType::Sample: {
            const auto* s = tables.sample[static_cast<std::size_t>(o)];
            if (s == nullptr || s->frames() < 2) return;
            const auto frames = static_cast<double>(s->frames());
            const bool fileLoop = osc.smpFileLoop && s->loopStart() >= 0;
            const std::int64_t ls = fileLoop ? s->loopStart() : static_cast<std::int64_t>(osc.smpLoopStart * frames);
            const std::int64_t le = fileLoop ? s->loopEnd() : static_cast<std::int64_t>(osc.smpLoopEnd * frames);
            setupPlayer(gen.players[0], *s, static_cast<std::int64_t>(osc.smpStart * frames), static_cast<std::int64_t>(osc.smpEnd * frames),
                        osc.smpLoop, ls, le, osc.smpXfade * static_cast<double>(le - ls), sr);
            gen.region[0] = -1;
            gen.playerCount = 1;
            gen.assetId = s->id();
            return;
        }
        case modules::OscType::Multisample: {
            const auto* ms = tables.multi[static_cast<std::size_t>(o)];
            if (ms == nullptr) return;
            const std::uint32_t rr = tables.roundRobin != nullptr ? tables.roundRobin[o]++ : 0u;
            for (std::size_t r = 0; r < ms->regions.size() && gen.playerCount < Generator::kLayers; ++r) {
                if (!regionMatches(ms->regions[r], m_note, m_velocityRaw, rr, dsp::Multisample::Trigger::Attack)) continue;
                setupRegion(gen.players[static_cast<std::size_t>(gen.playerCount)], ms->regions[r], m_note, m_velocityRaw, sr);
                gen.region[static_cast<std::size_t>(gen.playerCount++)] = static_cast<int>(r);
            }
            gen.assetId = ms->id;
            return;
        }
        case modules::OscType::Granular: {
            const auto* s = tables.sample[static_cast<std::size_t>(o)];
            if (s == nullptr) return;
            const std::uint64_t seed = (m_order + 1) * 0x9E3779B97F4A7C15ull ^ static_cast<std::uint64_t>(o + 1) * 0xBF58476D1CE4E5B9ull;
            gen.granular.start(osc.grn, seed);
            gen.assetId = s->id();
            return;
        }
        case modules::OscType::Spectral: {
            const auto* sp = tables.spectral[static_cast<std::size_t>(o)];
            if (sp == nullptr) return;
            gen.spectral.start(*sp, osc.spc);
            gen.assetId = sp->id();
            return;
        }
        case modules::OscType::Wavetable:
        case modules::OscType::Count:
            return;
    }
}

void Voice::startReleaseRegions(int o, const VoiceTables& tables) noexcept
{
    auto& gen = (*m_gen)[static_cast<std::size_t>(o)];
    const auto* ms = tables.multi[static_cast<std::size_t>(o)];
    if (ms == nullptr || ms->id != gen.assetId) return;
    // Release regions replace finished players first, then fill free slots.
    for (std::size_t r = 0; r < ms->regions.size(); ++r) {
        if (!regionMatches(ms->regions[r], m_note, m_velocityRaw, 0u, dsp::Multisample::Trigger::Release)) continue;
        int slot = -1;
        for (int p = 0; p < gen.playerCount && slot < 0; ++p) if (!gen.players[static_cast<std::size_t>(p)].active) slot = p;
        if (slot < 0 && gen.playerCount < Generator::kLayers) slot = gen.playerCount++;
        if (slot < 0) return;
        setupRegion(gen.players[static_cast<std::size_t>(slot)], ms->regions[r], m_note, m_velocityRaw, m_sampleRate);
        gen.players[static_cast<std::size_t>(slot)].released = true;
        gen.region[static_cast<std::size_t>(slot)] = static_cast<int>(r);
    }
}

bool Voice::renderGenerator(int o, int numSamples, const VoiceTables& tables) noexcept
{
    auto& osc = m_osc[static_cast<std::size_t>(o)];
    auto& gen = (*m_gen)[static_cast<std::size_t>(o)];
    if (!gen.started || gen.startedType != osc.type) startGenerator(o, tables);
    if (gen.assetId == 0) return false;
    std::fill(gen.left.begin(), gen.left.begin() + numSamples, 0.0f);
    std::fill(gen.right.begin(), gen.right.begin() + numSamples, 0.0f);

    switch (osc.type) {
        case modules::OscType::Sample: {
            const auto* s = tables.sample[static_cast<std::size_t>(o)];
            if (s == nullptr || s->id() != gen.assetId) return false;   // asset replaced: stay silent until the next note
            auto& pl = gen.players[0];
            if (!(osc.smpFileLoop && s->loopStart() >= 0)) {   // loop points are modulatable
                const auto frames = static_cast<double>(s->frames());
                pl.loopStart = std::clamp<std::int64_t>(static_cast<std::int64_t>(osc.smpLoopStart * frames), pl.start, pl.end - 1);
                pl.loopEnd = std::clamp<std::int64_t>(static_cast<std::int64_t>(osc.smpLoopEnd * frames), pl.loopStart + 1, pl.end);
                pl.xfade = osc.smpXfade * static_cast<double>(pl.loopEnd - pl.loopStart);
            }
            const double pitch = std::exp2((static_cast<double>(m_note - s->rootKey()) + osc.genOffsetSemis) / 12.0);
            pl.render(*s, gen.left.data(), gen.right.data(), numSamples, pitch);
            return true;
        }
        case modules::OscType::Multisample: {
            const auto* ms = tables.multi[static_cast<std::size_t>(o)];
            if (ms == nullptr || ms->id != gen.assetId) return false;
            if (m_releasePending) startReleaseRegions(o, tables);
            const double pitch = std::exp2(osc.genOffsetSemis / 12.0);
            for (int p = 0; p < gen.playerCount; ++p) {
                const int r = gen.region[static_cast<std::size_t>(p)];
                if (r < 0 || r >= static_cast<int>(ms->regions.size())) continue;
                gen.players[static_cast<std::size_t>(p)].render(*ms->regions[static_cast<std::size_t>(r)].sample, gen.left.data(), gen.right.data(), numSamples, pitch);
            }
            return true;
        }
        case modules::OscType::Granular: {
            const auto* s = tables.sample[static_cast<std::size_t>(o)];
            if (s == nullptr || s->id() != gen.assetId) return false;
            osc.grn.pitch = std::exp2((static_cast<double>(m_note - s->rootKey()) + osc.genOffsetSemis) / 12.0);
            gen.granular.render(*s, osc.grn, gen.left.data(), gen.right.data(), numSamples);
            return true;
        }
        case modules::OscType::Spectral: {
            const auto* sp = tables.spectral[static_cast<std::size_t>(o)];
            if (sp == nullptr || sp->id() != gen.assetId || tables.fft == nullptr) return false;
            osc.spc.pitch = std::exp2((static_cast<double>(m_note - sp->rootKey()) + osc.genOffsetSemis) / 12.0);
            gen.spectral.render(*sp, osc.spc, *tables.fft, gen.left.data(), gen.right.data(), numSamples);
            return true;
        }
        case modules::OscType::Wavetable:
        case modules::OscType::Count:
            break;
    }
    return false;
}

void Voice::render(const VoiceOutputs& out, int numSamples, const VoiceTables& tables) noexcept
{
    if (!m_env[0].isActive()) return;

    const int os = m_oversample;
    float buf[kBusCount][2][kControlBlock * kMaxOversample];
    float cutoffHz[modules::FilterModule::kCount][kControlBlock];
    bool  fastCutoff[modules::FilterModule::kCount] = {};

    // Sample-based sources render their whole chunk up front (at the base rate).
    bool genActive[kOscCount] = {}, wtActive[kOscCount] = {};
    bool anyGen = false;
    for (int o = 0; o < kOscCount; ++o) {
        const auto& osc = m_osc[static_cast<std::size_t>(o)];
        if (!osc.on || !osc.mapped) continue;
        if (osc.type == modules::OscType::Wavetable) wtActive[o] = true;
        else anyGen |= genActive[o] = renderGenerator(o, numSamples, tables);
    }
    m_releasePending = false;

    for (int i = 0; i < numSamples; ++i) {
        // Per base sample: smoothed per-oscillator values, plus audio-rate LFO modulation on top.
        float wt[kOscCount], level[kOscCount], pitchRatio[kOscCount];
        float dPitch[kOscCount] = {}, dLevel[kOscCount] = {}, dWt[kOscCount] = {};
        float dCutoff[modules::FilterModule::kCount] = {};
        bool  cutoffMod[modules::FilterModule::kCount] = {};
        if (m_fastCount > 0) {
            float lfoValue[modulation::kLfoCount];
            for (int l = 0; l < modulation::kLfoCount; ++l)
                if (m_lfoFast[static_cast<std::size_t>(l)]) lfoValue[l] = m_lfo[static_cast<std::size_t>(l)].tick(tables.lfo(l));
            for (int f = 0; f < m_fastCount; ++f) {
                const auto& fs = m_fast[static_cast<std::size_t>(f)];
                const float d = shapeSource(lfoValue[fs.lfo], fs.curve, fs.bipolar) * fs.scale;
                switch (fs.dest.kind) {
                    case modules::FastDest::Kind::OscPitch:     dPitch[fs.dest.osc] += d * fs.dest.span; break;
                    case modules::FastDest::Kind::OscLevel:     dLevel[fs.dest.osc] += d * fs.dest.span; break;
                    case modules::FastDest::Kind::OscWtPos:     dWt[fs.dest.osc]    += d * fs.dest.span; break;
                    case modules::FastDest::Kind::FilterCutoff: dCutoff[fs.dest.osc] += d; cutoffMod[fs.dest.osc] = true; break;
                    case modules::FastDest::Kind::None: break;
                }
            }
        }
        for (int o = 0; o < kOscCount; ++o) {
            auto& osc = m_osc[static_cast<std::size_t>(o)];
            wt[o]    = std::clamp(osc.wtPos.next() + dWt[o], 0.0f, 1.0f);
            level[o] = std::max(0.0f, osc.level.next() + dLevel[o]);
            pitchRatio[o] = dPitch[o] != 0.0f ? std::exp2(dPitch[o] / 12.0f) : 1.0f;
        }
        for (int f = 0; f < modules::FilterModule::kCount; ++f) {
            if (cutoffMod[f]) {
                fastCutoff[f] = true;
                cutoffHz[f][i] = m_keytrackMul[static_cast<std::size_t>(f)] * static_cast<float>(fromNormalized(
                    m_modules->targets.at(m_modules->filter[static_cast<std::size_t>(f)]->cutoffIndex()).meta,
                    std::clamp(static_cast<double>(m_cutoffNorm[static_cast<std::size_t>(f)] + dCutoff[f]), 0.0, 1.0)));
            } else {
                cutoffHz[f][i] = -1.0f;
            }
        }
        const float subLevel   = m_subLevel.next();
        const float noiseLevel = m_noiseLevel.next();

        // Frame position per oscillator, resolved once per base sample (shared by its unison voices unless
        // they spread across the table).
        dsp::WavetableBank::FramePos frames[kOscCount];
        for (int o = 0; o < kOscCount; ++o) {
            const auto& osc = m_osc[static_cast<std::size_t>(o)];
            const dsp::WavetableBank* table = tables.osc[static_cast<std::size_t>(o)];
            if (!wtActive[o] || table == nullptr) continue;
            float frame = wt[o] * static_cast<float>(table->frameCount() - 1);
            if (!osc.smooth) frame = std::round(frame);
            frames[o] = table->resolveFrame(frame);
        }

        for (int s = 0; s < os; ++s) {
            float acc[kBusCount][2] = {};
            auto send = [&](int src, float l, float r) {
                const auto& sd = m_sends[static_cast<std::size_t>(src)];
                for (int i = 0; i < sd.count; ++i) {
                    acc[sd.bus[i]][0] += l * sd.gain[i];
                    acc[sd.bus[i]][1] += r * sd.gain[i];
                }
            };

            float sub = 0.0f;
            if (m_subNeeded && tables.sub != nullptr) {
                sub = tables.sub->read(m_subPhase, static_cast<float>(m_subShape), m_subLevels);
                m_subPhase += m_subInc;
                if (m_subPhase >= 1.0) m_subPhase -= 1.0;
                if (m_subOn) send(kSubSource, sub * subLevel * m_subGainL, sub * subLevel * m_subGainR);
            }

            float noise = 0.0f;
            if (m_noiseNeeded && tables.noise != nullptr) {
                noise = tables.noise->read(m_noiseType, m_noisePos);
                m_noisePos += m_noiseRate;
                if (m_noisePos >= dsp::NoiseTables::kLength) m_noisePos -= dsp::NoiseTables::kLength;
                if (m_noiseOn) send(kNoiseSource, noise * noiseLevel * m_noiseGainL, noise * noiseLevel * m_noiseGainR);
            }

            for (int o = 0; o < kOscCount; ++o) {
                auto& osc = m_osc[static_cast<std::size_t>(o)];
                const dsp::WavetableBank* table = tables.osc[static_cast<std::size_t>(o)];
                if (!wtActive[o] || table == nullptr) continue;

                const float lastFrame = static_cast<float>(table->frameCount() - 1);
                const float paired = m_osc[static_cast<std::size_t>(kPairedOsc[o])].last;
                const float mod1 = warpInputValue(osc.warp1, paired, noise, sub);
                const float mod2 = warpInputValue(osc.warp2, paired, noise, sub);
                const bool  warped = osc.warp1 != dsp::WarpMode::Off || osc.warp2 != dsp::WarpMode::Off;
                const float baseFrame = wt[o] * lastFrame;
                const dsp::CurveTable* remap = tables.remap[static_cast<std::size_t>(o)];
                const double ratio = pitchRatio[o];

                float oscL = 0.0f, oscR = 0.0f, mono = 0.0f;
                int u = 0;
                if (osc.span <= 0.0f && !warped) {
                    // Plain path: four unison voices per read4 (SSE2 maths; see WavetableBank::read4).
                    for (; u + 4 <= osc.count; u += 4) {
                        auto* v = &osc.voices[static_cast<std::size_t>(u)];
                        const double ph[4] = {v[0].phase, v[1].phase, v[2].phase, v[3].phase};
                        const dsp::WavetableBank::LevelChoice lc[4] = {v[0].levels, v[1].levels, v[2].levels, v[3].levels};
                        float x4[4];
                        table->read4(ph, frames[o], lc, x4);
                        for (int k = 0; k < 4; ++k) {
                            v[k].phase += v[k].inc * ratio;
                            if (v[k].phase >= 1.0) v[k].phase -= std::floor(v[k].phase);
                            oscL += x4[k] * v[k].gainL;
                            oscR += x4[k] * v[k].gainR;
                            mono += x4[k];
                        }
                    }
                    if (osc.count - u >= 2) {
                        // Trailing 2-3 voices: one padded read4 (extra lanes repeat the last voice, ignored).
                        const int lanes = osc.count - u;
                        auto* v = &osc.voices[static_cast<std::size_t>(u)];
                        double ph[4];
                        dsp::WavetableBank::LevelChoice lc[4];
                        for (int k = 0; k < 4; ++k) {
                            const auto& src = v[std::min(k, lanes - 1)];
                            ph[k] = src.phase;
                            lc[k] = src.levels;
                        }
                        float x4[4];
                        table->read4(ph, frames[o], lc, x4);
                        for (int k = 0; k < lanes; ++k) {
                            v[k].phase += v[k].inc * ratio;
                            if (v[k].phase >= 1.0) v[k].phase -= std::floor(v[k].phase);
                            oscL += x4[k] * v[k].gainL;
                            oscR += x4[k] * v[k].gainR;
                            mono += x4[k];
                        }
                        u = osc.count;
                    }
                } else if (osc.span <= 0.0f) {
                    // Warped path: warps per lane (they move the read phase / scale the output), reads four at a time.
                    while (osc.count - u >= 2) {
                        const int lanes = std::min(4, osc.count - u);
                        auto* v = &osc.voices[static_cast<std::size_t>(u)];
                        double ph[4];
                        float amp[4];
                        dsp::WavetableBank::LevelChoice lc[4];
                        for (int k = 0; k < 4; ++k) {
                            const auto& src = v[std::min(k, lanes - 1)];
                            const dsp::WarpOut w = dsp::applyDualWarp(osc.warp1, src.warp1, mod1, osc.warp2, src.warp2, mod2, src.phase, remap);
                            ph[k] = w.phase;
                            amp[k] = w.amp;
                            lc[k] = src.levels;
                        }
                        float x4[4];
                        table->read4(ph, frames[o], lc, x4);
                        for (int k = 0; k < lanes; ++k) {
                            const float x = x4[k] * amp[k];
                            v[k].phase += v[k].inc * ratio;
                            if (v[k].phase >= 1.0) v[k].phase -= std::floor(v[k].phase);
                            oscL += x * v[k].gainL;
                            oscR += x * v[k].gainR;
                            mono += x;
                        }
                        u += lanes;
                    }
                }
                for (; u < osc.count; ++u) {
                    auto& uv = osc.voices[static_cast<std::size_t>(u)];
                    dsp::WavetableBank::FramePos fp = frames[o];
                    if (osc.span > 0.0f) {
                        float frame = std::clamp(baseFrame + osc.span * uv.spread * 0.5f * lastFrame, 0.0f, lastFrame);
                        if (!osc.smooth) frame = std::round(frame);
                        fp = table->resolveFrame(frame);
                    }

                    float x;
                    if (warped) {
                        const dsp::WarpOut w = dsp::applyDualWarp(osc.warp1, uv.warp1, mod1, osc.warp2, uv.warp2, mod2, uv.phase, remap);
                        x = w.amp != 0.0f ? table->read(w.phase, fp, uv.levels) * w.amp : 0.0f;
                    } else {
                        x = table->read(uv.phase, fp, uv.levels);
                    }
                    uv.phase += uv.inc * ratio;
                    if (uv.phase >= 1.0) uv.phase -= std::floor(uv.phase);

                    oscL += x * uv.gainL;
                    oscR += x * uv.gainR;
                    mono += x;
                }
                osc.last = mono / static_cast<float>(osc.count);
                send(o, oscL * level[o], oscR * level[o]);
            }

            if (anyGen) {
                for (int o = 0; o < kOscCount; ++o) {
                    if (!genActive[o]) continue;
                    // Base-rate generator output, linearly interpolated across oversampled sub-steps.
                    auto& osc = m_osc[static_cast<std::size_t>(o)];
                    const auto& gen = (*m_gen)[static_cast<std::size_t>(o)];
                    float gl = gen.left[static_cast<std::size_t>(i)], gr = gen.right[static_cast<std::size_t>(i)];
                    if (os > 1) {
                        const float t = static_cast<float>(s + 1) / static_cast<float>(os);
                        gl = osc.prevL + (gl - osc.prevL) * t;
                        gr = osc.prevR + (gr - osc.prevR) * t;
                        if (s + 1 == os) { osc.prevL = gen.left[static_cast<std::size_t>(i)]; osc.prevR = gen.right[static_cast<std::size_t>(i)]; }
                    }
                    osc.last = 0.5f * (gl + gr);
                    send(o, gl * level[o] * osc.genGainL, gr * level[o] * osc.genGainR);
                }
            }

            for (int b = 0; b < kBusCount; ++b) {
                if (!m_busUsed[static_cast<std::size_t>(b)]) continue;
                buf[b][0][i * os + s] = acc[b][0];
                buf[b][1][i * os + s] = acc[b][1];
            }
        }
    }

    // Decimate each used bus to the base rate (in place: each stage halves the length).
    if (os >= 2) {
        for (int b = 0; b < kBusCount; ++b) {
            if (!m_busUsed[static_cast<std::size_t>(b)]) continue;
            auto* d = &m_down[static_cast<std::size_t>(b * 4)];
            if (os == 4) {
                d[0].process_block(buf[b][0], buf[b][0], numSamples * 2);
                d[1].process_block(buf[b][1], buf[b][1], numSamples * 2);
            }
            d[2].process_block(buf[b][0], buf[b][0], numSamples);
            d[3].process_block(buf[b][1], buf[b][1], numSamples);
        }
    }

    // Filters (serial: F1 → F2; parallel: independent), each to its output target; then the amp envelope.
    const bool filtersUsed = m_busUsed[kBusF1] || m_busUsed[kBusF2];
    const bool serial = m_filterRouting == modules::FilterRouting::Serial;
    for (int i = 0; i < numSamples; ++i) {
        float outMain[2]   = {m_busUsed[kBusMain] ? buf[kBusMain][0][i] : 0.0f, m_busUsed[kBusMain] ? buf[kBusMain][1][i] : 0.0f};
        float outDirect[2] = {m_busUsed[kBusDirect] ? buf[kBusDirect][0][i] : 0.0f, m_busUsed[kBusDirect] ? buf[kBusDirect][1][i] : 0.0f};

        if (filtersUsed) {
            float f1l = m_busUsed[kBusF1] ? buf[kBusF1][0][i] : 0.0f, f1r = m_busUsed[kBusF1] ? buf[kBusF1][1][i] : 0.0f;
            float f2l = m_busUsed[kBusF2] ? buf[kBusF2][0][i] : 0.0f, f2r = m_busUsed[kBusF2] ? buf[kBusF2][1][i] : 0.0f;
            for (int f = 0; f < modules::FilterModule::kCount; ++f)
                if (fastCutoff[f] && cutoffHz[f][i] > 0.0f) m_filters[static_cast<std::size_t>(f)].setCutoff(cutoffHz[f][i]);
            // A bus only carries signal while its filter is enabled (bypassed filters were folded into the
            // routing at control rate).
            auto toTarget = [&](int f, float l, float r) {
                float* dst = m_filterOut[static_cast<std::size_t>(f)] == modules::FilterOutput::Direct ? outDirect : outMain;
                dst[0] += l;
                dst[1] += r;
            };
            if (m_busUsed[kBusF1]) {
                m_filters[0].process(f1l, f1r);
                if (serial) {
                    if (m_filterOn[1]) { f2l += f1l; f2r += f1r; }
                    else               toTarget(1, f1l, f1r);   // F2 bypassed: F1 goes where F2 would
                } else {
                    toTarget(0, f1l, f1r);
                }
            }
            if (m_busUsed[kBusF2] || (serial && m_busUsed[kBusF1] && m_filterOn[1])) {
                m_filters[1].process(f2l, f2r);
                toTarget(1, f2l, f2r);
            }
        }

        const float amp = m_env[0].next();
        for (int e = 1; e < modulation::kEnvCount; ++e) m_env[static_cast<std::size_t>(e)].next();
        out.mainL[i]   += outMain[0] * amp;
        out.mainR[i]   += outMain[1] * amp;
        out.directL[i] += outDirect[0] * amp;
        out.directR[i] += outDirect[1] * amp;
        if (m_busUsed[kBusB1]) {
            out.bus1L[i] += buf[kBusB1][0][i] * amp;
            out.bus1R[i] += buf[kBusB1][1][i] * amp;
        }
        if (m_busUsed[kBusB2]) {
            out.bus2L[i] += buf[kBusB2][0][i] * amp;
            out.bus2R[i] += buf[kBusB2][1][i] * amp;
        }
    }

    // Control-rate LFOs advance in one step per chunk; audio-rate ones were ticked per sample above.
    for (int l = 0; l < modulation::kLfoCount; ++l)
        if (!m_lfoFast[static_cast<std::size_t>(l)])
            m_lfo[static_cast<std::size_t>(l)].advance(numSamples, tables.lfo(l));
}

} // namespace winerose::voice
