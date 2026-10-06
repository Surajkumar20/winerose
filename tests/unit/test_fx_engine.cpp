// FX racks inside the engine: slot types, Main/Bus/Direct routing, mixer levels, block-size invariance.

#include "EngineRig.h"

#include "engine/fx/Effect.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace winerose;
using winerose::test::midiEvent;
using winerose::test::Rig;
using fx::FxType;
using Catch::Approx;

namespace {

double rmsOf(const std::vector<float>& x, std::size_t from = 0)
{
    double s = 0.0;
    for (std::size_t i = from; i < x.size(); ++i) s += static_cast<double>(x[i]) * x[i];
    return std::sqrt(s / static_cast<double>(x.size() - from));
}

ParamRegistry& slot(Rig& rig, int rack, int s) { return rig.reg("FXRack" + std::to_string(rack) + "Slot" + std::to_string(s)); }

void setFx(Rig& rig, int rack, int s, FxType type, std::initializer_list<std::pair<int, float>> params = {})
{
    auto& r = slot(rig, rack, s);
    const auto& defaults = fx::defaultParams(type);
    for (int i = 0; i < fx::kParamCount; ++i) r.set("p" + std::to_string(i), defaults[static_cast<std::size_t>(i)]);
    for (const auto& [i, v] : params) r.set("p" + std::to_string(i), v);
    r.set("type", static_cast<int>(type));   // the engine builds the effect on this change
    r.set("enabled", true);
}

constexpr float kUtilityMinus48 = 0.0f;   // utility gain knob at the bottom: -48 dB

} // namespace

TEST_CASE("engine registers 3 racks x 8 slots and the mixer", "[fx][engine]")
{
    Rig rig;
    for (int r = 0; r < fx::kRackCount; ++r)
        for (int s = 0; s < fx::kSlotsPerRack; ++s) CHECK(rig.cm->findParamRegistry("FXRack" + std::to_string(r) + "Slot" + std::to_string(s)) != nullptr);
    CHECK(rig.cm->findParamRegistry("Mixer") != nullptr);
    CHECK_FALSE(slot(rig, 0, 0).find("type")->automatable);
    CHECK(slot(rig, 0, 0).find("p0")->automatable);
}

TEST_CASE("a Main-rack effect processes the voices; type changes apply at the next block", "[fx][engine]")
{
    Rig plain;
    plain.reg("Oscillator0").set("random", 0.0f);
    const double base = rmsOf(plain.renderNote(60, 0.2), 4800);

    Rig rig;
    rig.reg("Oscillator0").set("random", 0.0f);
    setFx(rig, 0, 0, FxType::Utility, {{0, kUtilityMinus48}});
    CHECK(rmsOf(rig.renderNote(60, 0.2), 4800) < base * 0.01);

    slot(rig, 0, 0).set("type", static_cast<int>(FxType::None));
    rig.block({midiEvent(0, 0x80, 60, 0)});
    for (int b = 0; b < 4; ++b) rig.block();
    CHECK(rmsOf(rig.renderNote(60, 0.2), 4800) == Approx(base).epsilon(1e-6));
}

TEST_CASE("Direct bypasses the Main rack", "[fx][engine]")
{
    Rig rig;
    rig.reg("Oscillator0").set("random", 0.0f);
    rig.reg("Oscillator0").set("route", 2);   // Direct
    setFx(rig, 0, 0, FxType::Utility, {{0, kUtilityMinus48}});
    Rig plain;
    plain.reg("Oscillator0").set("random", 0.0f);
    CHECK(rmsOf(rig.renderNote(60, 0.2), 4800) == Approx(rmsOf(plain.renderNote(60, 0.2), 4800)).epsilon(1e-6));
}

TEST_CASE("bus sends reach their racks and the mixer levels", "[fx][engine]")
{
    auto level = [](float send, float busLevel, bool quietBusFx) {
        Rig rig;
        rig.reg("Oscillator0").set("random", 0.0f);
        rig.reg("Oscillator0").set("bus1Send", send);
        rig.reg("Mixer").set("bus1Level", busLevel);
        if (quietBusFx) setFx(rig, 1, 0, FxType::Utility, {{0, kUtilityMinus48}});
        return rmsOf(rig.renderNote(60, 0.2), 4800);
    };
    const double dry = level(0.0f, 1.0f, false);
    CHECK(level(1.0f, 1.0f, false) == Approx(2.0 * dry).epsilon(1e-4));   // main + an equal send
    CHECK(level(1.0f, 0.0f, false) == Approx(dry).epsilon(1e-6));         // bus muted at the mixer
    CHECK(level(1.0f, 1.0f, true) == Approx(dry).epsilon(0.01));          // bus FX turns its copy down
}

TEST_CASE("a reverb tail rings on after the note ends", "[fx][engine]")
{
    Rig rig;
    setFx(rig, 0, 0, FxType::Reverb, {{1, 0.6f}});   // ~2.4 s RT60
    slot(rig, 0, 0).set("mix", 0.5f);
    rig.block({midiEvent(0, 0x90, 60, 100)});
    for (int b = 0; b < 20; ++b) rig.block();
    rig.block({midiEvent(0, 0x80, 60, 0)});
    for (int b = 0; b < 10; ++b) rig.block();   // the voice is long gone (15 ms release)
    CHECK(rig.engine.activeVoiceCount() == 0);
    CHECK(rig.block() > 0.01f);
}

TEST_CASE("FX output does not depend on the host block size", "[fx][engine]")
{
    auto render = [](int blockSize) {
        auto cm = std::make_shared<ConfigManager>();
        Engine engine(cm);
        auto set = [&](int s, FxType t, float mix) {
            auto& r = *cm->findParamRegistry("FXRack0Slot" + std::to_string(s));
            const auto& d = fx::defaultParams(t);
            for (int i = 0; i < fx::kParamCount; ++i) r.set("p" + std::to_string(i), d[static_cast<std::size_t>(i)]);
            r.set("type", static_cast<int>(t));
            r.set("enabled", true);
            r.set("mix", mix);
        };
        set(0, FxType::Distortion, 1.0f);
        set(1, FxType::Chorus, 0.5f);
        set(2, FxType::Delay, 0.3f);
        set(3, FxType::Reverb, 0.3f);
        engine.prepare(48000.0, blockSize);
        std::vector<float> out;
        std::vector<float> l(static_cast<std::size_t>(blockSize)), r(static_cast<std::size_t>(blockSize));
        float* ch[] = {l.data(), r.data()};
        const int total = 9600;
        for (int pos = 0; pos < total; pos += blockSize) {
            MidiEvent on = midiEvent(0, 0x90, 57, 100);
            const bool first = pos == 0;
            engine.process(ch, 2, blockSize, first ? &on : nullptr, first ? 1 : 0, TransportInfo{});
            out.insert(out.end(), l.begin(), l.end());
        }
        out.resize(total);
        return out;
    };
    const auto reference = render(512);
    for (int block : {1, 37, 64, 333}) {
        const auto y = render(block);
        double worst = 0.0;
        for (std::size_t i = 0; i < reference.size(); ++i) worst = std::max(worst, static_cast<double>(std::abs(y[i] - reference[i])));
        INFO("block " << block << " worst difference " << worst);
        CHECK(worst < 1e-6);
    }
}
