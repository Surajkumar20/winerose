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

void Voice::initDownsamplerCoefs() noexcept
{
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
    m_svfL.reset();
    m_svfR.reset();
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
    m_cutoffNorm = static_cast<float>(toNormalized(mods.targets.at(mods.filter0->cutoffIndex()).meta,
                                                   m_settings.control.filter.cutoffHz));
    m_resonance = m_settings.control.filter.resonance;
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
    m_noteNorm = static_cast<float>(std::clamp(note, 0, 127)) / 127.0f;

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

    m_svfL.reset();
    m_svfR.reset();
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
    m_noteNorm = static_cast<float>(std::clamp(note, 0, 127)) / 127.0f;
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
        if (!osc.on) continue;

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

    // Sub
    m_subOn    = control.sub.enabled;
    m_subShape = static_cast<int>(control.sub.shape);
    m_subInc   = std::min(hzForSemis(static_cast<double>(m_note) - 69.0 + control.sub.pitchSemis + control.bendSemis) / sr, 0.5) / os;
    m_subLevels = dsp::WavetableBank::selectLevel(m_subInc);
    panGains(control.sub.pan, m_subGainL, m_subGainR);

    // Noise: rate in table samples per output sample (the tables are "recorded" at the engine rate).
    m_noiseOn   = control.noise.enabled;
    m_noiseType = control.noise.type;
    const double noiseSemis = (control.noise.keytrack ? static_cast<double>(m_note) - 60.0 : 0.0) + control.noise.pitchSemis;
    m_noiseRate = std::exp2(noiseSemis / 12.0) / os;
    panGains(control.noise.pan, m_noiseGainL, m_noiseGainR);

    // Filter
    if (control.filter.enabled != m_filterOn) {
        m_svfL.reset();
        m_svfR.reset();
    }
    m_filterOn = control.filter.enabled;
    m_coefs    = control.filterCoefs;
}

// --- Audio --------------------------------------------------------------------------------------------

void Voice::render(float* left, float* right, int numSamples, const VoiceTables& tables) noexcept
{
    if (!m_env[0].isActive()) return;

    const int os = m_oversample;
    float bufL[kControlBlock * kMaxOversample];
    float bufR[kControlBlock * kMaxOversample];
    float cutoffHz[kControlBlock];
    bool  fastCutoff = false;

    for (int i = 0; i < numSamples; ++i) {
        // Per base sample: smoothed per-oscillator values, plus audio-rate LFO modulation on top.
        float wt[kOscCount], level[kOscCount], pitchRatio[kOscCount];
        float dPitch[kOscCount] = {}, dLevel[kOscCount] = {}, dWt[kOscCount] = {};
        float dCutoff = 0.0f;
        bool  cutoffMod = false;
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
                    case modules::FastDest::Kind::FilterCutoff: dCutoff += d; cutoffMod = true; break;
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
        if (cutoffMod) {
            fastCutoff = true;
            cutoffHz[i] = static_cast<float>(fromNormalized(
                m_modules->targets.at(m_modules->filter0->cutoffIndex()).meta,
                std::clamp(static_cast<double>(m_cutoffNorm + dCutoff), 0.0, 1.0)));
        } else {
            cutoffHz[i] = -1.0f;
        }
        const float subLevel   = m_subLevel.next();
        const float noiseLevel = m_noiseLevel.next();

        // Frame position per oscillator, resolved once per base sample (shared by its unison voices unless
        // they spread across the table).
        dsp::WavetableBank::FramePos frames[kOscCount];
        for (int o = 0; o < kOscCount; ++o) {
            const auto& osc = m_osc[static_cast<std::size_t>(o)];
            const dsp::WavetableBank* table = tables.osc[static_cast<std::size_t>(o)];
            if (!osc.on || table == nullptr) continue;
            float frame = wt[o] * static_cast<float>(table->frameCount() - 1);
            if (!osc.smooth) frame = std::round(frame);
            frames[o] = table->resolveFrame(frame);
        }

        for (int s = 0; s < os; ++s) {
            float accL = 0.0f, accR = 0.0f;

            float sub = 0.0f;
            if (m_subOn && tables.sub != nullptr) {
                sub = tables.sub->read(m_subPhase, static_cast<float>(m_subShape), m_subLevels);
                m_subPhase += m_subInc;
                if (m_subPhase >= 1.0) m_subPhase -= 1.0;
                accL += sub * subLevel * m_subGainL;
                accR += sub * subLevel * m_subGainR;
            }

            float noise = 0.0f;
            if (tables.noise != nullptr) {
                noise = tables.noise->read(m_noiseType, m_noisePos);
                m_noisePos += m_noiseRate;
                if (m_noisePos >= dsp::NoiseTables::kLength) m_noisePos -= dsp::NoiseTables::kLength;
                if (m_noiseOn) {
                    accL += noise * noiseLevel * m_noiseGainL;
                    accR += noise * noiseLevel * m_noiseGainR;
                }
            }

            for (int o = 0; o < kOscCount; ++o) {
                auto& osc = m_osc[static_cast<std::size_t>(o)];
                const dsp::WavetableBank* table = tables.osc[static_cast<std::size_t>(o)];
                if (!osc.on || table == nullptr) continue;

                const float lastFrame = static_cast<float>(table->frameCount() - 1);
                const float paired = m_osc[static_cast<std::size_t>(kPairedOsc[o])].last;
                const float mod1 = warpInputValue(osc.warp1, paired, noise, sub);
                const float mod2 = warpInputValue(osc.warp2, paired, noise, sub);
                const bool  warped = osc.warp1 != dsp::WarpMode::Off || osc.warp2 != dsp::WarpMode::Off;
                const float baseFrame = wt[o] * lastFrame;
                const dsp::CurveTable* remap = tables.remap[static_cast<std::size_t>(o)];
                const double ratio = pitchRatio[o];

                float oscL = 0.0f, oscR = 0.0f, mono = 0.0f;
                for (int u = 0; u < osc.count; ++u) {
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
                accL += oscL * level[o];
                accR += oscR * level[o];
            }

            bufL[i * os + s] = accL;
            bufR[i * os + s] = accR;
        }
    }

    // Decimate to the base rate (in place: each stage halves the length).
    if (os == 4) {
        m_down[0].process_block(bufL, bufL, numSamples * 2);
        m_down[1].process_block(bufR, bufR, numSamples * 2);
    }
    if (os >= 2) {
        m_down[2].process_block(bufL, bufL, numSamples);
        m_down[3].process_block(bufR, bufR, numSamples);
    }

    for (int i = 0; i < numSamples; ++i) {
        float l = bufL[i], r = bufR[i];
        if (m_filterOn) {
            const dsp::Svf::Coefs coefs = (fastCutoff && cutoffHz[i] > 0.0f)
                ? dsp::Svf::compute(cutoffHz[i], m_resonance, m_sampleRate) : m_coefs;
            l = m_svfL.processLow(l, coefs);
            r = m_svfR.processLow(r, coefs);
        }
        const float amp = m_env[0].next();
        for (int e = 1; e < modulation::kEnvCount; ++e) m_env[static_cast<std::size_t>(e)].next();
        left[i]  += l * amp;
        right[i] += r * amp;
    }

    // Control-rate LFOs advance in one step per chunk; audio-rate ones were ticked per sample above.
    for (int l = 0; l < modulation::kLfoCount; ++l)
        if (!m_lfoFast[static_cast<std::size_t>(l)])
            m_lfo[static_cast<std::size_t>(l)].advance(numSamples, tables.lfo(l));
}

} // namespace winerose::voice
