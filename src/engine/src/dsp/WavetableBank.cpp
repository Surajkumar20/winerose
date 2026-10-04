#include "engine/dsp/WavetableBank.h"

#include "engine/dsp/Fft.h"

#include <algorithm>
#include <cmath>

namespace winerose::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Cubic (Catmull-Rom) resample of one periodic frame to kFrameSize. Only used for imported frames that
// aren't already 2048 samples; the mipmap build band-limits the result afterwards.
std::vector<float> resampleFrame(const float* src, int srcSize)
{
    std::vector<float> out(WavetableBank::kFrameSize);
    for (int i = 0; i < WavetableBank::kFrameSize; ++i) {
        const double pos = static_cast<double>(i) * srcSize / WavetableBank::kFrameSize;
        const int    k   = static_cast<int>(pos);
        const float  t   = static_cast<float>(pos - k);
        auto at = [&](int j) { return src[((j % srcSize) + srcSize) % srcSize]; };
        const float xm1 = at(k - 1), x0 = at(k), x1 = at(k + 1), x2 = at(k + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        out[static_cast<std::size_t>(i)] = ((c3 * t + c2) * t + c1) * t + x0;
    }
    return out;
}

} // namespace

std::shared_ptr<const WavetableBank> WavetableBank::build(const std::vector<float>& frames, int frameSize,
                                                          std::string name)
{
    std::shared_ptr<WavetableBank> bank(new WavetableBank());
    bank->m_name = std::move(name);

    const bool valid = frameSize >= 64 && frameSize <= 8192 && !frames.empty();
    const int count = valid ? std::clamp(static_cast<int>(frames.size() / static_cast<std::size_t>(frameSize)), 1, kMaxFrames) : 1;
    bank->m_frameCount = count;
    bank->m_data.assign(static_cast<std::size_t>(kLevels) * static_cast<std::size_t>(count) * kStride, 0.0f);
    if (!valid) return bank;

    RealFft fft(kFrameSize);
    std::vector<float> frame(kFrameSize), spectrum(kFrameSize), limited(kFrameSize), level(kFrameSize);

    for (int f = 0; f < count; ++f) {
        const float* src = frames.data() + static_cast<std::size_t>(f) * static_cast<std::size_t>(frameSize);
        if (frameSize == kFrameSize) std::copy(src, src + kFrameSize, frame.begin());
        else                         frame = resampleFrame(src, frameSize);

        fft.forward(frame.data(), spectrum.data());
        spectrum[0] = 0.0f;   // remove DC: an oscillator offset would thump through the filter and envelope

        for (int L = 0; L < kLevels; ++L) {
            const int keep = maxHarmonic(L);
            limited = spectrum;
            if (keep < kFrameSize / 2) limited[1] = 0.0f;   // Nyquist bin only survives at level 0
            for (int k = keep + 1; k < kFrameSize / 2; ++k) {
                limited[static_cast<std::size_t>(2 * k)]     = 0.0f;
                limited[static_cast<std::size_t>(2 * k + 1)] = 0.0f;
            }
            fft.inverse(limited.data(), level.data());

            float* dst = const_cast<float*>(bank->frameData(L, f));
            std::copy(level.begin(), level.end(), dst);
            for (int g = 1; g <= kGuardPre; ++g) dst[-g] = level[static_cast<std::size_t>(kFrameSize - g)];
            for (int g = 0; g < kGuardPost; ++g) dst[kFrameSize + g] = level[static_cast<std::size_t>(g)];
        }
    }
    return bank;
}

std::shared_ptr<const WavetableBank> makeBasicShapesTable()
{
    // Naive (not band-limited) shapes; the mipmap build band-limits every level. Each frame peaks near ±1.
    constexpr int N = WavetableBank::kFrameSize;
    std::vector<float> frames(static_cast<std::size_t>(4 * N));
    for (int i = 0; i < N; ++i) {
        const double p = (i + 0.5) / N;   // half-sample offset keeps the saw/square edges symmetric
        frames[static_cast<std::size_t>(i)]         = static_cast<float>(1.0 - 2.0 * p);                          // saw
        frames[static_cast<std::size_t>(N + i)]     = p < 0.5 ? 1.0f : -1.0f;                                     // square
        frames[static_cast<std::size_t>(2 * N + i)] = static_cast<float>(p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p); // triangle
        frames[static_cast<std::size_t>(3 * N + i)] = static_cast<float>(std::sin(2.0 * kPi * i / N));             // sine
    }
    return WavetableBank::build(frames, N, "Basic Shapes");
}

} // namespace winerose::dsp
