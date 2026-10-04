// Engine Phase 4 (SPEC §5.4): filter types against analytic responses, self-oscillation, fuzzing, drive,
// stereo, and routing (per-source routes, serial/parallel, outputs, keytrack).

#include "EngineRig.h"

#include "engine/dsp/filters/FilterUnit.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <functional>
#include <string>
#include <vector>

using namespace winerose;
using winerose::test::midi;
using winerose::test::Rig;
using dsp::FilterSettings;
using dsp::FilterType;
using dsp::FilterUnit;
using Catch::Approx;
using cplx = std::complex<double>;

namespace {

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

FilterSettings settingsFor(FilterType type, float cutoff, float res, float var = 0.5f)
{
    FilterSettings s;
    s.type = type;
    s.cutoffHz = cutoff;
    s.resonance = res;
    s.var = var;
    return s;
}

// Steady-state gain (dB) of the filter for a sine at f: run 0.5 s, then demodulate 0.25 s.
double measuredGainDb(const FilterSettings& s, double f)
{
    FilterUnit unit;
    unit.prepare(kFs);
    unit.set(s);
    const int settle = 24000, measure = 12000;
    double re = 0.0, im = 0.0;
    for (int n = 0; n < settle + measure; ++n) {
        const double ph = 2.0 * kPi * f * n / kFs;
        float l = static_cast<float>(std::sin(ph)), r = l;
        unit.process(l, r);
        if (n >= settle) {
            re += l * std::sin(ph);
            im += l * std::cos(ph);
        }
    }
    const double amp = 2.0 * std::sqrt(re * re + im * im) / measure;
    return 20.0 * std::log10(std::max(amp, 1e-12));
}

// Bilinear (prewarped) analog frequency for TPT filters: s = j·tan(πf/fs)/tan(πfc/fs).
cplx sOf(double f, double fc) { return {0.0, std::tan(kPi * f / kFs) / std::tan(kPi * fc / kFs)}; }

double db(cplx h) { return 20.0 * std::log10(std::max(std::abs(h), 1e-12)); }

struct Case {
    const char* name;
    FilterSettings settings;
    std::function<cplx(double f)> analytic;
};

std::vector<Case> linearCases()
{
    const double fc = 1000.0, res = 0.3, k = 2.0 - 2.0 * res;
    auto den = [=](cplx s, double kk) { return s * s + kk * s + 1.0; };
    std::vector<Case> cases;
    auto add = [&](const char* name, FilterType t, std::function<cplx(cplx)> h) {
        cases.push_back({name, settingsFor(t, static_cast<float>(fc), static_cast<float>(res)),
                         [=](double f) { return h(sOf(f, fc)); }});
    };
    add("LP 6",  FilterType::Lp6,  [](cplx s) { return 1.0 / (1.0 + s); });
    add("HP 6",  FilterType::Hp6,  [](cplx s) { return s / (1.0 + s); });
    add("LP 12", FilterType::Lp12, [=](cplx s) { return 1.0 / den(s, k); });
    add("HP 12", FilterType::Hp12, [=](cplx s) { return s * s / den(s, k); });
    add("BP 12", FilterType::Bp12, [=](cplx s) { return k * s / den(s, k); });
    add("Notch 12", FilterType::Notch12, [=](cplx s) { return (s * s + 1.0) / den(s, k); });
    add("Peak 12", FilterType::Peak12, [=](cplx s) { return (1.0 - s * s) / den(s, k); });
    add("Allpass 12", FilterType::Allpass12, [=](cplx s) { return (s * s - k * s + 1.0) / den(s, k); });
    add("LP 18", FilterType::Lp18, [=](cplx s) { return 1.0 / den(s, k) / (1.0 + s); });
    add("LP 24", FilterType::Lp24, [=](cplx s) { return 1.0 / den(s, k) / den(s, 2.0); });
    add("HP 24", FilterType::Hp24, [=](cplx s) { return s * s / den(s, k) * s * s / den(s, 2.0); });
    // Ladder: U = comp·X / (1 + k·H⁴), tap n = Hⁿ·U with H = 1/(1+s).
    const double kl = 3.98 * res, comp = 1.0 + 0.5 * kl;
    auto ladder = [=](cplx s, int n) {
        const cplx H = 1.0 / (1.0 + s);
        const cplx U = comp / (1.0 + kl * std::pow(H, 4));
        return std::pow(H, n) * U;
    };
    add("Ladder 6",  FilterType::Ladder6,  [=](cplx s) { return ladder(s, 1); });
    add("Ladder 12", FilterType::Ladder12, [=](cplx s) { return ladder(s, 2); });
    add("Ladder 18", FilterType::Ladder18, [=](cplx s) { return ladder(s, 3); });
    add("Ladder 24", FilterType::Ladder24, [=](cplx s) { return ladder(s, 4); });
    add("Ladder HP 24", FilterType::LadderHp24, [=](cplx s) {
        const cplx H = 1.0 / (1.0 + s);
        return std::pow(1.0 - H, 4) * comp / (1.0 + kl * std::pow(H, 4));
    });
    add("Disperser", FilterType::Disperser, [](cplx) { return cplx(1.0, 0.0); });   // allpass: unit magnitude
    return cases;
}

} // namespace

// --- Acceptance: linear responses -----------------------------------------------------------------------

TEST_CASE("linear filter responses are within 0.5 dB of analytic (SPEC Phase 4 acceptance)", "[filter][acceptance]")
{
    const double freqs[] = {50.0, 200.0, 600.0, 1000.0, 1700.0, 4000.0, 11000.0};
    for (const auto& c : linearCases()) {
        for (double f : freqs) {
            const double expected = db(c.analytic(f));
            if (expected < -60.0) continue;   // below the measurement floor
            const double got = measuredGainDb(c.settings, f);
            INFO(c.name << " at " << f << " Hz: expected " << expected << " dB, got " << got << " dB");
            CHECK(std::abs(got - expected) < 0.5);
        }
    }
}

TEST_CASE("feedback comb matches 1/(1 - g z^-D) at integer delays", "[filter][acceptance]")
{
    const float fc = 480.0f;   // D = 100 samples exactly
    for (FilterType t : {FilterType::CombPlus, FilterType::CombMinus}) {
        const auto s = settingsFor(t, fc, 0.6f);
        const double g = (t == FilterType::CombPlus ? 1.0 : -1.0) * 0.6 * 0.98;
        for (double f : {240.0, 480.0, 700.0, 960.0, 1300.0}) {
            const cplx z = std::polar(1.0, -2.0 * kPi * f * 100.0 / kFs);   // z^-D
            const double expected = db((1.0 - std::abs(g)) / (1.0 - g * z));
            INFO(dsp::filterInfo(t).name << " at " << f << " Hz");
            CHECK(std::abs(measuredGainDb(s, f) - expected) < 0.5);
        }
    }
}

// --- Acceptance: stable self-oscillation ----------------------------------------------------------------

TEST_CASE("nonlinear filters self-oscillate stably at full resonance (SPEC Phase 4 acceptance)", "[filter][acceptance]")
{
    for (FilterType t : {FilterType::DrivenLadder24, FilterType::DrivenLadder12, FilterType::Acid24,
                         FilterType::Acid18, FilterType::SkLp12, FilterType::SkBp12}) {
        FilterUnit unit;
        unit.prepare(kFs);
        unit.set(settingsFor(t, 1000.0f, 1.0f));
        float peak = 0.0f, lastSecondPeak = 0.0f;
        int crossings = 0;
        float prev = 0.0f;
        const int total = static_cast<int>(kFs * 3.0);
        for (int n = 0; n < total; ++n) {
            float l = n < 32 ? 0.5f : 0.0f, r = l;   // short kick, then silence
            unit.process(l, r);
            REQUIRE(std::isfinite(l));
            peak = std::max(peak, std::abs(l));
            if (n >= total - static_cast<int>(kFs)) {
                lastSecondPeak = std::max(lastSecondPeak, std::abs(l));
                crossings += (prev <= 0.0f && l > 0.0f) ? 1 : 0;
            }
            prev = l;
        }
        INFO(dsp::filterInfo(t).name << ": peak " << peak << ", last-second peak " << lastSecondPeak << ", f ≈ " << crossings);
        CHECK(peak < 4.0f);                  // bounded
        CHECK(lastSecondPeak > 0.05f);       // still oscillating 2 s after the kick
        CHECK(crossings > 600);              // near the 1 kHz cutoff
        CHECK(crossings < 1400);
    }
}

// --- Acceptance: fuzzing ---------------------------------------------------------------------------------

TEST_CASE("every filter type stays finite under parameter and input fuzzing (SPEC Phase 4 acceptance)", "[filter][acceptance]")
{
    std::uint32_t seed = 12345;
    auto rnd = [&seed] { seed = seed * 1664525u + 1013904223u; return static_cast<float>(seed >> 8) / 16777216.0f; };
    for (int t = 0; t < static_cast<int>(FilterType::Count); ++t) {
        FilterUnit unit;
        unit.prepare(kFs);
        float worst = 0.0f;
        for (int block = 0; block < 40; ++block) {
            FilterSettings s;
            s.type = static_cast<FilterType>(t);
            s.cutoffHz = 8.18f * std::pow(22050.0f / 8.18f, rnd());
            s.resonance = rnd();
            s.drive = rnd() < 0.5f ? 0.0f : rnd();
            s.clean = rnd() < 0.5f;
            s.var = rnd();
            s.x = rnd();
            s.y = rnd();
            s.stereo = rnd();
            unit.set(s);
            const float gain = 4.0f * rnd();
            for (int n = 0; n < 1200; ++n) {
                float l = (rnd() * 2.0f - 1.0f) * gain, r = (rnd() * 2.0f - 1.0f) * gain;
                if (n % 97 == 0) unit.setCutoff(8.18f * std::pow(22050.0f / 8.18f, rnd()));   // audio-rate cutoff jumps
                unit.process(l, r);
                REQUIRE(std::isfinite(l));
                REQUIRE(std::isfinite(r));
                worst = std::max({worst, std::abs(l), std::abs(r)});
            }
        }
        // Inputs reach 4.0 and Clean drive adds up to +12 dB with no limiter; allpass cascades can also
        // stack peaks far above their input. The bound catches runaway growth, not loud-but-stable output.
        INFO(dsp::kFilterInfo[t].name << " worst |y| = " << worst);
        CHECK(worst < 2000.0f);
    }
}

// --- Drive, stereo, mix ---------------------------------------------------------------------------------

TEST_CASE("drive adds harmonics; clean mode adds fewer", "[filter]")
{
    auto thirdHarmonicDb = [](float drive, bool clean) {
        FilterUnit unit;
        unit.prepare(kFs);
        auto s = settingsFor(FilterType::Lp12, 20000.0f, 0.0f);
        s.drive = drive;
        s.clean = clean;
        unit.set(s);
        double re = 0.0, im = 0.0;
        for (int n = 0; n < 48000; ++n) {
            float l = 0.5f * static_cast<float>(std::sin(2.0 * kPi * 200.0 * n / kFs)), r = l;
            unit.process(l, r);
            if (n >= 24000) {
                re += l * std::sin(2.0 * kPi * 600.0 * n / kFs);
                im += l * std::cos(2.0 * kPi * 600.0 * n / kFs);
            }
        }
        return 20.0 * std::log10(std::max(2.0 * std::hypot(re, im) / 24000.0, 1e-12));
    };
    const double none = thirdHarmonicDb(0.0f, false), hard = thirdHarmonicDb(0.6f, false), soft = thirdHarmonicDb(0.6f, true);
    INFO("H3: none " << none << " dB, drive " << hard << " dB, clean " << soft << " dB");
    CHECK(none < -100.0);
    CHECK(hard > -30.0);
    CHECK(soft < hard - 6.0);
}

TEST_CASE("stereo spreads the left and right cutoffs; mix 0 is dry", "[filter]")
{
    FilterUnit unit;
    unit.prepare(kFs);
    auto s = settingsFor(FilterType::Lp12, 1000.0f, 0.5f);
    s.stereo = 1.0f;
    unit.set(s);
    double el = 0.0, er = 0.0;
    for (int n = 0; n < 24000; ++n) {
        float l = static_cast<float>(std::sin(2.0 * kPi * 1400.0 * n / kFs)), r = l;
        unit.process(l, r);
        if (n > 12000) { el += l * l; er += r * r; }
    }
    CHECK(er > el * 1.5);   // right channel has the higher cutoff

    s.stereo = 0.0f;
    s.mix = 0.0f;
    unit.set(s);
    float l = 0.3f, r = -0.2f;
    unit.process(l, r);
    CHECK(l == Approx(0.3f));
    CHECK(r == Approx(-0.2f));
}

// --- Routing in the engine ------------------------------------------------------------------------------

namespace {

double rmsOf(const std::vector<float>& x)
{
    double s = 0.0;
    for (std::size_t i = x.size() / 2; i < x.size(); ++i) s += static_cast<double>(x[i]) * x[i];
    return std::sqrt(s / static_cast<double>(x.size() - x.size() / 2));
}

// A dark, enabled Filter 1: a saw through it loses most of its energy.
void darkFilter(Rig& rig, int filter = 0)
{
    auto& f = rig.reg("Filter" + std::to_string(filter));
    f.set("enabled", true);
    f.set("cutoff", 100.0f);
    f.set("resonance", 0.0f);
}

} // namespace

TEST_CASE("by default only oscillator A goes through the filters", "[filter][routing]")
{
    auto level = [](const char* osc, bool filterOn) {
        Rig rig;
        rig.reg("Oscillator0").set("enabled", false);
        rig.reg(osc).set("enabled", true);
        rig.reg(osc).set("random", 0.0f);
        if (filterOn) darkFilter(rig);
        return rmsOf(rig.renderNote(60, 0.3));
    };
    CHECK(level("Oscillator0", true) < 0.5 * level("Oscillator0", false));   // A is filtered
    CHECK(level("Oscillator1", true) == Approx(level("Oscillator1", false)).epsilon(1e-6));   // B goes to Main
}

TEST_CASE("a disabled filter passes its routed signal through", "[filter][routing]")
{
    Rig a, b;
    a.reg("Oscillator0").set("random", 0.0f);
    b.reg("Oscillator0").set("random", 0.0f);
    b.reg("Filter0").set("cutoff", 100.0f);   // set but disabled
    CHECK(rmsOf(a.renderNote(60, 0.2)) == Approx(rmsOf(b.renderNote(60, 0.2))).epsilon(1e-6));
}

TEST_CASE("route None silences a source; Direct and Main both reach the output", "[filter][routing]")
{
    auto level = [](int route) {
        Rig rig;
        rig.reg("Oscillator0").set("random", 0.0f);
        rig.reg("Oscillator0").set("route", route);
        return rmsOf(rig.renderNote(60, 0.2));
    };
    const double filter = level(0), main = level(1), direct = level(2), none = level(3);
    CHECK(none == 0.0);
    CHECK(main == Approx(filter).epsilon(1e-6));    // Filter 1 disabled: same as Main
    CHECK(direct == Approx(main).epsilon(1e-6));    // no FX yet: Direct sounds the same
}

TEST_CASE("filter balance sends a source to Filter 2", "[filter][routing]")
{
    auto level = [](float balance) {
        Rig rig;
        rig.reg("Oscillator0").set("random", 0.0f);
        rig.reg("Oscillator0").set("filterBalance", balance);
        rig.reg("Routing").set("filterRouting", 1);   // parallel
        darkFilter(rig, 1);                           // only Filter 2 is dark
        return rmsOf(rig.renderNote(60, 0.3));
    };
    CHECK(level(1.0f) < 0.5 * level(0.0f));
}

TEST_CASE("serial routing feeds Filter 1 into Filter 2; parallel does not", "[filter][routing]")
{
    auto level = [](int routing) {
        Rig rig;
        rig.reg("Oscillator0").set("random", 0.0f);
        rig.reg("Routing").set("filterRouting", routing);
        darkFilter(rig, 1);   // Filter 1 off (pass-through), Filter 2 dark
        return rmsOf(rig.renderNote(60, 0.3));
    };
    const double serial = level(0), parallel = level(1);
    CHECK(serial < 0.5 * parallel);   // serial: A → F1 → dark F2; parallel: A → F1 → Main
}

TEST_CASE("keytrack raises the cutoff with the note", "[filter][routing]")
{
    // Fraction of the signal a 400 Hz low-pass lets through at C6 (two octaves above C4, fundamental 1047 Hz).
    auto passed = [](float keytrack) {
        auto render = [&](bool filterOn) {
            Rig rig;
            rig.reg("Oscillator0").set("random", 0.0f);
            auto& f = rig.reg("Filter0");
            f.set("enabled", filterOn);
            f.set("cutoff", 400.0f);
            f.set("resonance", 0.0f);
            f.set("keytrack", keytrack);
            return rmsOf(rig.renderNote(84, 0.3));
        };
        return render(true) / render(false);
    };
    const double without = passed(0.0f), with = passed(1.0f);   // with keytrack the cutoff is 1600 Hz
    INFO("passed: keytrack 0 = " << without << ", keytrack 1 = " << with);
    CHECK(with > 3.0 * without);
}

TEST_CASE("audio-rate LFO modulation reaches Filter 2's cutoff", "[filter][routing]")
{
    auto render = [](bool modulated) {
        Rig rig;
        rig.reg("Oscillator0").set("random", 0.0f);
        rig.reg("Oscillator0").set("filterBalance", 1.0f);
        rig.reg("Routing").set("filterRouting", 1);
        auto& f = rig.reg("Filter1");
        f.set("enabled", true);
        f.set("cutoff", 500.0f);
        auto& lfo = rig.reg("LFO0");
        lfo.set("sync", false);
        lfo.set("rate", 200.0f);
        lfo.set("shape", 1);   // sine
        if (modulated) {
            auto& s = rig.reg("ModSlot0");
            s.set("source", 5);   // LFO 1
            s.set<std::string>("destination", "Filter1.cutoff");
            s.set("amount", 0.3f);
            rig.engine.publishSnapshot();
        }
        return rig.renderNote(48, 0.2);
    };
    const auto a = render(false), b = render(true);
    double diff = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) diff = std::max(diff, static_cast<double>(std::abs(a[i] - b[i])));
    CHECK(diff > 0.05);
}
