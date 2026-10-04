#include "engine/dsp/NoiseTable.h"

#include <cmath>

namespace winerose::dsp {

namespace {

void normalizeRms(std::vector<float>& x, float targetRms)
{
    double sum = 0.0, mean = 0.0;
    for (float v : x) mean += v;
    mean /= static_cast<double>(x.size());
    for (float& v : x) { v = static_cast<float>(v - mean); sum += static_cast<double>(v) * v; }
    const double rms = std::sqrt(sum / static_cast<double>(x.size()));
    const float g = rms > 0.0 ? static_cast<float>(targetRms / rms) : 0.0f;
    for (float& v : x) v *= g;
}

} // namespace

std::shared_ptr<const NoiseTables> NoiseTables::make()
{
    std::shared_ptr<NoiseTables> n(new NoiseTables());
    std::uint32_t seed = 0x12345678u;
    auto white = [&seed] {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return static_cast<float>(seed) / 2147483648.0f - 1.0f;   // [-1, 1)
    };

    std::vector<float> w(kLength), p(kLength), b(kLength);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, brown = 0;
    // Run the filters over the table twice so the second pass starts from steady state and loops cleanly.
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < kLength; ++i) {
            const double x = white();
            b0 = 0.99886 * b0 + x * 0.0555179;  b1 = 0.99332 * b1 + x * 0.0750759;
            b2 = 0.96900 * b2 + x * 0.1538520;  b3 = 0.86650 * b3 + x * 0.3104856;
            b4 = 0.55000 * b4 + x * 0.5329522;  b5 = -0.7616 * b5 - x * 0.0168980;
            const double pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + x * 0.5362;
            b6 = x * 0.115926;
            brown = 0.998 * brown + x * 0.02;
            w[static_cast<std::size_t>(i)] = static_cast<float>(x);
            p[static_cast<std::size_t>(i)] = static_cast<float>(pink);
            b[static_cast<std::size_t>(i)] = static_cast<float>(brown);
        }
    }

    // Remove the end-to-start jump (matters for brown noise's random walk) so the loop point doesn't click.
    for (auto* t : {&p, &b}) {
        const double jump = static_cast<double>(t->back()) - (*t)[0];
        for (int i = 0; i < kLength; ++i)
            (*t)[static_cast<std::size_t>(i)] -= static_cast<float>(jump * i / (kLength - 1));
    }

    constexpr float kRms = 0.3f;   // peaks stay near ±1 for white noise
    for (auto* t : {&w, &p, &b}) {
        normalizeRms(*t, kRms);
        t->push_back((*t)[0]);   // wrap guard for interpolation
    }
    n->m_tables[0] = std::move(w);
    n->m_tables[1] = std::move(p);
    n->m_tables[2] = std::move(b);
    return n;
}

} // namespace winerose::dsp
