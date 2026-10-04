#include "engine/dsp/Unison.h"
#include "engine/dsp/Warp.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace winerose::dsp;
using Catch::Approx;

namespace {
WarpMode modeAt(int i) { return static_cast<WarpMode>(i); }
bool bipolar(WarpMode m) { return m == WarpMode::BendPlusMinus || m == WarpMode::AsymPlusMinus; }
}

// --- Warps: properties every mode must have -----------------------------------------------------------

TEST_CASE("every warp is neutral at its rest amount", "[warp]")
{
    for (int i = 0; i < static_cast<int>(WarpMode::Count); ++i) {
        const WarpMode m = modeAt(i);
        const float rest = bipolar(m) ? 0.5f : 0.0f;
        for (double p = 0.0; p < 1.0; p += 0.0137) {
            const auto w = applyWarp(m, rest, p, 0.37f);
            INFO(kWarpNames[i] << " p=" << p);
            if (m == WarpMode::Quantize) REQUIRE(std::abs(w.phase - p) < 1.0 / 2048.0);   // 2048 steps
            else                         REQUIRE(w.phase == Approx(p).margin(1e-12));
            REQUIRE(w.amp == 1.0f);
        }
    }
}

TEST_CASE("every warp keeps the read phase in [0,1] and the gain bounded", "[warp]")
{
    std::uint32_t seed = 7;
    auto rnd = [&seed] { seed = seed * 1664525u + 1013904223u; return static_cast<double>(seed >> 8) / 16777216.0; };
    for (int i = 0; i < static_cast<int>(WarpMode::Count); ++i) {
        for (int n = 0; n < 20000; ++n) {
            const auto w = applyWarp(modeAt(i), static_cast<float>(rnd()), rnd(), static_cast<float>(rnd() * 2.0 - 1.0));
            INFO(kWarpNames[i]);
            REQUIRE(w.phase >= 0.0);
            REQUIRE(w.phase <= 1.0);
            REQUIRE(std::abs(w.amp) <= 1.0f);
            REQUIRE(std::isfinite(w.phase));
        }
    }
}

// --- Warps: per-mode behaviour (SPEC §1.2 formulas) ----------------------------------------------------

TEST_CASE("sync multiplies the phase rate up to 8x", "[warp]")
{
    CHECK(applyWarp(WarpMode::Sync, 1.0f, 0.3, 0).phase == Approx(std::fmod(0.3 * 8.0, 1.0)));
    CHECK(applyWarp(WarpMode::Sync, 0.5f, 0.2, 0).phase == Approx(std::fmod(0.2 * 4.5, 1.0)));
    CHECK(warpPitchFactor(WarpMode::Sync, 1.0f) == 8.0);
}

TEST_CASE("window sync fades each cycle in and out", "[warp]")
{
    CHECK(applyWarp(WarpMode::WindowSync, 1.0f, 0.0, 0).amp == Approx(0.0f).margin(1e-6));
    CHECK(applyWarp(WarpMode::WindowSync, 1.0f, 0.5, 0).amp == Approx(1.0f));
}

TEST_CASE("bend pinches around the middle, asym bends the whole cycle", "[warp]")
{
    CHECK(applyWarp(WarpMode::BendPlus, 1.0f, 0.25, 0).phase == Approx(0.5 * std::pow(0.5, 8.0)));
    CHECK(applyWarp(WarpMode::BendMinus, 1.0f, 0.25, 0).phase == Approx(0.5 * std::pow(0.5, 0.125)));
    CHECK(applyWarp(WarpMode::BendPlus, 1.0f, 0.5, 0).phase == Approx(0.5));   // middle is fixed
    CHECK(applyWarp(WarpMode::AsymPlus, 1.0f, 0.5, 0).phase == Approx(std::pow(0.5, 8.0)));
    CHECK(applyWarp(WarpMode::AsymMinus, 1.0f, 0.5, 0).phase == Approx(std::pow(0.5, 0.125)));
    CHECK(applyWarp(WarpMode::AsymPlusMinus, 1.0f, 0.5, 0).phase == applyWarp(WarpMode::AsymPlus, 1.0f, 0.5, 0).phase);
}

TEST_CASE("PWM squeezes the cycle and silences the rest", "[warp]")
{
    const auto early = applyWarp(WarpMode::Pwm, 0.5f, 0.2, 0);
    CHECK(early.phase == Approx(0.2 / (1.0 - 0.495)));
    CHECK(early.amp == 1.0f);
    CHECK(applyWarp(WarpMode::Pwm, 0.5f, 0.9, 0).amp == 0.0f);
}

TEST_CASE("flip inverts polarity from position 1-k", "[warp]")
{
    CHECK(applyWarp(WarpMode::Flip, 0.3f, 0.5, 0).amp == 1.0f);
    CHECK(applyWarp(WarpMode::Flip, 0.3f, 0.8, 0).amp == -1.0f);
}

TEST_CASE("mirror folds the second half back", "[warp]")
{
    CHECK(applyWarp(WarpMode::Mirror, 1.0f, 0.25, 0).phase == Approx(0.5));
    CHECK(applyWarp(WarpMode::Mirror, 1.0f, 0.75, 0).phase == Approx(0.5));
}

TEST_CASE("remaps 3 and 4 are sinusoidal; remap 1 is identity until curves exist", "[warp]")
{
    CHECK(applyWarp(WarpMode::Remap3, 1.0f, 0.5, 0).phase == Approx(0.5));
    CHECK(applyWarp(WarpMode::Remap3, 1.0f, 0.25, 0).phase == Approx(0.5 - 0.5 * std::cos(3.14159265358979 * 0.25)));
    CHECK(applyWarp(WarpMode::Remap4, 1.0f, 0.25, 0).phase == Approx(0.25));   // segment boundary is fixed
    CHECK(applyWarp(WarpMode::Remap1, 1.0f, 0.3, 0).phase == Approx(0.3));
}

TEST_CASE("quantize reduces the phase to 2 steps at full amount", "[warp]")
{
    CHECK(applyWarp(WarpMode::Quantize, 1.0f, 0.3, 0).phase == 0.0);
    CHECK(applyWarp(WarpMode::Quantize, 1.0f, 0.7, 0).phase == 0.5);
}

TEST_CASE("FM / AM / RM use the modulation input", "[warp]")
{
    CHECK(applyWarp(WarpMode::Fm, 1.0f, 0.1, 0.5f).phase == Approx(0.6));
    CHECK(applyWarp(WarpMode::Fm, 1.0f, 0.1, -0.5f).phase == Approx(0.6));   // wraps
    CHECK(applyWarp(WarpMode::FmNoise, 0.5f, 0.1, 0.4f).phase == Approx(0.3));
    CHECK(applyWarp(WarpMode::Am, 1.0f, 0.1, -1.0f).amp == Approx(0.0f));
    CHECK(applyWarp(WarpMode::Am, 1.0f, 0.1, 1.0f).amp == Approx(1.0f));
    CHECK(applyWarp(WarpMode::Rm, 1.0f, 0.1, -1.0f).amp == Approx(-1.0f));
    CHECK(warpInput(WarpMode::Fm) == WarpInput::PairedOsc);
    CHECK(warpInput(WarpMode::FmSub) == WarpInput::Sub);
}

TEST_CASE("dual warp chains phases and multiplies gains", "[warp]")
{
    const double p = 0.3;
    const auto first = applyWarp(WarpMode::Sync, 0.5f, p, 0);
    const auto second = applyWarp(WarpMode::Flip, 0.6f, first.phase, 0);
    const auto both = applyDualWarp(WarpMode::Sync, 0.5f, 0, WarpMode::Flip, 0.6f, 0, p);
    CHECK(both.phase == Approx(second.phase));
    CHECK(both.amp == first.amp * second.amp);
}

// --- Unison layout -----------------------------------------------------------------------------------

TEST_CASE("unison positions span -1..1 evenly", "[unison]")
{
    CHECK(unison::position(0, 1) == 0.0f);
    CHECK(unison::position(0, 7) == -1.0f);
    CHECK(unison::position(3, 7) == 0.0f);
    CHECK(unison::position(6, 7) == 1.0f);
    CHECK(unison::position(1, 4) == Approx(-1.0f / 3.0f));
}

TEST_CASE("detune curves follow the SPEC exponents", "[unison]")
{
    CHECK(unison::curve(unison::Mode::Linear, 0.5f) == 0.5f);
    CHECK(unison::curve(unison::Mode::Super, 0.5f) == Approx(std::pow(0.5f, 1.3f)));
    CHECK(unison::curve(unison::Mode::Exp, -0.5f) == Approx(-0.25f));
    CHECK(unison::curve(unison::Mode::Inv, 0.25f) == Approx(0.5f));
    CHECK(unison::curve(unison::Mode::Super, -1.0f) == -1.0f);
}

TEST_CASE("blend 0.75 weighs all voices equally; 0 keeps only the centre", "[unison]")
{
    for (int u = 0; u < 7; ++u) CHECK(unison::blendWeight(0.75f, u, 7) == 1.0f);
    CHECK(unison::blendWeight(0.0f, 3, 7) == 1.0f);
    CHECK(unison::blendWeight(0.0f, 0, 7) == 0.0f);
    CHECK(unison::blendWeight(1.0f, 3, 7) == 0.0f);
    CHECK(unison::blendWeight(1.0f, 0, 7) == 1.0f);
    // Even counts: the two middle voices are the centre. Two voices never fall silent.
    CHECK(unison::blendWeight(0.0f, 1, 4) == 1.0f);
    CHECK(unison::blendWeight(0.0f, 2, 4) == 1.0f);
    CHECK(unison::blendWeight(1.0f, 0, 2) == 1.0f);
    CHECK(unison::blendWeight(0.0f, 1, 2) == 1.0f);
}

TEST_CASE("stack modes add octaves and fifths", "[unison]")
{
    using S = unison::Stack;
    CHECK(unison::stackSemis(S::Off, 1, 4) == 0.0f);
    CHECK(unison::stackSemis(S::Octave1x, 1, 4) == 12.0f);
    CHECK(unison::stackSemis(S::Octave2x, 2, 4) == 24.0f);
    CHECK(unison::stackSemis(S::Fifth, 3, 4) == 7.0f);
    CHECK(unison::stackSemis(S::OctaveFifth, 2, 5) == 12.0f);
    CHECK(unison::stackSemis(S::Center12, 2, 5) == -12.0f);
    CHECK(unison::stackSemis(S::Center24, 1, 5) == 0.0f);
}
