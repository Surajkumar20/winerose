#include "engine/fx/Effects.h"

#include "engine/dsp/Fft.h"

#include <cmath>
#include <cstdint>

namespace winerose::fx {

using namespace dsp;

// Uniformly-partitioned overlap-save stage covering taps [offset, offset + parts·N) with block size N.
// Requires offset >= N: the result for the next block is ready the moment the current block completes,
// so the stage adds no latency of its own.
struct Convolve::Stage {
    int N = 0, offset = 0, parts = 0;
    std::unique_ptr<winerose::dsp::RealFft> fft;
    std::vector<std::vector<float>> H;   // partition spectra (ordered pffft layout, 2N floats)
    std::vector<std::vector<float>> X;   // frequency-domain delay line of input spectra
    int head = 0;
    std::vector<float> in;               // [previous block | current block]
    std::vector<float> out;              // this block's output, computed at the end of the last one
    std::vector<float> acc, tmp;
    int fill = 0;

    void init(const std::vector<float>& ir, int blockSize, int firstTap, int lastTap)
    {
        N = blockSize;
        offset = firstTap;
        parts = std::max(0, (lastTap - firstTap + N - 1) / N);
        fft = std::make_unique<winerose::dsp::RealFft>(2 * N);
        H.assign(static_cast<std::size_t>(parts), std::vector<float>(static_cast<std::size_t>(2 * N), 0.0f));
        X.assign(static_cast<std::size_t>(parts), std::vector<float>(static_cast<std::size_t>(2 * N), 0.0f));
        in.assign(static_cast<std::size_t>(2 * N), 0.0f);
        out.assign(static_cast<std::size_t>(N), 0.0f);
        acc.assign(static_cast<std::size_t>(2 * N), 0.0f);
        tmp.assign(static_cast<std::size_t>(2 * N), 0.0f);
        std::vector<float> frame(static_cast<std::size_t>(2 * N));
        for (int p = 0; p < parts; ++p) {
            std::fill(frame.begin(), frame.end(), 0.0f);
            for (int j = 0; j < N; ++j) {
                const int tap = offset + p * N + j;
                if (tap < lastTap && tap < static_cast<int>(ir.size())) frame[static_cast<std::size_t>(j)] = ir[static_cast<std::size_t>(tap)];
            }
            fft->forward(frame.data(), H[static_cast<std::size_t>(p)].data());
        }
    }

    void reset() noexcept
    {
        for (auto& x : X) std::fill(x.begin(), x.end(), 0.0f);
        std::fill(in.begin(), in.end(), 0.0f);
        std::fill(out.begin(), out.end(), 0.0f);
        head = 0;
        fill = 0;
    }

    /** One sample in, this stage's contribution out. activeTaps fades out partitions past the length. */
    float process(float x, int activeTaps) noexcept
    {
        const float y = out[static_cast<std::size_t>(fill)];
        in[static_cast<std::size_t>(N + fill)] = x;
        if (++fill == N) {
            fill = 0;
            if (parts > 0) {
                fft->forward(in.data(), X[static_cast<std::size_t>(head)].data());
                std::fill(acc.begin(), acc.end(), 0.0f);
                const float fade = 4.0f * static_cast<float>(N);
                for (int p = 0; p < parts; ++p) {
                    const int start = offset + p * N;
                    const float g = std::clamp((static_cast<float>(activeTaps - start)) / fade, 0.0f, 1.0f);
                    if (g <= 0.0f) break;   // later partitions are all past the length
                    const auto& h = H[static_cast<std::size_t>(p)];
                    const auto& xs = X[static_cast<std::size_t>((head - p + parts) % parts)];
                    acc[0] += g * xs[0] * h[0];   // DC
                    acc[1] += g * xs[1] * h[1];   // Nyquist
                    for (int k = 1; k < N; ++k) {
                        const float xr = xs[static_cast<std::size_t>(2 * k)], xi = xs[static_cast<std::size_t>(2 * k + 1)];
                        const float hr = h[static_cast<std::size_t>(2 * k)], hi = h[static_cast<std::size_t>(2 * k + 1)];
                        acc[static_cast<std::size_t>(2 * k)]     += g * (xr * hr - xi * hi);
                        acc[static_cast<std::size_t>(2 * k + 1)] += g * (xr * hi + xi * hr);
                    }
                }
                fft->inverse(acc.data(), tmp.data());
                std::copy(tmp.begin() + N, tmp.end(), out.begin());   // overlap-save: keep the last N
                head = (head + 1) % parts;
            }
            std::copy(in.begin() + N, in.end(), in.begin());
        }
        return y;
    }
};

Convolve::Convolve() : Effect(FxType::Convolve) {}
Convolve::~Convolve() = default;

void Convolve::prepare(double sr)
{
    m_sr = sr;
    const int length = static_cast<int>(kMaxSeconds * sr);
    // Built-in impulse: decorrelated decaying noise per channel (RT ≈ 1.5 s), energy-normalized.
    for (int c = 0; c < 2; ++c) {
        auto& ir = m_ir[static_cast<std::size_t>(c)];
        ir.assign(static_cast<std::size_t>(length), 0.0f);
        std::uint32_t seed = 0xC0FFEEu + static_cast<std::uint32_t>(c) * 7919u;
        double energy = 0.0;
        for (int i = 0; i < length; ++i) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            const double noise = static_cast<double>(seed) / 2147483648.0 - 1.0;
            const double v = noise * std::exp(-6.907755 * i / (1.5 * sr));
            ir[static_cast<std::size_t>(i)] = static_cast<float>(v);
            energy += v * v;
        }
        const float norm = static_cast<float>(0.5 / std::sqrt(energy));
        for (float& v : ir) v *= norm;
        m_headHistory[static_cast<std::size_t>(c)].assign(static_cast<std::size_t>(2 * kHead), 0.0f);
        m_stages[static_cast<std::size_t>(c * 2)]     = std::make_unique<Stage>();
        m_stages[static_cast<std::size_t>(c * 2)]->init(ir, kSmallBlock, kHead, kLargeBlock);
        m_stages[static_cast<std::size_t>(c * 2 + 1)] = std::make_unique<Stage>();
        m_stages[static_cast<std::size_t>(c * 2 + 1)]->init(ir, kLargeBlock, kLargeBlock, length);
    }
    m_preL.allocate(static_cast<int>(0.26 * sr));
    m_preR.allocate(static_cast<int>(0.26 * sr));
    m_activeLength = length;
    reset();
}

void Convolve::reset() noexcept
{
    for (auto& s : m_stages) if (s) s->reset();
    for (auto& h : m_headHistory) std::fill(h.begin(), h.end(), 0.0f);
    m_headPos = {};
    m_preL.reset(); m_preR.reset();
    for (auto* b : {&m_lowCutL, &m_lowCutR, &m_highCutL, &m_highCutR}) b->reset();
}

void Convolve::setParams(const std::array<float, kParamCount>& p, const FxContext&) noexcept
{
    m_activeLength = static_cast<int>(expo(p[0], 0.05f, static_cast<float>(kMaxSeconds)) * m_sr);
    m_preDelay = std::max(1.0f, static_cast<float>(lin(p[1], 0.0f, 0.25f) * m_sr));
    m_lowCutOn = p[2] > 0.001f;
    m_highCutOn = p[3] < 0.999f;
    m_lowCutL.set(Biquad::Kind::Highpass, expo(p[2], 20.0f, 1000.0f), 0.707, 0.0, m_sr);
    m_lowCutR = m_lowCutL;
    m_highCutL.set(Biquad::Kind::Lowpass, expo(p[3], 1000.0f, 20000.0f), 0.707, 0.0, m_sr);
    m_highCutR = m_highCutL;
}

float Convolve::processSample(int ch, float x) noexcept
{
    // Direct form for the first kHead taps (double-length history keeps the window contiguous).
    auto& hist = m_headHistory[static_cast<std::size_t>(ch)];
    int& pos = m_headPos[static_cast<std::size_t>(ch)];
    hist[static_cast<std::size_t>(pos)] = x;
    hist[static_cast<std::size_t>(pos + kHead)] = x;
    const float* window = hist.data() + pos + kHead;   // window[-j] = x[t - j]
    const auto& ir = m_ir[static_cast<std::size_t>(ch)];
    float y = 0.0f;
    for (int j = 0; j < kHead; ++j) y += ir[static_cast<std::size_t>(j)] * window[-j];
    pos = (pos + 1) % kHead;

    y += m_stages[static_cast<std::size_t>(ch * 2)]->process(x, m_activeLength);
    y += m_stages[static_cast<std::size_t>(ch * 2 + 1)]->process(x, m_activeLength);
    return y;
}

void Convolve::process(float* l, float* r, int n) noexcept
{
    for (int i = 0; i < n; ++i) {
        m_preL.write(l[i]);
        m_preR.write(r[i]);
        const float inL = m_preL.readInt(static_cast<int>(m_preDelay));
        const float inR = m_preR.readInt(static_cast<int>(m_preDelay));
        float yl = processSample(0, inL), yr = processSample(1, inR);
        if (m_lowCutOn)  { yl = m_lowCutL.process(yl);  yr = m_lowCutR.process(yr); }
        if (m_highCutOn) { yl = m_highCutL.process(yl); yr = m_highCutR.process(yr); }
        l[i] = yl;
        r[i] = yr;
    }
}

} // namespace winerose::fx
