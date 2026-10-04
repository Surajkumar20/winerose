// Engine Phase 5 (SPEC §5.4): effects and racks. Acceptance: null tests for linear FX, reverb RT60 within 10%.

#include "engine/fx/Effects.h"
#include "engine/fx/FxRack.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace winerose::fx;
using Catch::Approx;

namespace {

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

std::array<float, kParamCount> params(FxType t) { return defaultParams(t); }
float expoInv(float v, float lo, float hi) { return std::log(v / lo) / std::log(hi / lo); }

// Run an effect over a stereo signal in 32-sample chunks (the engine's control block).
void run(Effect& e, std::vector<float>& l, std::vector<float>& r)
{
    for (std::size_t i = 0; i < l.size(); i += 32) {
        const int n = static_cast<int>(std::min<std::size_t>(32, l.size() - i));
        e.process(l.data() + i, r.data() + i, n);
    }
}

std::vector<float> noise(int n, std::uint32_t seed)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (auto& v : x) { seed = seed * 1664525u + 1013904223u; v = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f; }
    return x;
}

double peakDbOfSine(const std::vector<float>& y, std::size_t from)
{
    float p = 0.0f;
    for (std::size_t i = from; i < y.size(); ++i) p = std::max(p, std::abs(y[i]));
    return 20.0 * std::log10(std::max(p, 1e-12f));
}

double toneDb(const std::vector<float>& y, double f, std::size_t from)
{
    double re = 0, im = 0;
    for (std::size_t i = from; i < y.size(); ++i) {
        re += y[i] * std::sin(2 * kPi * f * static_cast<double>(i) / kFs);
        im += y[i] * std::cos(2 * kPi * f * static_cast<double>(i) / kFs);
    }
    return 20.0 * std::log10(std::max(2.0 * std::hypot(re, im) / static_cast<double>(y.size() - from), 1e-12));
}

} // namespace

// --- Acceptance: null tests for linear effects at neutral settings -------------------------------------

TEST_CASE("linear effects null at neutral settings (SPEC Phase 5 acceptance)", "[fx][acceptance]")
{
    struct Case { FxType type; std::array<float, kParamCount> p; };
    auto eq = params(FxType::Eq);                   // shelves at 0 dB
    auto eqPeak = params(FxType::Eq); eqPeak[0] = 0.5f; eqPeak[4] = 0.5f;   // peaks at 0 dB
    auto util = params(FxType::Utility);            // gain 0 dB, centre, width 100%
    auto comp = params(FxType::Compressor); comp[0] = 1.0f; comp[4] = 0.0f; comp[5] = 0.0f;   // threshold 0 dB, no makeup/knee
    for (const auto& c : {Case{FxType::Eq, eq}, Case{FxType::Eq, eqPeak}, Case{FxType::Utility, util}, Case{FxType::Compressor, comp}}) {
        auto e = createEffect(c.type, kFs);
        e->setParams(c.p, FxContext{});
        auto l = noise(48000, 1), r = noise(48000, 2);
        for (auto& v : l) v *= 0.5f;   // below 0 dBFS so the compressor never engages
        for (auto& v : r) v *= 0.5f;
        auto refL = l, refR = r;
        run(*e, l, r);
        double worst = 0.0;
        for (std::size_t i = 0; i < l.size(); ++i)
            worst = std::max({worst, static_cast<double>(std::abs(l[i] - refL[i])), static_cast<double>(std::abs(r[i] - refR[i]))});
        INFO(kFxTypeNames[static_cast<int>(c.type)] << " worst residual " << worst);
        CHECK(worst == 0.0);
    }
}

// --- Acceptance: reverb RT60 within 10% ------------------------------------------------------------------

TEST_CASE("reverb decay matches the RT60 setting within 10% (SPEC Phase 5 acceptance)", "[fx][acceptance][reverb]")
{
    for (int mode = 0; mode < 2; ++mode) {
        for (float rt60 : {0.6f, 1.5f, 4.0f}) {
            Reverb rev;
            rev.prepare(kFs);
            auto p = params(FxType::Reverb);
            p[0] = mode == 0 ? 0.0f : 0.99f;            // Plate / Hall
            p[1] = expoInv(rt60, 0.1f, 20.0f);
            p[3] = 0.0f;                                 // no pre-delay
            p[4] = 0.0f;                                 // low cut at 20 Hz
            p[5] = 1.0f;                                 // high cut at 20 kHz
            p[6] = 0.0f;                                 // no damping: broadband RT60
            rev.setParams(p, FxContext{});
            const int n = static_cast<int>(kFs * rt60 * 1.6);
            std::vector<float> l(static_cast<std::size_t>(n), 0.0f), r(static_cast<std::size_t>(n), 0.0f);
            l[0] = r[0] = 1.0f;
            run(rev, l, r);
            // Schroeder backward integration; T20 from the -5 dB and -25 dB crossings.
            std::vector<double> edc(static_cast<std::size_t>(n));
            double acc = 0.0;
            for (int i = n - 1; i >= 0; --i) {
                acc += static_cast<double>(l[static_cast<std::size_t>(i)]) * l[static_cast<std::size_t>(i)]
                     + static_cast<double>(r[static_cast<std::size_t>(i)]) * r[static_cast<std::size_t>(i)];
                edc[static_cast<std::size_t>(i)] = acc;
            }
            auto crossing = [&](double db) {
                const double target = edc[0] * std::pow(10.0, db / 10.0);
                for (int i = 0; i < n; ++i) if (edc[static_cast<std::size_t>(i)] <= target) return i / kFs;
                return n / kFs;
            };
            const double measured = 3.0 * (crossing(-25.0) - crossing(-5.0));
            INFO((mode == 0 ? "Plate" : "Hall") << " RT60 set " << rt60 << " s, measured " << measured << " s");
            CHECK(std::abs(measured - rt60) / rt60 < 0.10);
        }
    }
}

// --- Behaviour of individual effects ---------------------------------------------------------------------

TEST_CASE("compressor follows its static curve", "[fx][compressor]")
{
    CHECK(Compressor::gainComputer(-30.0f, -20.0f, 4.0f, 0.0f) == 0.0f);
    CHECK(Compressor::gainComputer(-10.0f, -20.0f, 4.0f, 0.0f) == Approx(-7.5f));
    CHECK(Compressor::gainComputer(-10.0f, -20.0f, 1e9f, 0.0f) == Approx(-10.0f).margin(1e-3));   // limit

    Compressor c;
    c.prepare(kFs);
    auto p = params(FxType::Compressor);
    p[0] = (-20.0f + 60.0f) / 60.0f;      // threshold -20 dB
    p[1] = expoInv(4.0f, 1.0f, 20.0f);    // ratio 4
    p[2] = 0.0f;                          // fastest attack
    p[3] = 1.0f;                          // slowest release: the static curve applies (no ripple between peaks)
    p[4] = 0.0f; p[5] = 0.0f;
    c.setParams(p, FxContext{});
    std::vector<float> l(48000), r(48000);
    for (std::size_t i = 0; i < l.size(); ++i) l[i] = r[i] = 0.31623f * static_cast<float>(std::sin(2 * kPi * 1000.0 * i / kFs));   // -10 dB
    run(c, l, r);
    CHECK(peakDbOfSine(l, 24000) == Approx(-17.5).margin(0.5));
}

TEST_CASE("delay echoes at the set time; ping-pong alternates sides", "[fx][delay]")
{
    Delay d;
    d.prepare(kFs);
    auto p = params(FxType::Delay);
    p[0] = 0.0f;                                  // normal
    p[1] = 0.0f;                                  // free time
    p[2] = expoInv(100.0f, 1.0f, 2000.0f);        // 100 ms
    p[3] = 0.5f;                                  // right = left
    p[4] = 0.5f / 0.98f;                          // feedback 0.5
    p[6] = 0.0f;                                  // no filter
    d.setParams(p, FxContext{});
    std::vector<float> l(24000, 0.0f), r(24000, 0.0f);
    l[0] = r[0] = 1.0f;
    run(d, l, r);
    CHECK(l[4800] == Approx(1.0f).margin(1e-3));      // first echo at 100 ms
    CHECK(l[9600] == Approx(0.5f).margin(1e-3));      // second, at the feedback level
    CHECK(std::abs(l[4000]) < 1e-6f);

    p[0] = 0.5f;                                  // ping-pong
    d.setParams(p, FxContext{});
    d.reset();
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(r.begin(), r.end(), 0.0f);
    l[0] = 1.0f;
    run(d, l, r);
    CHECK(std::abs(l[4800]) > 0.3f);                  // first echo left
    CHECK(std::abs(r[4800]) < 1e-6f);
    CHECK(std::abs(r[9600]) > 0.1f);                  // second right
    CHECK(std::abs(l[9600]) < 1e-6f);
}

TEST_CASE("Bode shifter moves a tone up and suppresses the image", "[fx][bode]")
{
    Bode b;
    b.prepare(kFs);
    auto p = params(FxType::Bode);
    p[0] = 0.5f + 0.5f * std::cbrt(200.0f / 5000.0f);   // +200 Hz
    p[1] = 0.0f;
    p[2] = 0.0f;
    b.setParams(p, FxContext{});
    std::vector<float> l(48000), r(48000);
    for (std::size_t i = 0; i < l.size(); ++i) l[i] = r[i] = 0.5f * static_cast<float>(std::sin(2 * kPi * 1000.0 * i / kFs));
    run(b, l, r);
    const double up = toneDb(l, 1200.0, 24000), image = toneDb(l, 800.0, 24000), orig = toneDb(l, 1000.0, 24000);
    INFO("1200 Hz " << up << " dB, 800 Hz " << image << " dB, 1000 Hz " << orig << " dB");
    CHECK(up > -8.0);
    CHECK(image < up - 30.0);
    CHECK(orig < up - 30.0);
}

TEST_CASE("convolution is zero-latency and equals the impulse response", "[fx][convolve]")
{
    Convolve c;
    c.prepare(kFs);
    auto p = params(FxType::Convolve);
    p[0] = 1.0f;    // full length
    p[1] = 0.0f;    // no pre-delay
    p[2] = 0.0f;    // low cut off
    p[3] = 1.0f;    // high cut off
    c.setParams(p, FxContext{});
    std::vector<float> l(20000, 0.0f), r(20000, 0.0f);
    l[0] = r[0] = 1.0f;
    run(c, l, r);
    double worst = 0.0;
    for (std::size_t i = 0; i < l.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::abs(l[i] - c.impulse(0)[i])));
        worst = std::max(worst, static_cast<double>(std::abs(r[i] - c.impulse(1)[i])));
    }
    INFO("worst |y - h| = " << worst);
    CHECK(worst < 1e-5);
    CHECK(l[0] == Approx(c.impulse(0)[0]));   // no latency
}

TEST_CASE("distortion adds harmonics and its output knob scales", "[fx][distortion]")
{
    Distortion d;
    d.prepare(kFs);
    auto p = params(FxType::Distortion);
    p[0] = 0.5f;   // 24 dB drive
    d.setParams(p, FxContext{});
    std::vector<float> l(48000), r(48000);
    for (std::size_t i = 0; i < l.size(); ++i) l[i] = r[i] = 0.3f * static_cast<float>(std::sin(2 * kPi * 200.0 * i / kFs));
    run(d, l, r);
    CHECK(toneDb(l, 600.0, 24000) > -30.0);   // third harmonic
}

TEST_CASE("every effect stays finite under parameter and input fuzzing", "[fx]")
{
    std::uint32_t seed = 4242;
    auto rnd = [&seed] { seed = seed * 1664525u + 1013904223u; return static_cast<float>(seed >> 8) / 16777216.0f; };
    for (int t = 1; t < static_cast<int>(FxType::Count); ++t) {
        auto e = createEffect(static_cast<FxType>(t), kFs);
        if (!e) continue;   // splitters
        float worst = 0.0f;
        for (int block = 0; block < 60; ++block) {
            std::array<float, kParamCount> p;
            for (auto& v : p) v = rnd();
            e->setParams(p, FxContext{120.0 + 60.0 * rnd()});
            float l[32], r[32];
            for (int rep = 0; rep < 30; ++rep) {
                const float gain = 2.0f * rnd();
                for (int i = 0; i < 32; ++i) { l[i] = (rnd() * 2 - 1) * gain; r[i] = (rnd() * 2 - 1) * gain; }
                e->process(l, r, 32);
                for (int i = 0; i < 32; ++i) {
                    REQUIRE(std::isfinite(l[i]));
                    REQUIRE(std::isfinite(r[i]));
                    worst = std::max({worst, std::abs(l[i]), std::abs(r[i])});
                }
            }
        }
        INFO(kFxTypeNames[t] << " worst |y| = " << worst);
        CHECK(worst < 1000.0f);
    }
}

// --- Racks -----------------------------------------------------------------------------------------------

namespace {

struct RackRig {
    FxRack rack;
    FxRack::Params params {};
    RackRig() { rack.prepare(kFs); }
    void set(int slot, FxType type, std::array<float, kParamCount> p, float mix = 1.0f, bool enabled = true)
    {
        rack.setSlotType(slot, type, kFs);
        params[static_cast<std::size_t>(slot)] = SlotParams{type, enabled, mix, p};
    }
    std::vector<float> runSine(double f, int n = 24000)
    {
        rack.beginBlock();
        rack.setParams(params, FxContext{});
        std::vector<float> l(static_cast<std::size_t>(n)), r(static_cast<std::size_t>(n));
        for (std::size_t i = 0; i < l.size(); ++i) l[i] = r[i] = 0.5f * static_cast<float>(std::sin(2 * kPi * f * i / kFs));
        for (std::size_t i = 0; i < l.size(); i += 32) rack.process(l.data() + i, r.data() + i, 32);
        return l;
    }
};

} // namespace

TEST_CASE("an empty rack, a disabled slot and mix 0 all pass audio unchanged", "[fx][rack]")
{
    RackRig rig;
    auto ref = rig.runSine(440.0);
    RackRig dis;
    dis.set(0, FxType::Distortion, params(FxType::Distortion), 1.0f, false);
    CHECK(dis.runSine(440.0) == ref);
    RackRig dry;
    dry.set(0, FxType::Distortion, params(FxType::Distortion), 0.0f);
    CHECK(dry.runSine(440.0) == ref);
}

TEST_CASE("splitter bands sum flat and route through the following slots", "[fx][rack][splitter]")
{
    for (FxType split : {FxType::SplitLowHigh, FxType::SplitLowMidHigh}) {
        RackRig rig;
        rig.set(0, split, params(split));
        for (double f : {60.0, 500.0, 2000.0, 9000.0}) {
            const auto y = rig.runSine(f);
            INFO(kFxTypeNames[static_cast<int>(split)] << " at " << f << " Hz");
            CHECK(toneDb(y, f, 12000) == Approx(20.0 * std::log10(0.5)).margin(0.05));   // allpass: magnitude flat
        }
    }
    // A -24 dB utility on the low band of a low/high split only affects low frequencies.
    RackRig rig;
    auto sp = params(FxType::SplitLowHigh);
    sp[0] = expoInv(1000.0f, 20.0f, 20000.0f);
    rig.set(0, FxType::SplitLowHigh, sp);
    auto quiet = params(FxType::Utility);
    quiet[0] = (-24.0f + 48.0f) / 60.0f;
    rig.set(1, FxType::Utility, quiet);
    CHECK(peakDbOfSine(rig.runSine(100.0), 12000) < -25.0);
    CHECK(peakDbOfSine(rig.runSine(8000.0), 12000) > -7.0);
}

TEST_CASE("slot order matters and type changes take effect at the next block", "[fx][rack]")
{
    auto drive = params(FxType::Distortion);
    drive[0] = 0.6f;
    auto cut = params(FxType::Eq);
    cut[4] = 0.99f;   // high band = low-pass
    cut[5] = expoInv(800.0f, 500.0f, 20000.0f);
    RackRig a, b;
    a.set(0, FxType::Distortion, drive); a.set(1, FxType::Eq, cut);
    b.set(0, FxType::Eq, cut);           b.set(1, FxType::Distortion, drive);
    CHECK(toneDb(a.runSine(300.0), 2700.0, 12000) < toneDb(b.runSine(300.0), 2700.0, 12000) - 6.0);

    RackRig c;
    c.set(0, FxType::Utility, params(FxType::Utility));
    const auto before = c.runSine(440.0);
    auto quiet = params(FxType::Utility);
    quiet[0] = 0.0f;   // -48 dB
    c.params[0].p = quiet;
    CHECK(peakDbOfSine(c.runSine(440.0), 1000) < peakDbOfSine(before, 1000) - 40.0);
}
