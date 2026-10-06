// Engine Phase 3 (SPEC §5.4): curves, LFOs (incl. chaos and audio rate), envelopes 1-4, macros and the
// 64-slot mod matrix.

#include "EngineRig.h"

#include "engine/dsp/Curve.h"
#include "engine/dsp/Fft.h"
#include "engine/dsp/Warp.h"
#include "engine/modulation/Lfo.h"
#include "engine/modulation/Sources.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace winerose;
using winerose::test::midiEvent;
using winerose::test::Rig;
using modulation::LfoMode;
using modulation::LfoSettings;
using modulation::LfoShape;
using modulation::LfoState;
using modulation::Source;
using Catch::Approx;

namespace {

void route(Rig& rig, int slot, Source source, const std::string& dest, float amount)
{
    auto& s = rig.reg("ModSlot" + std::to_string(slot));
    s.set("source", static_cast<int>(source));
    s.set<std::string>("destination", dest);
    s.set("amount", amount);
    rig.engine.publishSnapshot();   // the control layer does this on string changes; tests call it directly
}

// A clean sine voice: A on, sine frame, no random phase, instant attack.
void sineVoice(Rig& rig)
{
    auto& osc = rig.reg("Oscillator0");
    osc.set("wtPos", 1.0f);
    osc.set("random", 0.0f);
    rig.reg("Env0").set("attack", 0.0f);
}

float peakOver(Rig& rig, int blocks)
{
    float peak = 0.0f;
    for (int b = 0; b < blocks; ++b) peak = std::max(peak, rig.block());
    return peak;
}

std::vector<double> spectrum(const std::vector<float>& x)   // amplitude per 1 Hz bin for 48000 samples
{
    dsp::RealFft fft(48000);
    std::vector<float> s(48000);
    fft.forward(x.data(), s.data());
    std::vector<double> a(24000, 0.0);
    for (int k = 1; k < 24000; ++k)
        a[static_cast<std::size_t>(k)] = 2.0 * std::hypot(s[static_cast<std::size_t>(2 * k)], s[static_cast<std::size_t>(2 * k + 1)]) / 48000.0;
    return a;
}

double db(double a) { return a > 0.0 ? 20.0 * std::log10(a) : -400.0; }

} // namespace

// --- Curves ------------------------------------------------------------------------------------------

TEST_CASE("curves parse, evaluate and round-trip", "[mod][curve]")
{
    const auto tri = dsp::Curve::parse("0,0,0;0.5,1,0;1,0,0", dsp::Curve::identity());
    CHECK(tri.evaluate(0.25) == Approx(0.5));
    CHECK(tri.evaluate(0.5) == Approx(1.0));
    CHECK(tri.evaluate(0.75) == Approx(0.5));
    CHECK(dsp::Curve::parse(tri.serialize(), dsp::Curve::identity()).evaluate(0.3) == Approx(tri.evaluate(0.3)));

    const auto bent = dsp::Curve::parse("0,0,0.5;1,1,0", dsp::Curve::identity());
    CHECK(bent.evaluate(0.5) < 0.2);   // c > 0: slow start

    for (const char* bad : {"", "garbage", "0,0", "0.2,0,0;1,1,0", "0,0,0;0.5,x,0;1,1,0"})
        CHECK(dsp::Curve::parse(bad, dsp::Curve::triangle()).evaluate(0.5) == Approx(1.0));   // fallback
}

// --- LFO generator -----------------------------------------------------------------------------------

namespace {
struct LfoRig {
    std::shared_ptr<const dsp::WavetableBank> shapes;
    modulation::LfoTables tables;
    LfoRig()
    {
        // Same construction as the engine's fixed-shape bank.
        constexpr int N = dsp::WavetableBank::kFrameSize;
        std::vector<float> f(static_cast<std::size_t>(5 * N));
        for (int i = 0; i < N; ++i) {
            const double p = (i + 0.5) / N;
            f[static_cast<std::size_t>(i)]         = static_cast<float>(std::sin(2.0 * 3.14159265358979 * i / N));
            f[static_cast<std::size_t>(N + i)]     = static_cast<float>(p < 0.5 ? 4 * p - 1 : 3 - 4 * p);
            f[static_cast<std::size_t>(2 * N + i)] = static_cast<float>(2 * p - 1);
            f[static_cast<std::size_t>(3 * N + i)] = static_cast<float>(1 - 2 * p);
            f[static_cast<std::size_t>(4 * N + i)] = p < 0.5 ? 1.0f : -1.0f;
        }
        shapes = dsp::WavetableBank::build(f, N, "lfo", false);
        tables.shapes = shapes.get();
    }
};
}

TEST_CASE("a 1 kHz saw LFO is alias-free at audio rate (SPEC Phase 3 acceptance)", "[mod][lfo][acceptance]")
{
    LfoRig rig;
    LfoSettings s;
    s.shape = LfoShape::SawUp;
    s.mode = LfoMode::Free;
    s.sync = false;
    s.rateHz = 1000.0f;
    LfoState lfo;
    lfo.start(s, 0.0, 1);
    lfo.configure(s, 48000.0, 120.0);
    std::vector<float> x(48000);
    for (auto& v : x) v = lfo.tick(rig.tables) - 0.5f;   // remove the 0..1 offset
    const auto a = spectrum(x);
    double worst = 0.0;
    for (int k = 1; k < 24000; ++k) if (k % 1000 != 0) worst = std::max(worst, a[static_cast<std::size_t>(k)]);
    INFO("fundamental " << db(a[1000]) << " dB, worst alias " << db(worst) << " dB");
    CHECK(db(a[1000]) > -12.0);
    CHECK(db(worst) < -90.0);
}

TEST_CASE("tempo-synced LFO rates follow the BPM", "[mod][lfo]")
{
    LfoSettings s;
    s.sync = true;
    s.division = modulation::kDefaultSyncDivision;   // 1/4
    CHECK(s.frequency(120.0) == Approx(2.0));
    s.division = 3;                                   // 1 bar
    CHECK(s.frequency(120.0) == Approx(0.5));
    s.sync = false;
    s.rateHz = 7.0f;
    CHECK(s.frequency(120.0) == Approx(7.0));
}

TEST_CASE("LFO modes: Trig starts at the phase, Env holds at the end", "[mod][lfo]")
{
    LfoRig rig;
    LfoSettings s;
    s.shape = LfoShape::SawUp;
    s.sync = false;
    s.rateHz = 10.0f;
    s.mode = LfoMode::Trig;
    s.phase = 0.5f;
    LfoState lfo;
    lfo.start(s, 0.0, 1);
    lfo.configure(s, 48000.0, 120.0);
    lfo.advance(0, rig.tables);
    CHECK(lfo.value() == Approx(0.5f).margin(0.02f));

    s.mode = LfoMode::Env;
    s.phase = 0.0f;
    lfo.start(s, 0.0, 1);
    lfo.configure(s, 48000.0, 120.0);
    for (int i = 0; i < 48000; ++i) lfo.advance(1, rig.tables);   // 10 cycles' worth of time
    CHECK(lfo.value() > 0.95f);                                   // held near the saw's top
}

TEST_CASE("LFO delay holds the output and rise fades it in", "[mod][lfo]")
{
    LfoRig rig;
    LfoSettings s;
    s.shape = LfoShape::Square;
    s.sync = false;
    s.rateHz = 0.1f;          // slow: stays in the square's high half
    s.delaySeconds = 0.1f;
    s.riseSeconds = 0.1f;
    LfoState lfo;
    lfo.start(s, 0.0, 1);
    lfo.configure(s, 48000.0, 120.0);
    lfo.advance(2400, rig.tables);   // 50 ms: still delayed
    CHECK(lfo.value() == Approx(0.0f).margin(1e-3));
    lfo.advance(4800, rig.tables);   // 150 ms: halfway through the rise
    CHECK(lfo.value() == Approx(0.5f).margin(0.05f));
    lfo.advance(4800, rig.tables);   // 250 ms: fully risen
    CHECK(lfo.value() == Approx(1.0f).margin(0.02f));
}

TEST_CASE("chaos LFOs stay bounded and keep moving", "[mod][lfo][chaos]")
{
    LfoRig rig;
    for (LfoShape shape : {LfoShape::Lorenz, LfoShape::Rossler}) {
        LfoSettings s;
        s.shape = shape;
        s.sync = false;
        s.rateHz = 4.0f;
        LfoState lfo;
        lfo.start(s, 0.0, 42);
        lfo.configure(s, 48000.0, 120.0);
        double sum = 0.0, sumSq = 0.0;
        const int n = 3000;
        for (int i = 0; i < n; ++i) {
            lfo.advance(32, rig.tables);
            const float v = lfo.value();
            REQUIRE(std::isfinite(v));
            REQUIRE(v >= 0.0f);
            REQUIRE(v <= 1.0f);
            sum += v;
            sumSq += static_cast<double>(v) * v;
        }
        const double mean = sum / n, sd = std::sqrt(sumSq / n - mean * mean);
        INFO(modulation::kLfoShapeNames[static_cast<int>(shape)] << " sd " << sd);
        CHECK(sd > 0.05);
    }
}

TEST_CASE("sample & hold changes once per cycle", "[mod][lfo]")
{
    LfoRig rig;
    LfoSettings s;
    s.shape = LfoShape::SampleHold;
    s.sync = false;
    s.rateHz = 100.0f;   // 480 samples per cycle
    LfoState lfo;
    lfo.start(s, 0.0, 7);
    lfo.configure(s, 48000.0, 120.0);
    int changes = 0;
    float prev = -1.0f;
    for (int i = 0; i < 4800; ++i) {
        lfo.advance(1, rig.tables);
        if (lfo.value() != prev) { ++changes; prev = lfo.value(); }
    }
    CHECK(changes >= 9);
    CHECK(changes <= 11);
}

// --- Matrix in the engine ----------------------------------------------------------------------------

TEST_CASE("engine registers envelopes 1-4, LFOs, macros and 64 slots", "[mod][engine]")
{
    Rig rig;
    for (int e = 0; e < 4; ++e) CHECK(rig.cm->findParamRegistry("Env" + std::to_string(e)) != nullptr);
    for (int l = 0; l < 10; ++l) CHECK(rig.cm->findParamRegistry("LFO" + std::to_string(l)) != nullptr);
    for (int m = 0; m < 8; ++m) CHECK(rig.cm->findParamRegistry("Macro" + std::to_string(m)) != nullptr);
    for (int k = 0; k < 64; ++k) CHECK(rig.cm->findParamRegistry("ModSlot" + std::to_string(k)) != nullptr);
    CHECK(rig.reg("ModSlot0").find("amount")->automatable);
    CHECK_FALSE(rig.reg("ModSlot0").find("source")->automatable);
}

TEST_CASE("a fixed source adds to the destination in normalized units", "[mod][matrix]")
{
    Rig rig;
    sineVoice(rig);
    rig.reg("Oscillator0").set("level", 0.4f);
    rig.block({midiEvent(0, 0x90, 69, 100)});
    const float base = peakOver(rig, 4);

    Rig mod;
    sineVoice(mod);
    mod.reg("Oscillator0").set("level", 0.4f);
    route(mod, 0, Source::Fixed, "Oscillator0.level", 0.4f);   // 0.4 + 0.4 = 0.8
    mod.block({midiEvent(0, 0x90, 69, 100)});
    CHECK(peakOver(mod, 4) / base == Approx(2.0f).margin(0.01f));
}

TEST_CASE("slot switches: bypass, output scale, bipolar, aux", "[mod][matrix]")
{
    auto ratio = [](auto configure) {
        Rig rig;
        sineVoice(rig);
        rig.reg("Oscillator0").set("level", 0.4f);
        route(rig, 0, Source::Fixed, "Oscillator0.level", 0.4f);
        configure(rig.reg("ModSlot0"));
        rig.engine.publishSnapshot();
        rig.block({midiEvent(0, 0x90, 69, 100)});
        return peakOver(rig, 4) / (0.4f * 0.75f);   // relative to the unmodulated level
    };
    CHECK(ratio([](ParamRegistry& s) { s.set("bypass", true); }) == Approx(1.0f).margin(0.01f));
    CHECK(ratio([](ParamRegistry& s) { s.set("output", 0.5f); }) == Approx(1.5f).margin(0.01f));
    CHECK(ratio([](ParamRegistry& s) { s.set("bipolar", true); }) == Approx(2.0f).margin(0.01f));   // fixed 1 → +1
    CHECK(ratio([](ParamRegistry& s) {   // aux = 1 - Fixed = 0 at full depth → no modulation
        s.set("aux", static_cast<int>(Source::Fixed));
        s.set("auxInvert", true);
    }) == Approx(1.0f).margin(0.01f));
    CHECK(ratio([](ParamRegistry& s) {   // half depth → half the modulation
        s.set("aux", static_cast<int>(Source::Fixed));
        s.set("auxInvert", true);
        s.set("auxAmount", 0.5f);
    }) == Approx(1.5f).margin(0.01f));
}

TEST_CASE("unknown or non-modulatable destinations are ignored", "[mod][matrix]")
{
    for (const char* dest : {"Nope.thing", "Oscillator0.warp1Mode", "", "Oscillator0.remapCurve"}) {
        Rig rig;
        sineVoice(rig);
        route(rig, 0, Source::Fixed, dest, 1.0f);
        rig.block({midiEvent(0, 0x90, 69, 100)});
        INFO(dest);
        CHECK(peakOver(rig, 4) == Approx(0.5625f).margin(0.01f));
    }
}

TEST_CASE("velocity modulates per voice", "[mod][matrix]")
{
    auto peakFor = [](int velocity) {
        Rig rig;
        sineVoice(rig);
        rig.reg("Oscillator0").set("level", 0.0f);
        route(rig, 0, Source::Velocity, "Oscillator0.level", 1.0f);
        rig.block({midiEvent(0, 0x90, 69, static_cast<std::uint8_t>(velocity))});
        return peakOver(rig, 4);
    };
    CHECK(peakFor(127) == Approx(0.75f).margin(0.01f));
    CHECK(peakFor(64) == Approx(0.75f * 64.0f / 127.0f).margin(0.01f));
}

TEST_CASE("macros are sources and destinations", "[mod][matrix][macro]")
{
    Rig rig;
    sineVoice(rig);
    rig.reg("Oscillator0").set("level", 0.0f);
    route(rig, 0, Source::Macro1, "Oscillator0.level", 1.0f);
    route(rig, 1, Source::Fixed, "Macro0.value", 0.5f);   // Macro 1 = knob 0 + 0.5
    rig.block({midiEvent(0, 0x90, 69, 100)});
    CHECK(peakOver(rig, 4) == Approx(0.5f * 0.75f).margin(0.01f));

    rig.reg("Macro0").set("value", 0.25f);   // knob + modulation = 0.75
    rig.block();
    CHECK(peakOver(rig, 4) == Approx(0.75f * 0.75f).margin(0.01f));
}

TEST_CASE("mod wheel and aftertouch are global sources", "[mod][matrix]")
{
    Rig rig;
    sineVoice(rig);
    rig.reg("Oscillator0").set("level", 0.0f);
    route(rig, 0, Source::ModWheel, "Oscillator0.level", 1.0f);
    rig.block({midiEvent(0, 0x90, 69, 100)});
    CHECK(peakOver(rig, 2) == Approx(0.0f).margin(1e-4));
    rig.block({midiEvent(0, 0xB0, 1, 127)});
    CHECK(peakOver(rig, 2) == Approx(0.75f).margin(0.01f));
}

TEST_CASE("Env2 drives a destination over time", "[mod][matrix][env]")
{
    Rig rig;
    sineVoice(rig);
    rig.reg("Oscillator0").set("level", 0.0f);
    auto& env = rig.reg("Env1");
    env.set("attack", 0.1f);   // 100 ms linear rise
    route(rig, 0, Source::Env2, "Oscillator0.level", 1.0f);
    rig.block({midiEvent(0, 0x90, 69, 100)});
    const float early = peakOver(rig, 1);    // 11-21 ms
    for (int b = 0; b < 8; ++b) rig.block(); // past the 100 ms attack
    const float later = peakOver(rig, 2);
    CHECK(early < 0.25f);
    CHECK(later > 0.7f);
}

TEST_CASE("an LFO on the filter cutoff modulates the sound", "[mod][matrix][lfo]")
{
    auto render = [](bool modulated) {
        Rig rig;
        rig.reg("Oscillator0").set("random", 0.0f);
        rig.reg("Filter0").set("enabled", true);
        rig.reg("Filter0").set("cutoff", 300.0f);
        auto& lfo = rig.reg("LFO0");
        lfo.set("sync", false);
        lfo.set("rate", 5.0f);
        lfo.set("shape", static_cast<int>(LfoShape::Sine));
        if (modulated) route(rig, 0, Source::Lfo1, "Filter0.cutoff", 0.5f);
        return rig.renderNote(48, 0.5);
    };
    const auto plain = render(false), wobble = render(true);
    double diff = 0.0;
    for (std::size_t i = 0; i < plain.size(); ++i) diff = std::max(diff, static_cast<double>(std::abs(plain[i] - wobble[i])));
    CHECK(diff > 0.05);
}

TEST_CASE("audio-rate LFO FM produces sidebands at carrier +/- LFO rate", "[mod][matrix][lfo][acceptance]")
{
    Rig rig;
    sineVoice(rig);
    auto& lfo = rig.reg("LFO0");
    lfo.set("sync", false);
    lfo.set("rate", 1000.0f);
    lfo.set("shape", static_cast<int>(LfoShape::Sine));
    lfo.set("mode", static_cast<int>(LfoMode::Free));
    route(rig, 0, Source::Lfo1, "Oscillator0.coarse", 0.02f);   // ±1.92 st peak deviation
    rig.reg("ModSlot0").set("bipolar", true);
    rig.engine.publishSnapshot();
    const auto x = rig.renderNote(69, 2.1);   // carrier A4 = 440 Hz

    // Exponential (semitone) FM shifts the mean frequency slightly, so lines fall between integer bins:
    // use a Hann window and take the peak within a few bins of each expected line.
    std::vector<float> tail(x.end() - 48000, x.end());
    for (std::size_t i = 0; i < tail.size(); ++i)
        tail[i] *= static_cast<float>(0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * static_cast<double>(i) / 48000.0));
    const auto a = spectrum(tail);
    auto near = [&](int bin) {
        double m = 0.0;
        for (int k = bin - 4; k <= bin + 4; ++k) m = std::max(m, a[static_cast<std::size_t>(k)]);
        return db(m);
    };
    const double carrier = near(440), upper = near(1440), lower = near(560);
    double between = 0.0;   // where a 1.5 kHz control-rate update would alias the 1 kHz modulator (440 ± 500)
    for (int k = 600; k < 1400; ++k) between = std::max(between, a[static_cast<std::size_t>(k)]);
    INFO("carrier " << carrier << " dB, sidebands " << lower << " / " << upper << " dB, between " << db(between) << " dB");
    CHECK(upper > carrier - 40.0);
    CHECK(lower > carrier - 40.0);
    CHECK(db(between) < upper - 20.0);
}

TEST_CASE("pitch bend moves every oscillator by the bend range", "[mod][engine]")
{
    Rig rig;
    sineVoice(rig);
    rig.block({midiEvent(0, 0xE0, 0x7F, 0x7F), midiEvent(1, 0x90, 69, 100)});   // full bend up before the note
    std::vector<float> x;
    for (int b = 0; b < 94; ++b) { rig.block(); x.insert(x.end(), rig.l.begin(), rig.l.end()); }
    int crossings = 0;
    for (std::size_t i = 1; i < x.size(); ++i) crossings += (x[i - 1] <= 0.0f && x[i] > 0.0f) ? 1 : 0;
    CHECK(std::abs(crossings - 495) <= 3);   // 440·2^(2/12) = 493.9 Hz over ~1.003 s
}

TEST_CASE("envelope curve parameters reshape the decay", "[mod][env]")
{
    auto levelMidDecay = [](float curve) {
        Rig rig;
        sineVoice(rig);
        auto& env = rig.reg("Env0");
        env.set("decay", 0.2f);
        env.set("sustain", 0.0f);
        env.set("decayCurve", curve);
        rig.block({midiEvent(0, 0x90, 69, 100)});
        for (int b = 0; b < 8; ++b) rig.block();   // ~96 ms ≈ halfway
        return peakOver(rig, 1);
    };
    CHECK(levelMidDecay(0.0f) > levelMidDecay(-5.0f) + 0.1f);   // linear decays slower than fast-start
}

TEST_CASE("the remap curve shapes Remap 1", "[mod][warp]")
{
    auto render = [](const char* curve) {
        Rig rig;
        sineVoice(rig);
        auto& osc = rig.reg("Oscillator0");
        osc.set("warp1Mode", static_cast<int>(dsp::WarpMode::Remap1));
        osc.set("warp1Amount", 1.0f);
        osc.set<std::string>("remapCurve", curve);
        rig.engine.publishSnapshot();
        return rig.renderNote(69, 0.05);
    };
    const auto identity = render("0,0,0;1,1,0");
    const auto bent = render("0,0,0;0.5,0.8,0;1,1,0");
    double diff = 0.0;
    for (std::size_t i = 0; i < identity.size(); ++i) diff = std::max(diff, static_cast<double>(std::abs(identity[i] - bent[i])));
    CHECK(diff > 0.05);
}

TEST_CASE("the LFO path string defines the Path shape", "[mod][lfo]")
{
    auto peakFor = [](const char* path) {
        Rig rig;
        sineVoice(rig);
        rig.reg("Oscillator0").set("level", 0.0f);
        rig.reg("LFO0").set<std::string>("path", path);
        route(rig, 0, Source::Lfo1, "Oscillator0.level", 1.0f);
        rig.block({midiEvent(0, 0x90, 69, 100)});
        return peakOver(rig, 2);
    };
    CHECK(peakFor("0,0,0;1,0,0") == Approx(0.0f).margin(0.01f));    // flat zero
    CHECK(peakFor("0,1,0;1,1,0") == Approx(0.75f).margin(0.02f));   // flat one
}
