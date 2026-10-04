#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace winerose::fx::dsp {

inline constexpr double kPi = 3.14159265358979323846;

// --- Parameter mapping (generic slot params are 0..1) ---------------------------------------------------
inline float lin(float p, float lo, float hi) noexcept { return lo + (hi - lo) * std::clamp(p, 0.0f, 1.0f); }
inline float expo(float p, float lo, float hi) noexcept { return lo * std::pow(hi / lo, std::clamp(p, 0.0f, 1.0f)); }
inline float dbToGain(float db) noexcept { return std::pow(10.0f, db / 20.0f); }
inline float gainToDb(float g) noexcept { return 20.0f * std::log10(std::max(g, 1e-12f)); }
inline int   choice(float p, int count) noexcept { return std::clamp(static_cast<int>(p * count), 0, count - 1); }
/** Bipolar dB knob: p = 0.5 is exactly 0 dB. */
inline float bipolarDb(float p, float range) noexcept
{
    const float db = (std::clamp(p, 0.0f, 1.0f) - 0.5f) * 2.0f * range;
    return std::abs(db) < 1e-6f ? 0.0f : db;
}

// --- RBJ biquad (Bristow-Johnson cookbook), transposed direct form II ---------------------------------
struct Biquad {
    enum class Kind { Lowpass, Highpass, Bandpass, Peak, LowShelf, HighShelf, Allpass };
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    void set(Kind kind, double freq, double q, double gainDb, double sr) noexcept
    {
        const double w0 = 2.0 * kPi * std::clamp(freq, 1.0, 0.49 * sr) / sr;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * std::max(q, 0.01));
        const double A = std::pow(10.0, gainDb / 40.0);
        double B0 = 1, B1 = 0, B2 = 0, A0 = 1, A1 = 0, A2 = 0;
        switch (kind) {
            case Kind::Lowpass:  B0 = (1 - cw) / 2; B1 = 1 - cw; B2 = (1 - cw) / 2; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
            case Kind::Highpass: B0 = (1 + cw) / 2; B1 = -(1 + cw); B2 = (1 + cw) / 2; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
            case Kind::Bandpass: B0 = alpha; B1 = 0; B2 = -alpha; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
            case Kind::Allpass:  B0 = 1 - alpha; B1 = -2 * cw; B2 = 1 + alpha; A0 = 1 + alpha; A1 = -2 * cw; A2 = 1 - alpha; break;
            case Kind::Peak:
                B0 = 1 + alpha * A; B1 = -2 * cw; B2 = 1 - alpha * A; A0 = 1 + alpha / A; A1 = -2 * cw; A2 = 1 - alpha / A; break;
            case Kind::LowShelf: {
                const double s = 2 * std::sqrt(A) * alpha;
                B0 = A * ((A + 1) - (A - 1) * cw + s); B1 = 2 * A * ((A - 1) - (A + 1) * cw); B2 = A * ((A + 1) - (A - 1) * cw - s);
                A0 = (A + 1) + (A - 1) * cw + s; A1 = -2 * ((A - 1) + (A + 1) * cw); A2 = (A + 1) + (A - 1) * cw - s; break;
            }
            case Kind::HighShelf: {
                const double s = 2 * std::sqrt(A) * alpha;
                B0 = A * ((A + 1) + (A - 1) * cw + s); B1 = -2 * A * ((A - 1) + (A + 1) * cw); B2 = A * ((A + 1) + (A - 1) * cw - s);
                A0 = (A + 1) - (A - 1) * cw + s; A1 = 2 * ((A - 1) - (A + 1) * cw); A2 = (A + 1) - (A - 1) * cw - s; break;
            }
        }
        b0 = static_cast<float>(B0 / A0); b1 = static_cast<float>(B1 / A0); b2 = static_cast<float>(B2 / A0);
        a1 = static_cast<float>(A1 / A0); a2 = static_cast<float>(A2 / A0);
    }

    float process(float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void reset() noexcept { z1 = z2 = 0; }
};

// --- One-pole (TPT) ------------------------------------------------------------------------------------
struct OnePole {
    float G = 1.0f, s = 0.0f;
    void setCutoff(double hz, double sr) noexcept
    {
        const double g = std::tan(kPi * std::clamp(hz, 1.0, 0.49 * sr) / sr);
        G = static_cast<float>(g / (1.0 + g));
    }
    float lowpass(float x) noexcept { const float v = (x - s) * G; const float y = v + s; s = y + v; return y; }
    float highpass(float x) noexcept { return x - lowpass(x); }
    void reset() noexcept { s = 0.0f; }
};

// --- Fractional delay line (power-of-two ring, 4-point Hermite read) ----------------------------------
class DelayLine {
public:
    void allocate(int maxDelaySamples)
    {
        int size = 16;
        while (size < maxDelaySamples + 8) size <<= 1;
        m_buffer.assign(static_cast<std::size_t>(size), 0.0f);
        m_mask = size - 1;
        m_write = 0;
    }
    void reset() noexcept { std::fill(m_buffer.begin(), m_buffer.end(), 0.0f); m_write = 0; }
    int  capacity() const noexcept { return m_mask - 7; }

    void write(float x) noexcept
    {
        m_buffer[static_cast<std::size_t>(m_write)] = x;
        m_write = (m_write + 1) & m_mask;
    }

    /** Sample written `delay` samples ago (delay >= 1 for a read after write). */
    float read(float delay) const noexcept
    {
        const float pos = static_cast<float>(m_write) - std::clamp(delay, 1.0f, static_cast<float>(capacity()));
        const float fl = std::floor(pos);
        const float t = pos - fl;
        const int i = static_cast<int>(fl);
        const float xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    float readInt(int delay) const noexcept { return at(m_write - std::clamp(delay, 1, capacity())); }

private:
    float at(int i) const noexcept { return m_buffer[static_cast<std::size_t>(i & m_mask)]; }
    std::vector<float> m_buffer;
    int m_mask = 0, m_write = 0;
};

// --- Linkwitz-Riley 4th-order crossover (two cascaded Butterworth biquads per side) --------------------
// low + high sums to an allpass (flat magnitude), the standard property splitters rely on.
struct Lr4 {
    Biquad lp1, lp2, hp1, hp2;
    void set(double freq, double sr) noexcept
    {
        constexpr double q = 0.70710678118654752;
        lp1.set(Biquad::Kind::Lowpass, freq, q, 0, sr);
        lp2 = lp1;
        hp1.set(Biquad::Kind::Highpass, freq, q, 0, sr);
        hp2 = hp1;
        lp2.reset(); hp2.reset();
    }
    void setKeepState(double freq, double sr) noexcept
    {
        const Lr4 saved = *this;
        set(freq, sr);
        lp1.z1 = saved.lp1.z1; lp1.z2 = saved.lp1.z2; lp2.z1 = saved.lp2.z1; lp2.z2 = saved.lp2.z2;
        hp1.z1 = saved.hp1.z1; hp1.z2 = saved.hp1.z2; hp2.z1 = saved.hp2.z1; hp2.z2 = saved.hp2.z2;
    }
    void split(float x, float& low, float& high) noexcept
    {
        low = lp2.process(lp1.process(x));
        high = hp2.process(hp1.process(x));
    }
    /** The allpass equal to low + high (for phase-aligning a band that skipped this crossover). */
    float allpass(float x) noexcept
    {
        float l, h;
        split(x, l, h);
        return l + h;
    }
    void reset() noexcept { lp1.reset(); lp2.reset(); hp1.reset(); hp2.reset(); }
};

/** Linear ramp of a block-rate value across the samples of one process() call. */
struct Ramp {
    float value = 0.0f, step = 0.0f;
    void set(float target, int n) noexcept { step = n > 0 ? (target - value) / static_cast<float>(n) : 0.0f; if (n <= 0) value = target; }
    void snap(float v) noexcept { value = v; step = 0.0f; }
    float next() noexcept { value += step; return value; }
};

} // namespace winerose::fx::dsp
