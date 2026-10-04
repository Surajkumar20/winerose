#include "engine/dsp/Curve.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace winerose::dsp {

namespace {

// Same family as the envelope curve: y = (exp(k·x) - 1) / (exp(k) - 1), k = 10·c.
double bend(double x, double c) noexcept
{
    const double k = 10.0 * c;
    if (std::abs(k) < 1e-6) return x;
    return (std::exp(k * x) - 1.0) / (std::exp(k) - 1.0);
}

bool parseNumber(const std::string& s, double& out)
{
    const char* begin = s.c_str();
    char* end = nullptr;
    out = std::strtod(begin, &end);
    return end != begin && *end == '\0' && std::isfinite(out);
}

} // namespace

Curve Curve::identity()
{
    Curve c;
    c.m_points = {{0.0, 0.0, 0.0}, {1.0, 1.0, 0.0}};
    return c;
}

Curve Curve::triangle()
{
    Curve c;
    c.m_points = {{0.0, 0.0, 0.0}, {0.5, 1.0, 0.0}, {1.0, 0.0, 0.0}};
    return c;
}

Curve Curve::parse(const std::string& text, const Curve& fallback)
{
    Curve c;
    std::stringstream points(text);
    std::string point;
    while (std::getline(points, point, ';')) {
        if (point.find_first_not_of(" \t") == std::string::npos) continue;
        std::stringstream fields(point);
        std::string fx, fy, fc;
        double x = 0, y = 0, k = 0;
        if (!std::getline(fields, fx, ',') || !std::getline(fields, fy, ',')) return fallback;
        if (!parseNumber(fx, x) || !parseNumber(fy, y)) return fallback;
        if (std::getline(fields, fc, ',') && !parseNumber(fc, k)) return fallback;
        c.m_points.push_back({std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0), std::clamp(k, -1.0, 1.0)});
    }
    if (c.m_points.size() < 2) return fallback;
    std::stable_sort(c.m_points.begin(), c.m_points.end(), [](const Point& a, const Point& b) { return a.x < b.x; });
    if (c.m_points.front().x != 0.0 || c.m_points.back().x != 1.0) return fallback;
    return c;
}

std::string Curve::serialize() const
{
    std::ostringstream out;
    for (std::size_t i = 0; i < m_points.size(); ++i) {
        if (i) out << ';';
        out << m_points[i].x << ',' << m_points[i].y << ',' << m_points[i].c;
    }
    return out.str();
}

double Curve::evaluate(double x) const noexcept
{
    if (m_points.empty()) return x;
    x = std::clamp(x, 0.0, 1.0);
    for (std::size_t i = 1; i < m_points.size(); ++i) {
        const Point& a = m_points[i - 1];
        const Point& b = m_points[i];
        if (x <= b.x) {
            const double span = b.x - a.x;
            if (span <= 0.0) return b.y;   // vertical step
            return a.y + (b.y - a.y) * bend((x - a.x) / span, a.c);
        }
    }
    return m_points.back().y;
}

std::vector<float> Curve::render(int n) const
{
    std::vector<float> out(static_cast<std::size_t>(n) + 1);
    for (int i = 0; i <= n; ++i) out[static_cast<std::size_t>(i)] = static_cast<float>(evaluate(static_cast<double>(i) / n));
    return out;
}

CurveTable CurveTable::from(const Curve& curve)
{
    CurveTable t;
    const auto v = curve.render(kSize);
    std::copy(v.begin(), v.end(), t.values.begin());
    return t;
}

} // namespace winerose::dsp
