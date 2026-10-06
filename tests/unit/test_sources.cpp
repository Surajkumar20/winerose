// Phase 7 DSP (SPEC §1.4): sample reads, sample player loops, granular pool, spectral resynthesis.

#include "engine/dsp/Fft.h"
#include "engine/dsp/Granular.h"
#include "engine/dsp/SampleData.h"
#include "engine/dsp/Spectral.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace winerose::dsp;
using Catch::Approx;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSr = 48000.0;

std::vector<float> sine(double freq, double seconds, double sr = kSr, double amp = 0.5)
{
    std::vector<float> x(static_cast<std::size_t>(seconds * sr));
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) / sr));
    return x;
}

double db(double a) { return a > 0.0 ? 20.0 * std::log10(a) : -400.0; }

// Amplitude of the strongest partial within ±halfWidth bins of freq: Hann-windowed FFT, summing the power of
// its main lobe (±2 bins) so the reading doesn't depend on where the partial falls between bins.
double toneAmplitude(const std::vector<float>& x, double freq, double sr = kSr, int halfWidth = 2)
{
    const int n = static_cast<int>(x.size());
    RealFft fft(n);
    std::vector<float> w(x.size()), s(x.size());
    for (int i = 0; i < n; ++i) w[static_cast<std::size_t>(i)] = x[static_cast<std::size_t>(i)] * static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / n));
    fft.forward(w.data(), s.data());
    auto power = [&](int k) {
        if (k < 1 || k >= n / 2) return 0.0;
        const double re = s[static_cast<std::size_t>(2 * k)], im = s[static_cast<std::size_t>(2 * k + 1)];
        return re * re + im * im;
    };
    const int centre = static_cast<int>(std::lround(freq * n / sr));
    int peak = centre;
    for (int k = centre - halfWidth; k <= centre + halfWidth; ++k) if (power(k) > power(peak)) peak = k;
    double lobe = 0.0;
    for (int k = peak - 2; k <= peak + 2; ++k) lobe += power(k);
    return std::sqrt(32.0 * lobe / (3.0 * static_cast<double>(n) * n));   // Parseval with Σw² = 3N/8
}

struct Init {
    Init() { sinc::init(); granular::init(); spectral::init(); }
} g_init;

} // namespace

// --- Sample reads ----------------------------------------------------------------------------------------

TEST_CASE("sinc reads reconstruct a sine at fractional positions", "[sources][sample]")
{
    const auto data = SampleData::build(sine(1000.0, 0.2), {}, kSr);
    double worst = 0.0;
    for (int i = 0; i < 2000; ++i) {
        const double pos = 1000.0 + i * 0.37;
        const double expected = 0.5 * std::sin(2.0 * kPi * 1000.0 * pos / kSr);
        worst = std::max(worst, std::abs(sinc::read(data->data(0, 0), pos, 0) - expected));
    }
    CHECK(db(worst / 0.5) < -70.0);   // a 16-tap Kaiser kernel ripples at about -75 dB
}

TEST_CASE("reading faster than real time uses mip levels and does not alias", "[sources][sample]")
{
    // 14.4 kHz read at 2x would land at 28.8 kHz (above Nyquist): it must be removed, not folded to 19.2 kHz.
    const auto high = SampleData::build(sine(14400.0, 1.0), {}, kSr);
    const auto low  = SampleData::build(sine(4800.0, 1.0), {}, kSr);
    for (const auto& [data, keep] : {std::pair{high, false}, std::pair{low, true}}) {
        SamplePlayer p;
        p.start = 0;
        p.end = data->frames();
        p.rate = 2.0;
        p.begin(0.0);
        std::vector<float> l(8192, 0.0f), r(8192, 0.0f);
        p.render(*data, l.data(), r.data(), 8192, 1.0);
        double peak = 0.0;   // steady state: skip the onset (a sine switched on at full scale is broadband)
        for (std::size_t i = 256; i < l.size(); ++i) peak = std::max(peak, static_cast<double>(std::abs(l[i])));
        if (keep) CHECK(toneAmplitude(l, 9600.0) == Approx(0.5).margin(0.02));
        else      CHECK(db(peak / 0.5) < -60.0);
    }
}

TEST_CASE("forward loop with crossfade is continuous; ping-pong stays inside the loop", "[sources][sample]")
{
    // Loop points deliberately NOT on a cycle boundary: without the crossfade the seam would jump.
    const auto data = SampleData::build(sine(441.0, 1.0), {}, kSr);
    SamplePlayer p;
    p.start = 0;
    p.end = data->frames();
    p.loop = SamplePlayer::Loop::Forward;
    p.loopStart = 10000;
    p.loopEnd = 20037;
    p.xfade = 2000.0;
    p.rate = 1.0;
    p.begin(0.0);
    std::vector<float> l(96000, 0.0f), r(96000, 0.0f);
    p.render(*data, l.data(), r.data(), 96000, 1.0);
    CHECK(p.active);
    double maxStep = 0.0;
    for (std::size_t i = 1; i < l.size(); ++i) maxStep = std::max(maxStep, static_cast<double>(std::abs(l[i] - l[i - 1])));
    const double sineStep = 0.5 * 2.0 * kPi * 441.0 / kSr;   // largest step of the source itself
    // An equal-power fade of two same-frequency signals can reach sqrt(2) amplitude mid-fade; a seam jump
    // without the crossfade would be ~10x the per-sample step of the source.
    CHECK(maxStep < sineStep * 1.5);

    SamplePlayer q = p;
    q.loop = SamplePlayer::Loop::PingPong;
    q.begin(0.0);
    for (int block = 0; block < 300; ++block) {
        float a[64] = {}, b[64] = {};
        q.render(*data, a, b, 64, 1.0);
        if (block > 200) {   // past the attack (playback reaches the loop at sample 10000)
            CHECK(q.pos >= 10000.0 - 1.0);
            CHECK(q.pos <= 20037.0 + 1.0);
        }
    }
}

TEST_CASE("one-shot playback stops at the end; sustain loops until release", "[sources][sample]")
{
    const auto data = SampleData::build(sine(500.0, 0.1), {}, kSr);
    SamplePlayer p;
    p.start = 0;
    p.end = data->frames();
    p.rate = 1.0;
    p.begin(0.0);
    std::vector<float> l(10000, 0.0f), r(10000, 0.0f);
    p.render(*data, l.data(), r.data(), 10000, 1.0);
    CHECK_FALSE(p.active);

    p.loop = SamplePlayer::Loop::Sustain;
    p.loopStart = 1000;
    p.loopEnd = 2000;
    p.begin(0.0);
    p.render(*data, l.data(), r.data(), 10000, 1.0);
    CHECK(p.active);
    p.released = true;
    p.render(*data, l.data(), r.data(), 10000, 1.0);
    CHECK_FALSE(p.active);
}

// --- Granular --------------------------------------------------------------------------------------------

TEST_CASE("granular: the grain pool caps the cost whatever the settings", "[sources][granular]")
{
    const auto data = SampleData::build(sine(220.0, 2.0), sine(330.0, 2.0), kSr);
    granular::Params p;
    p.sizeMs = 500.0f;     // 24,000-sample grains
    p.density = 1000.0f;   // 1000 grains/s → would need 500 concurrent grains
    p.posRandom = 1.0f;
    p.pitchRandom = 1.0f;
    p.panRandom = 1.0f;
    p.sampleRate = kSr;
    granular::Engine g;
    g.start(p, 1234);
    std::vector<float> l(48000, 0.0f), r(48000, 0.0f);
    int maxGrains = 0;
    for (int i = 0; i < 48000; i += 32) {
        g.render(*data, p, l.data() + i, r.data() + i, 32);
        maxGrains = std::max(maxGrains, g.activeGrains());
    }
    CHECK(maxGrains == granular::kMaxGrains);
    double energy = 0.0;
    for (float v : l) energy += static_cast<double>(v) * v;
    CHECK(energy > 1.0);
    for (float v : l) REQUIRE(std::isfinite(v));
}

TEST_CASE("granular: every window shape, frozen and scanning playheads", "[sources][granular]")
{
    const auto data = SampleData::build(sine(440.0, 1.0), {}, kSr);
    for (int w = 0; w < static_cast<int>(granular::Window::Count); ++w) {
        granular::Params p;
        p.window = static_cast<granular::Window>(w);
        p.sizeMs = 50.0f;
        p.density = 40.0f;   // 2 overlapping grains
        p.scan = w % 2 == 0 ? 0.0f : 1.0f;
        p.sampleRate = kSr;
        granular::Engine g;
        g.start(p, 99);
        std::vector<float> l(24000, 0.0f), r(24000, 0.0f);
        g.render(*data, p, l.data(), r.data(), 24000);
        INFO("window " << granular::kWindowNames[w]);
        // A 440 Hz source read at pitch 1 keeps its frequency in every grain.
        CHECK(toneAmplitude(l, 440.0, kSr, 6) > 0.05);
        CHECK(g.activeGrains() <= 3);
    }
}

// --- Spectral --------------------------------------------------------------------------------------------

TEST_CASE("spectral: resynthesis at pitch 1 keeps frequency and level; +/-12 st moves the partial", "[sources][spectral]")
{
    const auto data = SampleData::build(sine(440.0, 2.0, kSr, 0.5), {}, kSr);
    const auto analysis = SpectralData::analyze(*data);
    RealFft fft(SpectralData::kFft);
    for (const double ratio : {1.0, 2.0, 0.5}) {
        spectral::Params p;
        p.pitch = ratio;
        p.sampleRate = kSr;
        auto voice = std::make_unique<spectral::Voice>();
        voice->start(*analysis, p);
        std::vector<float> l(65536, 0.0f), r(65536, 0.0f);
        voice->render(*analysis, p, fft, l.data(), r.data(), 65536);
        const std::vector<float> steady(l.begin() + 8192, l.begin() + 8192 + 32768);
        INFO("ratio " << ratio);
        CHECK(db(toneAmplitude(steady, 440.0 * ratio) / 0.5) == Approx(0.0).margin(1.0));
        CHECK(db(toneAmplitude(steady, 440.0 * ratio * 1.5, kSr, 1) / 0.5) < -30.0);   // no stray partials
    }
}

TEST_CASE("spectral: +/-12 st with < 1% formant drift (SPEC Phase 7)", "[sources][spectral]")
{
    // A vowel-like source: 110 Hz harmonics shaped by two broad formants (700 Hz, 1800 Hz).
    auto envelope = [](double f) {
        return 0.02 + std::exp(-0.5 * std::pow((f - 700.0) / 160.0, 2.0)) + 0.6 * std::exp(-0.5 * std::pow((f - 1800.0) / 220.0, 2.0));
    };
    const double f0 = 110.0;
    std::vector<float> src(static_cast<std::size_t>(2.0 * kSr), 0.0f);
    for (int h = 1; h * f0 < 6000.0; ++h) {
        const double a = 0.05 * envelope(h * f0), ph = 0.7 * h * h;
        for (std::size_t i = 0; i < src.size(); ++i) src[i] += static_cast<float>(a * std::sin(2.0 * kPi * h * f0 * static_cast<double>(i) / kSr + ph));
    }
    const auto data = SampleData::build(src, {}, kSr);
    const auto analysis = SpectralData::analyze(*data);
    RealFft fft(SpectralData::kFft);

    // Formant position = energy centroid of the harmonics within 350..1100 Hz (around the first formant).
    auto centroid = [&](const std::vector<float>& x, double fundamental, bool ideal) {
        double num = 0.0, den = 0.0;
        for (int h = 1; h * fundamental < 1100.0; ++h) {
            const double f = h * fundamental;
            if (f < 350.0) continue;
            const double a = ideal ? envelope(f) : toneAmplitude(x, f, kSr, 1);
            num += f * a * a;
            den += a * a;
        }
        return num / den;
    };

    for (const double semis : {12.0, -12.0}) {
        spectral::Params p;
        p.pitch = std::exp2(semis / 12.0);
        p.formant = 1.0f;
        p.scan = 0.0f;   // freeze on one frame: a steady vowel
        p.position = 0.5f;
        p.sampleRate = kSr;
        auto voice = std::make_unique<spectral::Voice>();
        voice->start(*analysis, p);
        std::vector<float> l(98304, 0.0f), r(98304, 0.0f);
        voice->render(*analysis, p, fft, l.data(), r.data(), 98304);
        const std::vector<float> steady(l.begin() + 16384, l.begin() + 16384 + 65536);

        const double fundamental = f0 * p.pitch;
        const double measured = centroid(steady, fundamental, false);
        const double reference = centroid(steady, fundamental, true);   // formants exactly preserved
        const double naive = [&] {                                       // formants moved with the pitch
            double num = 0.0, den = 0.0;
            for (int h = 1; h * fundamental < 1100.0; ++h) {
                const double f = h * fundamental;
                if (f < 350.0) continue;
                const double a = envelope(f / p.pitch);
                num += f * a * a;
                den += a * a;
            }
            return num / den;
        }();
        std::printf("formant drift at %+.0f st: measured %.1f Hz, preserved %.1f Hz (%.2f%%), unpreserved %.1f Hz\n", semis, measured,
                    reference, 100.0 * std::abs(measured - reference) / reference, naive);
        INFO(semis << " st: measured " << measured << " Hz, preserved " << reference << " Hz, unpreserved " << naive << " Hz");
        CHECK(std::abs(measured - reference) / reference < 0.01);
        CHECK(std::abs(naive - reference) / reference > 0.03);   // the metric can tell the difference
    }
}

TEST_CASE("spectral: timbre shift moves the formants without changing the pitch", "[sources][spectral]")
{
    std::vector<float> src(static_cast<std::size_t>(kSr), 0.0f);
    for (int h = 1; h * 200.0 < 8000.0; ++h) {
        const double a = 0.05 * std::exp(-0.5 * std::pow((h * 200.0 - 1000.0) / 200.0, 2.0)) + 0.002;
        for (std::size_t i = 0; i < src.size(); ++i) src[i] += static_cast<float>(a * std::sin(2.0 * kPi * h * 200.0 * static_cast<double>(i) / kSr));
    }
    const auto data = SampleData::build(src, {}, kSr);
    const auto analysis = SpectralData::analyze(*data);
    RealFft fft(SpectralData::kFft);
    spectral::Params p;
    p.timbreSemis = 12.0f;   // formant at 1000 Hz → 2000 Hz
    p.scan = 0.0f;
    p.position = 0.5f;
    p.sampleRate = kSr;
    auto voice = std::make_unique<spectral::Voice>();
    voice->start(*analysis, p);
    std::vector<float> l(65536, 0.0f), r(65536, 0.0f);
    voice->render(*analysis, p, fft, l.data(), r.data(), 65536);
    const std::vector<float> steady(l.begin() + 16384, l.begin() + 16384 + 32768);
    CHECK(toneAmplitude(steady, 2000.0) > 4.0 * toneAmplitude(steady, 1000.0));
    CHECK(toneAmplitude(steady, 200.0 * 10) > 0.01);   // still 200 Hz harmonics
}

TEST_CASE("spectral analysis flags a transient", "[sources][spectral]")
{
    std::vector<float> src(static_cast<std::size_t>(kSr), 0.0f);
    for (std::size_t i = 24000; i < 26000; ++i) src[i] = static_cast<float>(0.8 * std::sin(0.3 * static_cast<double>(i)) * std::exp(-(static_cast<double>(i) - 24000.0) / 400.0));
    const auto analysis = SpectralData::analyze(*SampleData::build(src, {}, kSr));
    int flagged = -1;
    for (int f = 0; f < analysis->frameCount(); ++f) if (analysis->transient(f)) { flagged = f; break; }
    REQUIRE(flagged >= 0);
    CHECK(std::abs(flagged * SpectralData::kHop - 24000) <= 2 * SpectralData::kHop);
}

