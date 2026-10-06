// Engine Phase 2 (SPEC §5.4): oscillators B/C, noise, sub, unison, phase/interpolation options, warps
// with Quality oversampling.

#include "EngineRig.h"

#include "engine/dsp/Fft.h"
#include "engine/dsp/Warp.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace winerose;
using winerose::test::midiEvent;
using winerose::test::Rig;
using Catch::Approx;

namespace {

double rms(const std::vector<float>& x, std::size_t from)
{
    double s = 0.0;
    for (std::size_t i = from; i < x.size(); ++i) s += static_cast<double>(x[i]) * x[i];
    return std::sqrt(s / static_cast<double>(x.size() - from));
}

int upwardCrossings(const std::vector<float>& x)
{
    int n = 0;
    for (std::size_t i = 1; i < x.size(); ++i) n += (x[i - 1] <= 0.0f && x[i] > 0.0f) ? 1 : 0;
    return n;
}

double maxDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    double d = 0.0;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) d = std::max(d, static_cast<double>(std::abs(a[i] - b[i])));
    return d;
}

} // namespace

TEST_CASE("engine registers oscillators A/B/C, noise and sub", "[engine][phase2]")
{
    Rig rig;
    for (const char* name : {"Oscillator0", "Oscillator1", "Oscillator2", "Oscillator3", "Oscillator4"})
        CHECK(rig.cm->findParamRegistry(name) != nullptr);
    CHECK(rig.reg("Oscillator0").get<bool>("enabled"));
    CHECK_FALSE(rig.reg("Oscillator1").get<bool>("enabled"));
    CHECK_FALSE(rig.reg("Oscillator3").get<bool>("enabled"));   // noise
    CHECK_FALSE(rig.reg("Oscillator4").get<bool>("enabled"));   // sub
}

TEST_CASE("oscillators B and C sound when enabled on their own", "[engine][phase2]")
{
    for (const char* osc : {"Oscillator1", "Oscillator2"}) {
        Rig rig;
        rig.reg("Oscillator0").set("enabled", false);
        rig.reg(osc).set("enabled", true);
        INFO(osc);
        CHECK(rig.block({midiEvent(0, 0x90, 60, 100)}) > 0.1f);
    }
}

TEST_CASE("sub oscillator follows its octave setting", "[engine][phase2]")
{
    Rig rig;
    rig.reg("Oscillator0").set("enabled", false);
    auto& sub = rig.reg("Oscillator4");
    sub.set("enabled", true);
    sub.set("octave", -1);   // shape 0 = sine
    const auto x = rig.renderNote(69, 1.0);   // A4 → 220 Hz
    CHECK(std::abs(upwardCrossings(x) - 220) <= 2);
}

TEST_CASE("noise oscillator produces output of every colour", "[engine][phase2]")
{
    for (int type = 0; type < 3; ++type) {
        Rig rig;
        rig.reg("Oscillator0").set("enabled", false);
        rig.reg("Oscillator3").set("enabled", true);
        rig.reg("Oscillator3").set("type", type);
        const auto x = rig.renderNote(60, 0.5);
        INFO("type " << type);
        CHECK(rms(x, 1000) > 0.05);
        CHECK(rms(x, 1000) < 0.5);
    }
}

TEST_CASE("unison keeps the level roughly constant and widens the stereo image", "[engine][phase2]")
{
    struct Result { double level, sideRatio; };
    auto measure = [](int voices, float width) {
        Rig rig;
        auto& osc = rig.reg("Oscillator0");
        osc.set("unison", voices);
        osc.set("uniWidth", width);
        double mid = 0.0, side = 0.0, total = 0.0;
        for (int b = 0; b < 40; ++b) {
            if (b == 0) rig.block({midiEvent(0, 0x90, 48, 100)});
            else        rig.block();
            if (b < 4) continue;
            for (std::size_t i = 0; i < 512; ++i) {
                mid   += std::pow(rig.l[i] + rig.r[i], 2.0);
                side  += std::pow(rig.l[i] - rig.r[i], 2.0);
                total += 0.5 * (rig.l[i] * rig.l[i] + rig.r[i] * rig.r[i]);
            }
        }
        return Result{total, side / mid};
    };
    const Result one = measure(1, 1.0f), seven = measure(7, 1.0f), narrow = measure(7, 0.0f);
    const double db = 10.0 * std::log10(seven.level / one.level);
    INFO("unison 7 vs 1: " << db << " dB");
    CHECK(std::abs(db) < 4.0);
    CHECK(one.sideRatio < 1e-9);       // single centred voice: L == R
    CHECK(seven.sideRatio > 0.05);     // wide unison has real side content
    CHECK(narrow.sideRatio < 1e-9);    // width 0 collapses to mono
}

TEST_CASE("start phase sets the first sample", "[engine][phase2]")
{
    Rig rig;
    auto& osc = rig.reg("Oscillator0");
    osc.set("wtPos", 1.0f);   // sine
    osc.set("random", 0.0f);
    osc.set("phase", 0.25f);
    rig.reg("Env0").set("attack", 0.0f);
    rig.block({midiEvent(0, 0x90, 69, 100)});
    CHECK(rig.l[0] == Approx(0.75f * 0.75f).margin(1e-3));   // sin(90°) · level · master
}

TEST_CASE("Mem phase continues from the voice's previous note", "[engine][phase2]")
{
    Rig rig;
    rig.reg("Global").set("polyphony", 1);   // reuse the same voice slot
    auto& osc = rig.reg("Oscillator0");
    osc.set("wtPos", 1.0f);
    osc.set("random", 0.0f);
    osc.set("phase", 1.0f);                  // 100% = Mem
    rig.reg("Env0").set("attack", 0.0f);
    rig.reg("Env0").set("release", 0.0f);
    rig.block({midiEvent(0, 0x90, 69, 100), midiEvent(100, 0x80, 69, 0)});
    const float beforeRetrigger = rig.l[99];
    rig.block({midiEvent(0, 0x90, 69, 100)});
    // A reset phase would start at sin(0)=0; Mem continues near where the last note stopped.
    CHECK(std::abs(rig.l[0]) > 0.05f);
    CHECK(std::abs(beforeRetrigger) > 0.0f);
}

TEST_CASE("smooth interpolation off snaps to the nearest frame", "[engine][phase2]")
{
    auto render = [](float wtPos, bool smooth) {
        Rig rig;
        auto& osc = rig.reg("Oscillator0");
        osc.set("random", 0.0f);
        osc.set("wtPos", wtPos);
        osc.set("wtSmooth", smooth);
        return rig.renderNote(60, 0.05);
    };
    const auto frame2 = render(2.0f / 3.0f, true);   // exactly frame 2 of 0..3
    CHECK(maxDifference(render(0.6f, false), frame2) < 1e-6);   // frame 1.8 → 2
    CHECK(maxDifference(render(0.6f, true), frame2) > 1e-3);
}

TEST_CASE("Quality oversampling reduces aliasing from a sync warp", "[engine][phase2]")
{
    // A6 = 1760 Hz exactly: over 1 s at 48 kHz every harmonic and alias lands on an integer bin.
    auto worstAliasDb = [](int quality) {
        Rig rig;
        rig.reg("Global").set("quality", quality);
        auto& osc = rig.reg("Oscillator0");
        osc.set("warp1Mode", static_cast<int>(dsp::WarpMode::Sync));
        osc.set("warp1Amount", 0.7f);
        const auto x = rig.renderNote(93, 2.1);
        std::vector<float> tail(x.end() - 48000, x.end());
        dsp::RealFft fft(48000);
        std::vector<float> s(48000);
        fft.forward(tail.data(), s.data());
        double worst = 0.0;
        for (int k = 1; k < 24000; ++k) {
            if (k % 1760 == 0) continue;
            const double a = 2.0 * std::hypot(s[static_cast<std::size_t>(2 * k)], s[static_cast<std::size_t>(2 * k + 1)]) / 48000.0;
            worst = std::max(worst, a);
        }
        return 20.0 * std::log10(worst);
    };
    const double good = worstAliasDb(0), ultra = worstAliasDb(2);
    INFO("sync warp worst alias: Good " << good << " dB, Ultra " << ultra << " dB");
    CHECK(ultra < good - 12.0);
}

TEST_CASE("FM warp from B changes A only when B is running", "[engine][phase2]")
{
    auto render = [](bool fm, bool bOn) {
        Rig rig;
        auto& a = rig.reg("Oscillator0");
        a.set("random", 0.0f);
        if (fm) {
            a.set("warp1Mode", static_cast<int>(dsp::WarpMode::Fm));
            a.set("warp1Amount", 0.5f);
        }
        auto& b = rig.reg("Oscillator1");
        b.set("enabled", bOn);
        b.set("level", 0.0f);   // B is silent in the mix: only its modulation is heard
        b.set("random", 0.0f);
        return rig.renderNote(60, 0.1);
    };
    const auto reference = render(false, false);
    CHECK(maxDifference(render(true, false), reference) < 1e-6);   // no modulator → no change
    CHECK(maxDifference(render(true, true), reference) > 0.05);
}
