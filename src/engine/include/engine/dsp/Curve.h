#pragma once

#include <array>
#include <string>
#include <vector>

namespace winerose::dsp {

/**
 * @brief Drawable breakpoint curve on [0,1] → [0,1] (SPEC §1.6: LFO paths; §1.2: warp Remap curves).
 *
 * Stored as a string parameter so it lives in the patch state without being host-automatable:
 *     "x,y,c;x,y,c;..."   points sorted by x, x from 0 to 1; c in [-1,1] bends the segment that STARTS at
 *                         the point (0 = straight, using the envelope curve shape with exponent 10·c).
 * Unparseable strings fall back to the curve's default. Rendering produces a lookup table that can be
 * sampled on the audio thread (or fed to WavetableBank to get a band-limited LFO shape).
 */
class Curve {
public:
    struct Point {
        double x, y, c;
    };

    static Curve parse(const std::string& text, const Curve& fallback);
    std::string serialize() const;

    static Curve identity();   // "0,0,0;1,1,0"
    static Curve triangle();   // "0,0,0;0.5,1,0;1,0,0" — the default LFO path

    /** Value at x in [0,1]. */
    double evaluate(double x) const noexcept;

    /** n+1 samples of evaluate(i/n), i = 0..n (the extra sample lets callers interpolate up to x = 1). */
    std::vector<float> render(int n) const;

    const std::vector<Point>& points() const noexcept { return m_points; }

private:
    std::vector<Point> m_points;
};

/** A rendered curve sampled with linear interpolation; used for warp remaps on the audio thread. */
struct CurveTable {
    static constexpr int kSize = 2048;
    std::array<float, kSize + 1> values {};

    static CurveTable from(const Curve& curve);

    float lookup(double x) const noexcept
    {
        const double pos = (x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x)) * kSize;
        const int    i   = pos >= kSize ? kSize - 1 : static_cast<int>(pos);
        const float  f   = static_cast<float>(pos - i);
        return values[static_cast<std::size_t>(i)] + (values[static_cast<std::size_t>(i) + 1] - values[static_cast<std::size_t>(i)]) * f;
    }
};

} // namespace winerose::dsp
