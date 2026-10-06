#include "engine/dsp/SampleData.h"

#include <algorithm>
#include <atomic>

namespace winerose::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

double besselI0(double x) noexcept
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

double kaiser(double t, double beta) noexcept   // t in [-1, 1]
{
    if (t <= -1.0 || t >= 1.0) return 0.0;
    return besselI0(beta * std::sqrt(1.0 - t * t)) / besselI0(beta);
}

double sincFn(double x) noexcept { return x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x); }

// Half-band decimation filter: 2:1, passband to ~0.2 fs, stopband from 0.25 fs (of the input rate).
std::vector<float> decimate(const std::vector<float>& in)
{
    constexpr int kHalf = 31;
    static const std::array<float, 2 * kHalf + 1> h = [] {
        std::array<float, 2 * kHalf + 1> c {};
        double sum = 0.0;
        for (int i = -kHalf; i <= kHalf; ++i) {
            const double v = 0.45 * sincFn(0.45 * i) * kaiser(static_cast<double>(i) / (kHalf + 1), 9.0);
            c[static_cast<std::size_t>(i + kHalf)] = static_cast<float>(v);
            sum += v;
        }
        for (auto& x : c) x = static_cast<float>(x / sum);
        return c;
    }();
    const auto n = static_cast<std::int64_t>(in.size());
    std::vector<float> out(static_cast<std::size_t>((n + 1) / 2));
    for (std::int64_t o = 0; o < static_cast<std::int64_t>(out.size()); ++o) {
        const std::int64_t centre = 2 * o;
        double acc = 0.0;
        for (int k = -kHalf; k <= kHalf; ++k) {
            const std::int64_t i = centre + k;
            if (i >= 0 && i < n) acc += static_cast<double>(in[static_cast<std::size_t>(i)]) * h[static_cast<std::size_t>(k + kHalf)];
        }
        out[static_cast<std::size_t>(o)] = static_cast<float>(acc);
    }
    return out;
}

std::vector<float> padded(const std::vector<float>& x)
{
    std::vector<float> p(x.size() + 2 * SampleData::kPad, 0.0f);
    std::copy(x.begin(), x.end(), p.begin() + SampleData::kPad);
    return p;
}

// [cutoff][phase 0..kPhases][tap]
alignas(16) float g_kernel[sinc::kCutoffs][sinc::kPhases + 1][sinc::kTaps];
bool g_ready = false;

} // namespace

std::uint64_t nextAssetId() noexcept
{
    static std::atomic<std::uint64_t> next {1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<const SampleData> SampleData::build(std::vector<float> left, std::vector<float> right, double sampleRate,
                                                    int rootKey, std::int64_t loopStart, std::int64_t loopEnd, std::string name)
{
    auto s = std::make_shared<SampleData>();
    s->m_id = nextAssetId();
    s->m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    s->m_channels = right.empty() ? 1 : 2;
    if (!right.empty()) right.resize(left.size(), 0.0f);
    s->m_frames = static_cast<std::int64_t>(left.size());
    s->m_rootKey = std::clamp(rootKey, 0, 127);
    s->m_name = std::move(name);
    if (loopStart >= 0 && loopEnd > loopStart + 1 && loopEnd <= s->m_frames) {
        s->m_loopStart = loopStart;
        s->m_loopEnd = loopEnd;
    }

    std::array<std::vector<float>, 2> current {std::move(left), std::move(right)};
    for (int level = 0; level < kMaxLevels; ++level) {
        std::array<std::vector<float>, 2> stored;
        for (int ch = 0; ch < s->m_channels; ++ch) stored[static_cast<std::size_t>(ch)] = padded(current[static_cast<std::size_t>(ch)]);
        s->m_levels.push_back(std::move(stored));
        if (current[0].size() < 64) break;
        for (int ch = 0; ch < s->m_channels; ++ch) current[static_cast<std::size_t>(ch)] = decimate(current[static_cast<std::size_t>(ch)]);
    }
    return s;
}

// --- sinc ------------------------------------------------------------------------------------------------

void sinc::init() noexcept
{
    if (g_ready) return;
    const double cutoffs[kCutoffs] = {0.92, 0.92 * std::pow(2.0, -0.25), 0.92 * std::pow(2.0, -0.5), 0.92 * std::pow(2.0, -0.75), 0.46};
    for (int c = 0; c < kCutoffs; ++c) {
        const double fc = cutoffs[c];
        for (int p = 0; p <= kPhases; ++p) {
            const double frac = static_cast<double>(p) / kPhases;
            double sum = 0.0;
            double taps[kTaps];
            for (int t = 0; t < kTaps; ++t) {
                const double x = static_cast<double>(t - (kTaps / 2 - 1)) - frac;   // tap offsets -7 .. +8
                taps[t] = fc * sincFn(fc * x) * kaiser(x / (kTaps / 2), 7.5);
                sum += taps[t];
            }
            for (int t = 0; t < kTaps; ++t) g_kernel[c][p][t] = static_cast<float>(taps[t] / sum);   // unity DC gain
        }
    }
    g_ready = true;
}

sinc::ReadSetup sinc::setup(double increment, int levelCount) noexcept
{
    ReadSetup r;
    const double inc = std::abs(increment);
    if (inc <= 1.0) return r;
    int level = static_cast<int>(std::floor(std::log2(inc)));
    level = std::clamp(level, 0, std::max(0, levelCount - 1));
    r.level = level;
    r.scale = 1.0 / static_cast<double>(1 << level);
    const double rel = inc * r.scale;   // in [1, 2) unless clamped at the top level
    r.cutoff = rel <= 1.0 ? 0 : std::clamp(1 + static_cast<int>(std::floor(std::log2(rel) * 4.0)), 1, kCutoffs - 1);
    return r;
}

namespace {
inline void kernelAt(double pos, int cutoff, std::int64_t& base, float (&k)[sinc::kTaps]) noexcept
{
    const double fl = std::floor(pos);
    base = static_cast<std::int64_t>(fl) - (sinc::kTaps / 2 - 1);
    const double f = (pos - fl) * sinc::kPhases;
    const int p = std::min(static_cast<int>(f), sinc::kPhases - 1);
    const float t = static_cast<float>(f - p);
    const float* a = g_kernel[cutoff][p];
    const float* b = g_kernel[cutoff][p + 1];
    for (int i = 0; i < sinc::kTaps; ++i) k[i] = a[i] + (b[i] - a[i]) * t;
}
}

float sinc::read(const float* data, double pos, int cutoff) noexcept
{
    std::int64_t base;
    float k[kTaps];
    kernelAt(pos, cutoff, base, k);
    const float* d = data + base;
    float acc = 0.0f;
    for (int i = 0; i < kTaps; ++i) acc += d[i] * k[i];
    return acc;
}

void sinc::read2(const float* left, const float* right, double pos, int cutoff, float& outL, float& outR) noexcept
{
    std::int64_t base;
    float k[kTaps];
    kernelAt(pos, cutoff, base, k);
    const float* l = left + base;
    const float* r = right + base;
    float al = 0.0f, ar = 0.0f;
    for (int i = 0; i < kTaps; ++i) {
        al += l[i] * k[i];
        ar += r[i] * k[i];
    }
    outL = al;
    outR = ar;
}

// --- SamplePlayer ----------------------------------------------------------------------------------------

void SamplePlayer::render(const SampleData& s, float* outL, float* outR, int n, double pitch) noexcept
{
    if (!active || s.frames() == 0) return;
    const double inc = rate * pitch;
    const auto rs = sinc::setup(inc, s.levelCount());
    const float* l = s.data(rs.level, 0);
    const float* r = s.data(rs.level, 1);
    const bool stereo = s.channels() == 2;
    const double maxPos = static_cast<double>(s.levelFrames(rs.level)) + SampleData::kPad - 9;

    const bool looping = (loop == Loop::Forward || loop == Loop::PingPong || (loop == Loop::Sustain && !released))
                      && loopEnd > loopStart + 1;
    const double ls = static_cast<double>(loopStart), le = static_cast<double>(loopEnd), len = le - ls;
    const double xf = loop == Loop::PingPong ? 0.0 : std::min({xfade, len * 0.5, ls});

    auto at = [&](double p, float& a, float& b) {
        const double q = std::min(p * rs.scale, maxPos);
        if (stereo) sinc::read2(l, r, q, rs.cutoff, a, b);
        else { a = sinc::read(l, q, rs.cutoff); b = a; }
    };

    for (int i = 0; i < n; ++i) {
        float a, b;
        at(pos, a, b);
        if (looping && xf > 0.0 && dir > 0 && pos >= le - xf) {
            // Equal-power crossfade into the audio leading up to the loop start.
            const double t = (pos - (le - xf)) / xf;
            float a2, b2;
            at(pos - len, a2, b2);
            const float g1 = static_cast<float>(std::cos(t * 1.5707963267948966));
            const float g2 = static_cast<float>(std::sin(t * 1.5707963267948966));
            a = a * g1 + a2 * g2;
            b = b * g1 + b2 * g2;
        }
        outL[i] += a * gainL;
        outR[i] += b * gainR;

        pos += inc * dir;
        if (looping) {
            if (loop == Loop::PingPong) {
                if (dir > 0 && pos >= le) { pos = 2.0 * le - pos; dir = -1; }
                else if (dir < 0 && pos <= ls) { pos = 2.0 * ls - pos; dir = 1; }
            } else if (pos >= le) {
                pos -= len;
            }
        } else if (pos >= static_cast<double>(end) || pos < 0.0) {
            active = false;
            return;
        }
    }
}

} // namespace winerose::dsp
