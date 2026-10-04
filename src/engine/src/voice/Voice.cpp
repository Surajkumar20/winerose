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

} // namespace

void Voice::initDownsamplerCoefs() noexcept
{
    // 12 coefficients, transition band 0.04 of the half-band: > 100 dB stopband (HIIR designer).
    hiir::PolyphaseIir2Designer::compute_coefs_spec_order_tbw(g_downCoefs, kDownsamplerCoefs, 0.04);
}

void Voice::prepare(double sampleRate) noexcept
{
    m_sampleRate = sampleRate;
    m_env.setSampleRate(sampleRate);
    for (auto& o : m_osc) {
        o.wtPos.setRampSamples(kControlBlock);
        o.level.setRampSamples(kControlBlock);
    }
    m_subLevel.setRampSamples(kControlBlock);
    m_noiseLevel.setRampSamples(kControlBlock);
    for (auto& d : m_down) d.set_coefs(g_downCoefs);
    resetDownsamplers();
    m_env.reset();
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

void Voice::start(int note, std::uint64_t order, const VoiceControl& control, Rng& rng) noexcept
{
    m_note = note;
    m_order = order;
    m_released = false;
    m_sustained = false;

    // Draw the same amount of randomness every note (independent of unison counts), so renders stay
    // deterministic even when parameters change between notes.
    for (int o = 0; o < kOscCount; ++o) {
        const auto& v = control.osc[static_cast<std::size_t>(o)];
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

    m_svfL.reset();
    m_svfR.reset();
    resetDownsamplers();
    m_oversample = control.oversample;
    layout(control);

    // Fresh voice: no ramps — start exactly at the current control values.
    for (int o = 0; o < kOscCount; ++o) {
        auto& osc = m_osc[static_cast<std::size_t>(o)];
        osc.wtPos.reset(control.osc[static_cast<std::size_t>(o)].wtPos);
        osc.level.reset(control.osc[static_cast<std::size_t>(o)].level);
    }
    m_subLevel.reset(control.sub.level);
    m_noiseLevel.reset(control.noise.level);

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
    layout(control);
    m_env.setSettings(control.env);
    m_env.noteOn();   // attacks from the current level
}

void Voice::control(const VoiceControl& control) noexcept
{
    if (control.oversample != m_oversample) {
        resetDownsamplers();
        m_oversample = control.oversample;
    }
    layout(control);
    for (int o = 0; o < kOscCount; ++o) {
        auto& osc = m_osc[static_cast<std::size_t>(o)];
        osc.wtPos.setTarget(control.osc[static_cast<std::size_t>(o)].wtPos);
        osc.level.setTarget(control.osc[static_cast<std::size_t>(o)].level);
    }
    m_subLevel.setTarget(control.sub.level);
    m_noiseLevel.setTarget(control.noise.level);
    m_env.setSettings(control.env);
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
            const double semis = static_cast<double>(m_note) - 69.0 + v.pitchSemis
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
    m_subInc   = std::min(hzForSemis(static_cast<double>(m_note) - 69.0 + control.sub.pitchSemis) / sr, 0.5) / os;
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

void Voice::render(float* left, float* right, int numSamples, const VoiceTables& tables) noexcept
{
    if (!m_env.isActive()) return;

    const int os = m_oversample;
    float bufL[kControlBlock * kMaxOversample];
    float bufR[kControlBlock * kMaxOversample];

    for (int i = 0; i < numSamples; ++i) {
        // Per base sample: smoothed per-oscillator values.
        float wt[kOscCount], level[kOscCount];
        for (int o = 0; o < kOscCount; ++o) {
            wt[o]    = m_osc[static_cast<std::size_t>(o)].wtPos.next();
            level[o] = m_osc[static_cast<std::size_t>(o)].level.next();
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
                        const dsp::WarpOut w = dsp::applyDualWarp(osc.warp1, uv.warp1, mod1, osc.warp2, uv.warp2, mod2, uv.phase);
                        x = w.amp != 0.0f ? table->read(w.phase, fp, uv.levels) * w.amp : 0.0f;
                    } else {
                        x = table->read(uv.phase, fp, uv.levels);
                    }
                    uv.phase += uv.inc;
                    if (uv.phase >= 1.0) uv.phase -= 1.0;

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
            l = m_svfL.processLow(l, m_coefs);
            r = m_svfR.processLow(r, m_coefs);
        }
        const float env = m_env.next();
        left[i]  += l * env;
        right[i] += r * env;
    }
}

} // namespace winerose::voice
