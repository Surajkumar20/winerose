#pragma once

#include <cstddef>

namespace winerose::dsp {

/**
 * @brief Real FFT of fixed size N over pffft (vendored, BSD-like). Non-realtime use only: wavetable
 *        mipmap builds, analysis in tests and tools.
 *
 * Spectrum layout ("ordered"): s[0] = DC (real), s[1] = Nyquist (real), then s[2k], s[2k+1] = re, im
 * of bin k for k = 1 .. N/2-1. Transforms are unnormalized forward; inverse() scales by 1/N so
 * inverse(forward(x)) == x.
 *
 * N must be a multiple of 32 whose only prime factors are 2, 3, 5 (pffft's constraint).
 */
class RealFft {
public:
    explicit RealFft(int size);
    ~RealFft();

    RealFft(const RealFft&) = delete;
    RealFft& operator=(const RealFft&) = delete;

    int size() const noexcept { return m_size; }

    void forward(const float* input, float* spectrum);
    void inverse(const float* spectrum, float* output);

private:
    int    m_size;
    void*  m_setup;    // PFFFT_Setup*
    float* m_bufA;     // aligned scratch (pffft wants 16-byte alignment)
    float* m_bufB;
};

} // namespace winerose::dsp
