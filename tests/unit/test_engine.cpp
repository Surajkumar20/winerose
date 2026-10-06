#include "EngineRig.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace winerose;
using winerose::test::midiEvent;
using winerose::test::Rig;
using Catch::Approx;

TEST_CASE("engine registers its Phase 1 modules", "[engine]")
{
    Rig rig;
    for (const char* name : {"Global", "Oscillator0", "Filter0", "Env0"})
        CHECK(rig.cm->findParamRegistry(name) != nullptr);
    CHECK(rig.reg("Global").get<Quality>("quality") == Quality::Good);
    CHECK(rig.reg("Global").get<int>("polyphony") == 16);
    CHECK(rig.reg("Oscillator0").get<bool>("enabled"));
    CHECK_FALSE(rig.reg("Filter0").get<bool>("enabled"));
}

TEST_CASE("two engines never share parameter namespaces", "[engine]")
{
    auto a = std::make_shared<ConfigManager>();
    auto b = std::make_shared<ConfigManager>();
    Engine ea(a), eb(b);
    a->findParamRegistry("Global")->set("masterVolume", 0.1f);
    CHECK(b->findParamRegistry("Global")->get<float>("masterVolume") == 0.75f);
}

TEST_CASE("silent without notes, for any block size", "[engine]")
{
    Rig rig;
    std::vector<float> l(512, 1.0f), r(512, 1.0f);
    float* chans[] = {l.data(), r.data()};
    for (int n : {1, 7, 64, 511, 512}) {
        rig.engine.process(chans, 2, n, nullptr, 0, TransportInfo{});
        for (int i = 0; i < n; ++i) REQUIRE(l[static_cast<std::size_t>(i)] == 0.0f);
    }
    CHECK(rig.engine.meters().peakLeft.load() == 0.0f);
}

TEST_CASE("a note sounds, then decays to silence after its release", "[engine]")
{
    Rig rig;
    CHECK(rig.block({midiEvent(0, 0x90, 60, 100)}) > 0.1f);
    CHECK(rig.engine.activeVoiceCount() == 1);
    CHECK(rig.engine.meters().peakLeft.load() > 0.1f);

    rig.block({midiEvent(0, 0x80, 60, 0)});
    for (int i = 0; i < 10; ++i) rig.block();   // default release 15 ms ≪ 10 blocks
    CHECK(rig.block() == 0.0f);
    CHECK(rig.engine.activeVoiceCount() == 0);
}

TEST_CASE("note-on with velocity 0 is a note-off", "[engine]")
{
    Rig rig;
    rig.block({midiEvent(0, 0x90, 60, 100)});
    rig.block({midiEvent(0, 0x90, 60, 0)});
    for (int i = 0; i < 10; ++i) rig.block();
    CHECK(rig.engine.activeVoiceCount() == 0);
}

TEST_CASE("events land on their exact sample", "[engine]")
{
    Rig rig;
    rig.block({midiEvent(300, 0x90, 69, 100)});
    for (std::size_t i = 0; i < 300; ++i) REQUIRE(rig.l[i] == 0.0f);
    float after = 0.0f;
    for (std::size_t i = 300; i < 512; ++i) after = std::max(after, std::abs(rig.l[i]));
    CHECK(after > 0.1f);
}

TEST_CASE("polyphony limit steals the oldest voice", "[engine]")
{
    Rig rig;
    rig.reg("Global").set("polyphony", 4);
    std::vector<MidiEvent> notes;
    for (int n = 0; n < 6; ++n) notes.push_back(midiEvent(n * 10, 0x90, static_cast<std::uint8_t>(60 + n), 100));
    rig.block(notes);
    CHECK(rig.engine.activeVoiceCount() == 4);
}

TEST_CASE("sustain pedal holds released notes until it lifts", "[engine]")
{
    Rig rig;
    rig.block({midiEvent(0, 0xB0, 64, 127), midiEvent(10, 0x90, 60, 100), midiEvent(20, 0x80, 60, 0)});
    for (int i = 0; i < 10; ++i) rig.block();
    CHECK(rig.engine.activeVoiceCount() == 1);
    rig.block({midiEvent(0, 0xB0, 64, 0)});
    for (int i = 0; i < 10; ++i) rig.block();
    CHECK(rig.engine.activeVoiceCount() == 0);
}

TEST_CASE("all-sound-off silences immediately", "[engine]")
{
    Rig rig;
    rig.block({midiEvent(0, 0x90, 60, 100), midiEvent(0, 0x90, 64, 100)});
    CHECK(rig.engine.activeVoiceCount() == 2);
    rig.block({midiEvent(0, 0xB0, 120, 0)});
    CHECK(rig.engine.activeVoiceCount() == 0);
}

TEST_CASE("the filter removes high-frequency energy when enabled", "[engine]")
{
    auto energyAbove = [](bool filterOn) {
        Rig rig;
        rig.reg("Oscillator0").set("random", 0.0f);
        rig.reg("Filter0").set("enabled", filterOn);
        rig.reg("Filter0").set("cutoff", 200.0f);
        rig.block({midiEvent(0, 0x90, 48, 100)});
        // First difference ≈ high-pass: its energy tracks the high-frequency content.
        double e = 0.0;
        for (int b = 0; b < 4; ++b) {
            rig.block();
            for (std::size_t i = 1; i < 512; ++i) e += std::pow(rig.l[i] - rig.l[i - 1], 2.0);
        }
        return e;
    };
    CHECK(energyAbove(true) < energyAbove(false) * 0.05);
}

TEST_CASE("oscillator pitch follows octave/semi/fine", "[engine]")
{
    // Count upward zero crossings of a sine (frame 3) over one second: equals the frequency.
    auto measuredHz = [](int octave, int semi, float fine) {
        Rig rig;
        auto& osc = rig.reg("Oscillator0");
        osc.set("wtPos", 1.0f);          // last frame = sine
        osc.set("random", 0.0f);
        osc.set("octave", octave);
        osc.set("semi", semi);
        osc.set("fine", fine);
        rig.reg("Env0").set("attack", 0.0f);
        int crossings = 0;
        float prev = 0.0f;
        for (int b = 0; b < 94; ++b) {   // 94 · 512 ≈ 1.003 s
            rig.block(b == 0 ? std::vector<MidiEvent>{midiEvent(0, 0x90, 69, 100)} : std::vector<MidiEvent>{});
            for (std::size_t i = 0; i < 512; ++i) {
                if (prev <= 0.0f && rig.l[i] > 0.0f) ++crossings;
                prev = rig.l[i];
            }
        }
        return crossings;
    };
    CHECK(std::abs(measuredHz(0, 0, 0.0f) - 441) <= 2);    // A4 = 440 Hz over ~1.003 s
    CHECK(std::abs(measuredHz(1, 0, 0.0f) - 883) <= 3);
    CHECK(std::abs(measuredHz(0, 7, 0.0f) - 661) <= 3);    // a fifth up
    CHECK(std::abs(measuredHz(0, 0, 100.0f) - 467) <= 3);  // +100 cents = one semitone
}

TEST_CASE("published wavetables take effect at the next block", "[engine]")
{
    Rig rig;
    std::vector<float> silent(2048, 0.0f);
    rig.engine.setOscillatorTable(0, dsp::WavetableBank::build(silent, 2048, "silence"));
    CHECK(rig.block({midiEvent(0, 0x90, 60, 100)}) == 0.0f);
    rig.engine.setOscillatorTable(0, dsp::makeBasicShapesTable());
    CHECK(rig.block() > 0.1f);
}
