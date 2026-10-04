#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace winerose::dsp {

/**
 * @brief Looping noise sources for the noise oscillator (Oscillator3).
 *
 * Serum's noise oscillator plays looping noise samples. Winerose generates its own (original content):
 * white, pink (Paul Kellet's filter) and brown (leaky integrator), each kLength samples, RMS-normalized
 * so the types sound equally loud, plus a wrap guard for interpolation. Deterministic: the same build always
 * produces the same tables. User noise samples (SPEC §2.1 embeddedNoiseData) arrive with feature/presets.
 */
class NoiseTables {
public:
    static constexpr int kLength = 1 << 17;   // ≈ 2.7 s at 48 kHz
    enum class Type : int { White = 0, Pink, Brown, Count };
    static constexpr const char* kTypeNames[] = {"White", "Pink", "Brown"};

    static std::shared_ptr<const NoiseTables> make();

    /** Linear-interpolated read at a position in [0, kLength). */
    float read(Type type, double position) const noexcept
    {
        const auto& t = m_tables[static_cast<std::size_t>(type)];
        const int   i = static_cast<int>(position);
        const float f = static_cast<float>(position - i);
        return t[static_cast<std::size_t>(i)] + (t[static_cast<std::size_t>(i) + 1] - t[static_cast<std::size_t>(i)]) * f;
    }

private:
    NoiseTables() = default;
    std::array<std::vector<float>, static_cast<std::size_t>(Type::Count)> m_tables;   // kLength + 1 each
};

} // namespace winerose::dsp
