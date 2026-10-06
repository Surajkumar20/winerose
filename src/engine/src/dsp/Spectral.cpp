#include "engine/dsp/Spectral.h"

#include <algorithm>
#include <cmath>

namespace winerose::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr int N = SpectralData::kFft;
constexpr int H = SpectralData::kHop;
constexpr int B = SpectralData::kBins;

double princarg(double x) noexcept { return x - kTwoPi * std::floor((x + kPi) / kTwoPi); }

const std::array<float, N>& hann()
{
    static const std::array<float, N> w = [] {
        std::array<float, N> a {};
        for (int i = 0; i < N; ++i) a[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * i / N));
        return a;
    }();
    return w;
}

// True envelope (Röbel & Rodet): iterated cepstral smoothing that rides on top of the harmonic peaks.
// logMag: B bins in, envelope out. The cepstral order follows the frame's pitch (0.5·sr/f0, Röbel's optimum),
// found as the cepstral peak in 50..1000 Hz; frames without a clear pitch use `fallbackOrder`.
void trueEnvelope(RealFft& fft, const float* logMag, float* env, double sampleRate, int fallbackOrder)
{
    std::vector<float> a(logMag, logMag + B), spec(N), cep(N);
    int order = fallbackOrder;
    for (int iter = 0; iter < 24; ++iter) {
        // Real, even log spectrum → cepstrum (inverse FFT of a real spectrum = real-valued cepstrum).
        spec[0] = a[0];
        spec[1] = a[B - 1];
        for (int k = 1; k < B - 1; ++k) { spec[static_cast<std::size_t>(2 * k)] = a[static_cast<std::size_t>(k)]; spec[static_cast<std::size_t>(2 * k + 1)] = 0.0f; }
        fft.inverse(spec.data(), cep.data());
        if (iter == 0) {
            const int qLo = std::max(2, static_cast<int>(sampleRate / 1000.0));
            const int qHi = std::min(N / 2 - 1, static_cast<int>(sampleRate / 50.0));
            int best = -1;
            float bestVal = 0.0f, energy = 0.0f;
            for (int q = qLo; q <= qHi; ++q) {
                energy += cep[static_cast<std::size_t>(q)] * cep[static_cast<std::size_t>(q)];
                if (cep[static_cast<std::size_t>(q)] > bestVal) { bestVal = cep[static_cast<std::size_t>(q)]; best = q; }
            }
            const float rms = std::sqrt(energy / static_cast<float>(qHi - qLo + 1));
            if (best > 0 && bestVal > 4.0f * rms) order = std::clamp(best / 2, 16, N / 4);   // clear pitch peak
        }
        for (int q = order + 1; q < N - order; ++q) cep[static_cast<std::size_t>(q)] = 0.0f;   // lifter
        fft.forward(cep.data(), spec.data());
        float maxGap = 0.0f;
        for (int k = 0; k < B; ++k) {
            const float v = k == 0 ? spec[0] : (k == B - 1 ? spec[1] : spec[static_cast<std::size_t>(2 * k)]);
            env[k] = v;
            const float gap = logMag[k] - v;
            maxGap = std::max(maxGap, gap);
            a[static_cast<std::size_t>(k)] = std::max(logMag[k], v);
        }
        if (maxGap < 0.05f) break;   // within ~0.5 dB of every peak
    }
}

// Transform of the Hann window about its centre, K(v) = sum w(n)·cos(2πv(n - N/2)/N), for v in [-4, 4] bins.
constexpr int kLobeSteps = 256;
constexpr int kLobeHalf = 4;
float g_lobe[2 * kLobeHalf * kLobeSteps + 2];
bool  g_lobeReady = false;

float lobe(double nu) noexcept
{
    const double x = (nu + kLobeHalf) * kLobeSteps;
    if (x < 0.0 || x >= 2 * kLobeHalf * kLobeSteps) return 0.0f;
    const int i = static_cast<int>(x);
    const float u = static_cast<float>(x - i);
    return g_lobe[i] + (g_lobe[i + 1] - g_lobe[i]) * u;
}

} // namespace

void spectral::init() noexcept
{
    if (g_lobeReady) return;
    const auto& w = hann();
    for (int i = 0; i <= 2 * kLobeHalf * kLobeSteps + 1; ++i) {
        const double nu = static_cast<double>(i) / kLobeSteps - kLobeHalf;
        double sum = 0.0;
        for (int n = 0; n < N; ++n) sum += w[static_cast<std::size_t>(n)] * std::cos(kTwoPi * nu * (n - N / 2) / N);
        g_lobe[i] = static_cast<float>(sum);
    }
    g_lobeReady = true;
}

std::shared_ptr<const SpectralData> SpectralData::analyze(const SampleData& sample)
{
    auto d = std::make_shared<SpectralData>();
    d->m_sampleRate = sample.sampleRate();
    d->m_rootKey = sample.rootKey();
    const std::int64_t len = std::min<std::int64_t>(sample.frames(), static_cast<std::int64_t>(kMaxSeconds * sample.sampleRate()));
    const int frames = static_cast<int>(len / H) + 1;
    d->m_frames = frames;
    d->m_env.resize(static_cast<std::size_t>(frames) * B);
    d->m_fine.resize(d->m_env.size());
    d->m_freq.resize(d->m_env.size());
    d->m_phase.resize(d->m_env.size());
    d->m_transient.assign(static_cast<std::size_t>(frames), 0);

    RealFft fft(N);
    const auto& w = hann();
    // Fallback envelope detail (unpitched frames): quefrencies below ~2 ms.
    const int order = std::clamp(static_cast<int>(sample.sampleRate() / 500.0), 24, N / 4);
    std::vector<float> frame(N), spec(N), logMag(B), prevPhase(B, 0.0f), prevMag(B, 0.0f), flux(static_cast<std::size_t>(frames), 0.0f),
                       level(static_cast<std::size_t>(frames), 0.0f);

    for (int f = 0; f < frames; ++f) {
        const std::int64_t start = static_cast<std::int64_t>(f) * H - N / 2;   // frame centred on f·H
        for (int i = 0; i < N; ++i) frame[static_cast<std::size_t>(i)] = sample.monoAt(start + i) * w[static_cast<std::size_t>(i)];
        fft.forward(frame.data(), spec.data());

        float* env = &d->m_env[static_cast<std::size_t>(f) * B];
        float* fine = &d->m_fine[static_cast<std::size_t>(f) * B];
        float* freq = &d->m_freq[static_cast<std::size_t>(f) * B];
        float* ph = &d->m_phase[static_cast<std::size_t>(f) * B];
        float fl = 0.0f, total = 0.0f;
        for (int k = 0; k < B; ++k) {
            float re, im;
            if (k == 0) { re = spec[0]; im = 0.0f; }
            else if (k == B - 1) { re = spec[1]; im = 0.0f; }
            else { re = spec[static_cast<std::size_t>(2 * k)]; im = spec[static_cast<std::size_t>(2 * k + 1)]; }
            const float mag = std::sqrt(re * re + im * im);
            logMag[static_cast<std::size_t>(k)] = std::log(mag + 1e-9f);
            ph[k] = std::atan2(im, re);
            // Instantaneous frequency (bins) from the phase advance over one hop.
            const double expected = kTwoPi * k * H / N;
            const double dev = princarg(static_cast<double>(ph[k]) - prevPhase[static_cast<std::size_t>(k)] - expected);
            freq[k] = static_cast<float>(k + dev * N / (kTwoPi * H));
            prevPhase[static_cast<std::size_t>(k)] = ph[k];
            fl += std::max(0.0f, mag - prevMag[static_cast<std::size_t>(k)]);
            total += mag;
            prevMag[static_cast<std::size_t>(k)] = mag;
        }
        if (f == 0) for (int k = 0; k < B; ++k) freq[k] = static_cast<float>(k);
        flux[static_cast<std::size_t>(f)] = fl;
        level[static_cast<std::size_t>(f)] = total;
        trueEnvelope(fft, logMag.data(), env, sample.sampleRate(), order);
        for (int k = 0; k < B; ++k) fine[k] = logMag[static_cast<std::size_t>(k)] - env[k];
    }

    // Transients: flux peaks well above the local median that are also a real rise in level (at least 20% of
    // the frame's magnitude) — a steady tone's flux is rounding noise and must never reset phases.
    for (int f = 1; f + 1 < frames; ++f) {
        std::array<float, 9> win {};
        int c = 0;
        for (int j = std::max(0, f - 4); j <= std::min(frames - 1, f + 4); ++j) win[static_cast<std::size_t>(c++)] = flux[static_cast<std::size_t>(j)];
        std::nth_element(win.begin(), win.begin() + c / 2, win.begin() + c);
        const float median = win[static_cast<std::size_t>(c / 2)];
        const float v = flux[static_cast<std::size_t>(f)];
        if (v > flux[static_cast<std::size_t>(f - 1)] && v >= flux[static_cast<std::size_t>(f + 1)] && v > 2.0f * median && v > 0.2f * level[static_cast<std::size_t>(f)] && v > 1e-3f)
            d->m_transient[static_cast<std::size_t>(f)] = 1;
    }
    return d;
}

namespace spectral {

void Voice::start(const SpectralData& data, const Params& p) noexcept
{
    m_frame = std::clamp(static_cast<double>(p.position), 0.0, 1.0) * std::max(0, data.frameCount() - 1);
    m_lastFrame = -1;
    m_acc.fill(0.0f);
    m_outPos = SpectralData::kHop;
    m_first = true;
}

void Voice::synthesize(const SpectralData& data, const Params& p, RealFft& fft) noexcept
{
    const int frames = data.frameCount();
    if (frames <= 0) return;
    const int i0 = std::clamp(static_cast<int>(std::floor(m_frame)), 0, frames - 1);
    const int i1 = std::min(i0 + 1, frames - 1);
    const float t = static_cast<float>(std::clamp(m_frame - i0, 0.0, 1.0));
    const float *e0 = data.envelope(i0), *e1 = data.envelope(i1);
    const float *f0 = data.fine(i0), *f1 = data.fine(i1);
    const float *q0 = data.frequency(i0), *q1 = data.frequency(i1);
    const float* aph = data.phase(t < 0.5f ? i0 : i1);

    // Output bin k <-> k·sr/N Hz. srcPerOut converts an output bin to the source bin of the same frequency.
    const double srcPerOut = p.sampleRate / data.sampleRate();
    const double ratio = p.pitch;
    // Envelope lookup ratio: formant = 1 keeps formants in place; 0 moves them with the pitch (plain shift).
    const double envRatio = std::exp2(static_cast<double>(p.timbreSemis) / 12.0)
                          * std::pow(ratio, 1.0 - std::clamp(static_cast<double>(p.formant), 0.0, 1.0));
    const double kLo = std::max(1.0, static_cast<double>(p.lowCutHz) * N / p.sampleRate);
    const double kHi = std::min(B - 2.0, static_cast<double>(p.highCutHz) * N / p.sampleRate);

    auto envAt = [&](double srcBin) {
        if (srcBin < 0.0) srcBin = 0.0;
        if (srcBin >= B - 2) return -30.0f;
        const int j = static_cast<int>(srcBin);
        const float u = static_cast<float>(srcBin - j);
        const float a = e0[j] + (e0[j + 1] - e0[j]) * u;
        const float b = e1[j] + (e1[j + 1] - e1[j]) * u;
        return a + (b - a) * t;
    };

    // Source log magnitude (frame-interpolated) and its partials: local maxima, minus the side lobes of a
    // stronger neighbour (same instantaneous frequency within 1.5 bins).
    float* srcMag = m_mag.data();
    for (int j = 0; j < B; ++j) srcMag[j] = (e0[j] + (e1[j] - e0[j]) * t) + (f0[j] + (f1[j] - f0[j]) * t);
    int peaks = 0;
    for (int j = 2; j < B - 2 && peaks < kMaxPeaks; ++j) {
        const float m = srcMag[j];
        if (!(m > srcMag[j - 1] && m >= srcMag[j + 1] && m > -18.0f)) continue;
        const float q = q0[j] + (q1[j] - q0[j]) * t;
        if (peaks > 0) {
            const int prev = m_peaks[static_cast<std::size_t>(peaks - 1)];
            const float qp = q0[prev] + (q1[prev] - q0[prev]) * t;
            if (std::abs(q - qp) < 1.5f) {
                if (m > srcMag[prev]) m_peaks[static_cast<std::size_t>(peaks - 1)] = static_cast<std::int16_t>(j);
                continue;
            }
        }
        m_peaks[static_cast<std::size_t>(peaks++)] = static_cast<std::int16_t>(j);
    }

    bool reset = m_first;
    if (p.transients)
        for (int f = std::max(m_lastFrame + 1, 0); f <= i0 && !reset; ++f) reset = data.transient(f);
    m_lastFrame = i0;

    // Each partial is re-drawn at its exact (fractional) target frequency from the analysis window's own
    // transform: X(k) = (A/2)·K(k - v)·e^{i(phc - πk)}, phc = the partial's phase at the window centre, which
    // advances by its frequency every hop. This keeps every partial coherent across overlapping frames.
    float* re = m_work.data();
    float* im = m_work.data() + B;
    std::fill(re, re + 2 * B, 0.0f);
    const double advance = kTwoPi * H / N;
    std::array<float, SpectralData::kBins> newCentre;
    std::fill(newCentre.begin(), newCentre.end(), 0.0f);
    std::array<std::uint8_t, SpectralData::kBins> written {};
    for (int pk = 0; pk < peaks; ++pk) {
        const int jp = m_peaks[static_cast<std::size_t>(pk)];
        const double freqSrc = q0[jp] + (q1[jp] - q0[jp]) * t;          // source bins
        const double freqOut = freqSrc * ratio / srcPerOut;             // output bins
        if (freqOut < kLo || freqOut > kHi) continue;
        const float kernelAtPeak = lobe(static_cast<double>(jp) - freqSrc);
        if (kernelAtPeak <= 1e-3f) continue;
        // Formant handling per partial: it takes the envelope level of where it lands, through envRatio.
        const float corr = envAt(freqOut * srcPerOut / envRatio) - envAt(freqSrc);
        const float amp = std::exp(srcMag[jp] + corr) / kernelAtPeak;   // = A/2

        const int kc = static_cast<int>(std::lround(freqOut));
        const float centre = reset ? static_cast<float>(princarg(aph[jp] + kPi * jp))
                                   : static_cast<float>(princarg(m_centre[static_cast<std::size_t>(kc)] + freqOut * advance));
        for (int d = -1; d <= 1; ++d) {   // remember for the next frame (tolerates a one-bin drift)
            const int k = kc + d;
            if (k >= 0 && k < B && (written[static_cast<std::size_t>(k)] == 0 || d == 0)) {
                newCentre[static_cast<std::size_t>(k)] = centre;
                written[static_cast<std::size_t>(k)] = 1;
            }
        }
        for (int k = std::max(1, kc - 3); k <= std::min(B - 2, kc + 3); ++k) {
            const float m = amp * lobe(static_cast<double>(k) - freqOut);
            const float ph = centre - static_cast<float>(kPi) * static_cast<float>(k & 1);   // e^{-iπk}
            re[k] += m * std::cos(ph);
            im[k] += m * std::sin(ph);
        }
    }
    m_centre = newCentre;

    // Spectrum -> time, synthesis window, overlap-add (Hann^2 at 4x overlap sums to 1.5).
    float* spec = m_time.data();
    spec[0] = 0.0f;
    spec[1] = 0.0f;
    for (int k = 1; k < B - 1; ++k) {
        spec[2 * k] = re[k];
        spec[2 * k + 1] = im[k];
    }
    float* time = m_work.data();
    fft.inverse(spec, time);
    const auto& w = hann();
    constexpr float kNorm = 1.0f / 1.5f;
    for (int i = 0; i < N; ++i) m_acc[static_cast<std::size_t>(i)] += time[i] * w[static_cast<std::size_t>(i)] * kNorm;

    std::copy(m_acc.begin(), m_acc.begin() + H, m_out.begin());
    std::copy(m_acc.begin() + H, m_acc.end(), m_acc.begin());
    std::fill(m_acc.end() - H, m_acc.end(), 0.0f);
    m_outPos = 0;
    m_first = false;

    // Advance the analysis position by one output hop (scan 1 = original speed).
    m_frame += static_cast<double>(p.scan) * (p.sampleRate > 0.0 ? data.sampleRate() / p.sampleRate : 1.0);
    if (m_frame >= frames - 1) m_frame = frames - 1;   // hold the last frame (sustain)
    if (m_frame < 0.0) m_frame = 0.0;
}

void Voice::render(const SpectralData& data, const Params& p, RealFft& fft, float* outL, float* outR, int n) noexcept
{
    for (int i = 0; i < n; ++i) {
        if (m_outPos >= H) synthesize(data, p, fft);
        const float v = m_out[static_cast<std::size_t>(m_outPos++)];
        outL[i] += v;
        outR[i] += v;
    }
}

} // namespace spectral
} // namespace winerose::dsp
