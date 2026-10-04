// CPU benchmarks (SPEC §5.4 Phase 2 acceptance: 16 voices × 3 oscillators × 7 unison < 25% of one core at
// 48 kHz / 128-sample blocks). Prints the realtime load for each scenario; the threshold is only asserted
// in optimized builds (Debug numbers are meaningless).

#include "engine/Engine.h"
#include "engine/dsp/Warp.h"
#include "engine/dsp/WavetableBank.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace winerose;

namespace {

MidiEvent noteOn(int note)
{
    MidiEvent e;
    e.data[0] = 0x90;
    e.data[1] = static_cast<std::uint8_t>(note);
    e.data[2] = 100;
    e.size = 3;
    return e;
}

struct Scenario {
    std::string name;
    int   unison;
    int   warp;      // dsp::WarpMode
    int   quality;   // 0 Good, 1 High, 2 Ultra
    bool  filter;
};

// Returns the fraction of one core used to render in realtime.
double measureLoad(const Scenario& sc, double seconds)
{
    constexpr double kRate = 48000.0;
    constexpr int kBlock = 128;
    auto cm = std::make_shared<ConfigManager>();
    Engine engine(cm);
    for (int o = 0; o < 3; ++o) {
        auto& osc = *cm->findParamRegistry("Oscillator" + std::to_string(o));
        osc.set("enabled", true);
        osc.set("unison", sc.unison);
        osc.set("wtPos", 0.3f + 0.2f * o);   // between frames: exercises the frame morph
        osc.set("warp1Mode", sc.warp);
        osc.set("warp1Amount", 0.5f);
    }
    cm->findParamRegistry("Global")->set("quality", sc.quality);
    cm->findParamRegistry("Filter0")->set("enabled", sc.filter);
    cm->findParamRegistry("Env0")->set("sustain", 1.0f);
    engine.prepare(kRate, kBlock);

    std::vector<float> l(kBlock), r(kBlock);
    float* chans[] = {l.data(), r.data()};
    std::vector<MidiEvent> chord;
    for (int n = 0; n < 16; ++n) chord.push_back(noteOn(36 + n * 3));
    engine.process(chans, 2, kBlock, chord.data(), static_cast<int>(chord.size()), TransportInfo{});
    REQUIRE(engine.activeVoiceCount() == 16);

    const int blocks = static_cast<int>(seconds * kRate / kBlock);
    const auto start = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b) engine.process(chans, 2, kBlock, nullptr, 0, TransportInfo{});
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return elapsed / (blocks * kBlock / kRate);
}

} // namespace

TEST_CASE("Phase 2 CPU budget: 16 voices x 3 osc x 7 unison", "[benchmark]")
{
    const std::vector<Scenario> scenarios = {
        {"16 voices x 3 osc x 7 unison (acceptance)", 7, 0, 0, false},
        {"  + filter", 7, 0, 0, true},
        {"  + Sync warp, Good", 7, static_cast<int>(dsp::WarpMode::Sync), 0, true},
        {"  + Sync warp, Ultra (4x)", 7, static_cast<int>(dsp::WarpMode::Sync), 2, true},
        {"16 voices x 3 osc x 16 unison", 16, 0, 0, true},
    };
#ifdef NDEBUG
    constexpr double kSeconds = 4.0;
#else
    constexpr double kSeconds = 0.5;
#endif
    double acceptance = 0.0;
    for (const auto& sc : scenarios) {
        const double load = measureLoad(sc, kSeconds);
        std::printf("%-46s %6.1f %% of one core\n", sc.name.c_str(), load * 100.0);
        if (&sc == &scenarios.front()) acceptance = load;
    }
#ifdef NDEBUG
    CHECK(acceptance < 0.25);
#else
    WARN("Debug build: CPU threshold not asserted");
    (void)acceptance;
#endif
}

TEST_CASE("micro: wavetable read cost", "[benchmark][micro]")
{
    const auto bank = dsp::makeBasicShapesTable();
    constexpr int kReads = 20'000'000;
    struct Case { const char* name; float frame; double inc; float forceBlend; };
    const Case cases[] = {
        {"slow, exact, no blend",  1.0f, 1.0 / 2048.0, 0.0f},
        {"slow, morph, no blend",  1.4f, 1.0 / 2048.0, 0.0f},
        {"slow, morph, blend",     1.4f, 1.0 / 2048.0, 0.5f},
        {"fast, exact, no blend",  1.0f, 0.00731, 0.0f},
        {"fast, morph, no blend",  1.4f, 0.00731, 0.0f},
        {"fast, morph, blend",     1.4f, 0.00731, 0.5f},
    };
    for (const auto& c : cases) {
        auto lc = dsp::WavetableBank::selectLevel(c.inc);
        lc.blend = c.forceBlend;
        const auto fp = bank->resolveFrame(c.frame);
        double phase = 0.0, sink = 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kReads; ++i) {
            sink += bank->read(phase, fp, lc);
            phase += c.inc;
            if (phase >= 1.0) phase -= 1.0;
        }
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / kReads;
        std::printf("%-30s blend=%.2f  %5.2f ns/read  (sink %g)\n", c.name, lc.blend, ns, sink);
    }
}
