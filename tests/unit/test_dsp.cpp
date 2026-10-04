#include "engine/dsp/Envelope.h"
#include "engine/dsp/Fft.h"
#include "engine/dsp/Svf.h"
#include "engine/dsp/WavetableBank.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <vector>

using namespace winerose::dsp;
using Catch::Approx;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Amplitude (peak, linear) of every bin of a real signal of length N, normalized so a full-scale sine reads 1.
std::vector<double> amplitudeSpectrum(const std::vector<float>& x)
{
    const int n = static_cast<int>(x.size());
    RealFft fft(n);
    std::vector<float> s(x.size());
    fft.forward(x.data(), s.data());
    std::vector<double> a(static_cast<std::size_t>(n / 2), 0.0);
    a[0] = std::abs(s[0]) / n;
    for (int k = 1; k < n / 2; ++k) {
        const double re = s[static_cast<std::size_t>(2 * k)], im = s[static_cast<std::size_t>(2 * k + 1)];
        a[static_cast<std::size_t>(k)] = 2.0 * std::sqrt(re * re + im * im) / n;
    }
    return a;
}

double db(double a) { return a > 0.0 ? 20.0 * std::log10(a) : -400.0; }

} // namespace

// --- WavetableBank ---------------------------------------------------------------------------------

TEST_CASE("each mip level contains only harmonics up to its limit", "[dsp][wavetable]")
{
    const auto bank = makeBasicShapesTable();
    REQUIRE(bank->frameCount() == 4);
    const int level = GENERATE(0, 3, 7, 10);
    std::vector<float> frame(bank->frameData(level, 0), bank->frameData(level, 0) + WavetableBank::kFrameSize);
    const auto a = amplitudeSpectrum(frame);
    const int limit = WavetableBank::maxHarmonic(level);
    double worstAbove = 0.0;
    for (int k = limit + 1; k < static_cast<int>(a.size()); ++k) worstAbove = std::max(worstAbove, a[static_cast<std::size_t>(k)]);
    INFO("level " << level << " limit " << limit);
    CHECK(db(worstAbove) < -120.0);
    CHECK(a[1] > 0.3);           // fundamental present
    CHECK(db(a[0]) < -120.0);    // DC removed
}

TEST_CASE("level selection never lets a partial reach Nyquist", "[dsp][wavetable]")
{
    for (double inc = 1e-5; inc < 0.5; inc *= 1.01) {
        const auto lc = WavetableBank::selectLevel(inc);
        REQUIRE(lc.level >= 0);
        REQUIRE(lc.level < WavetableBank::kLevels);
        INFO("inc " << inc << " level " << lc.level);
        REQUIRE(WavetableBank::maxHarmonic(lc.level) * inc < 0.5);
    }
}

TEST_CASE("level selection is continuous across octave boundaries", "[dsp][wavetable]")
{
    // Just below and just above a boundary must blend to (almost) the same mix of tables.
    for (int k = 0; k < 9; ++k) {
        const double boundary = std::exp2(k) / WavetableBank::kFrameSize;
        const auto below = WavetableBank::selectLevel(boundary * 0.99999);
        const auto above = WavetableBank::selectLevel(boundary * 1.00001);
        // below: level k with blend≈1 toward k+1; above: level k+1 with blend≈0.
        CHECK(below.level + 1 == above.level);
        CHECK(below.blend == Approx(1.0f).margin(1e-3));
        CHECK(above.blend == Approx(0.0f).margin(1e-3));
    }
}

TEST_CASE("C8 saw aliasing stays below -90 dBFS at 48 kHz (SPEC Phase 1 acceptance)", "[dsp][wavetable][acceptance]")
{
    // f0 = 4186 Hz exactly and N = fs = 48000: every harmonic AND every alias lands exactly on a bin, so a
    // rectangular window over one second of steady state gives an exact, leakage-free measurement.
    constexpr int fs = 48000;
    constexpr double f0 = 4186.0;
    const auto bank = makeBasicShapesTable();
    const double inc = f0 / fs;
    const auto lc = WavetableBank::selectLevel(inc);

    std::vector<float> x(fs);
    double phase = 0.0;
    for (int i = 0; i < fs; ++i) {
        x[static_cast<std::size_t>(i)] = bank->read(phase, 0.0f, lc);   // frame 0 = saw
        phase += inc;
        if (phase >= 1.0) phase -= 1.0;
    }
    const auto a = amplitudeSpectrum(x);
    double worstAlias = 0.0, fundamental = a[4186];
    for (int k = 1; k < fs / 2; ++k) {
        if (k % 4186 == 0) continue;   // a harmonic
        worstAlias = std::max(worstAlias, a[static_cast<std::size_t>(k)]);
    }
    INFO("fundamental " << db(fundamental) << " dBFS, worst alias " << db(worstAlias) << " dBFS");
    CHECK(db(fundamental) > -6.0);
    CHECK(db(worstAlias) < -90.0);
}

TEST_CASE("frame morph interpolates between adjacent frames", "[dsp][wavetable]")
{
    const auto bank = makeBasicShapesTable();
    const auto lc = WavetableBank::selectLevel(100.0 / 48000.0);
    for (double p : {0.1, 0.37, 0.8}) {
        const float a = bank->read(p, 2.0f, lc);
        const float b = bank->read(p, 3.0f, lc);
        CHECK(bank->read(p, 2.5f, lc) == Approx(0.5f * (a + b)).margin(1e-6));
    }
    CHECK(bank->read(0.25, 3.0f, lc) == Approx(1.0f).margin(1e-3));   // frame 3 = sine, peak at phase 0.25
}

TEST_CASE("frames of other sizes are resampled to 2048", "[dsp][wavetable]")
{
    std::vector<float> frame(256);
    for (int i = 0; i < 256; ++i) frame[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(2.0 * kPi * i / 256));
    const auto bank = WavetableBank::build(frame, 256, "sine256");
    REQUIRE(bank->frameCount() == 1);
    const auto lc = WavetableBank::selectLevel(0.001);
    CHECK(bank->read(0.25, 0.0f, lc) == Approx(1.0f).margin(1e-3));
    CHECK(WavetableBank::build({}, 2048, "empty")->frameCount() == 1);
}

// --- Svf ---------------------------------------------------------------------------------------------

TEST_CASE("SVF low-pass: unity DC gain and -3 dB at cutoff for the Butterworth setting", "[dsp][svf]")
{
    constexpr double fs = 48000.0, fc = 1000.0;
    const auto c = Svf::compute(fc, 1.0 - std::sqrt(2.0) / 2.0, fs);   // k = √2
    auto gainAt = [&](double f) {
        Svf svf;
        double peak = 0.0;
        for (int i = 0; i < 48000; ++i) {
            const float y = svf.processLow(static_cast<float>(std::sin(2.0 * kPi * f * i / fs)), c);
            if (i > 24000) peak = std::max(peak, static_cast<double>(std::abs(y)));
        }
        return db(peak);
    };
    CHECK(gainAt(20.0) == Approx(0.0).margin(0.05));
    CHECK(gainAt(fc) == Approx(-3.01).margin(0.1));
    CHECK(gainAt(8000.0) < -30.0);   // 12 dB/oct: three octaves above ≈ -36 dB
}

TEST_CASE("SVF stays finite at maximum resonance and extreme cutoffs", "[dsp][svf]")
{
    for (double fc : {8.18, 440.0, 22050.0, 1e6}) {
        const auto c = Svf::compute(fc, 1.0, 48000.0);
        Svf svf;
        std::uint32_t seed = 1;
        for (int i = 0; i < 100000; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const float x = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
            const float y = svf.processLow(x, c);
            REQUIRE(std::isfinite(y));
            REQUIRE(std::abs(y) < 1000.0f);
        }
    }
}

// --- Envelope ----------------------------------------------------------------------------------------

TEST_CASE("envelope timings are sample accurate", "[dsp][envelope]")
{
    constexpr double fs = 48000.0;
    Envelope env;
    env.setSampleRate(fs);
    env.setSettings({0.010f, 0.005f, 0.100f, 0.5f, 0.050f});
    env.noteOn();

    int samples = 0;
    while (env.stage() == Envelope::Stage::Attack) { env.next(); ++samples; }
    CHECK(std::abs(samples - 480) <= 1);       // 10 ms
    CHECK(env.level() == 1.0f);
    samples = 0;
    while (env.stage() == Envelope::Stage::Hold) { env.next(); ++samples; }
    CHECK(std::abs(samples - 240) <= 1);       // 5 ms
    samples = 0;
    while (env.stage() == Envelope::Stage::Decay) { env.next(); ++samples; }
    CHECK(std::abs(samples - 4800) <= 1);      // 100 ms
    CHECK(env.next() == 0.5f);                 // sustain

    env.noteOff();
    samples = 0;
    while (env.isActive()) { env.next(); ++samples; }
    CHECK(std::abs(samples - 2400) <= 1);      // 50 ms
    CHECK(env.level() == 0.0f);
}

TEST_CASE("envelope retrigger and release start from the current level", "[dsp][envelope]")
{
    Envelope env;
    env.setSampleRate(48000.0);
    env.setSettings({0.100f, 0.0f, 0.1f, 1.0f, 0.1f});
    env.noteOn();
    for (int i = 0; i < 2400; ++i) env.next();   // halfway up a linear attack
    const float mid = env.level();
    CHECK(mid == Approx(0.5f).margin(0.01));

    env.noteOff();
    CHECK(std::abs(env.next() - mid) < 0.01f);   // no jump into the release

    env.noteOn();
    const float after = env.next();
    CHECK(after >= env.level() - 1e-6f);
    CHECK(after < mid);                          // attack restarted from where release left it
}

TEST_CASE("zero-length stages jump straight through", "[dsp][envelope]")
{
    Envelope env;
    env.setSampleRate(48000.0);
    env.setSettings({0.0f, 0.0f, 0.0f, 0.25f, 0.0f});
    env.noteOn();
    CHECK(env.next() == 0.25f);
    env.noteOff();
    CHECK_FALSE(env.isActive());
}

TEST_CASE("read4 matches four scalar reads bit for bit", "[dsp][wavetable][simd]")
{
    const auto bank = makeBasicShapesTable();
    std::uint32_t seed = 99;
    auto rnd = [&seed] { seed = seed * 1664525u + 1013904223u; return static_cast<double>(seed >> 8) / 16777216.0; };
    for (int trial = 0; trial < 20000; ++trial) {
        const auto fp = bank->resolveFrame(static_cast<float>(rnd() * 3.0));
        double ph[4];
        WavetableBank::LevelChoice lc[4];
        for (int k = 0; k < 4; ++k) {
            ph[k] = trial % 97 == 0 ? 1.0 : rnd();   // include the phase == 1.0 edge
            lc[k] = WavetableBank::selectLevel(std::pow(10.0, -5.0 + 4.6 * rnd()));
        }
        float x4[4];
        bank->read4(ph, fp, lc, x4);
        for (int k = 0; k < 4; ++k) REQUIRE(x4[k] == bank->read(ph[k], fp, lc[k]));
    }
}
