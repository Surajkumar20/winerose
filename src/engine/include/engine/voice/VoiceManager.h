#pragma once

#include "engine/voice/Voice.h"

#include <array>
#include <cstdint>

namespace winerose::voice {

/**
 * @class VoiceManager
 * @brief Fixed 64-voice pool (SPEC §5.3) with polyphony limit, stealing and sustain pedal. Realtime-safe.
 *
 * Stealing order when the polyphony limit is reached: released voices first (oldest first), then the
 * oldest held voice. "Limit same note" is off, matching Serum 2's default.
 * Start phases come from phase + random·U[0,1) using a seeded generator, so renders are deterministic.
 */
class VoiceManager {
public:
    static constexpr int kMaxVoices = 64;

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    void noteOn(int note, int polyphony, const VoiceControl& control) noexcept;
    void noteOff(int note) noexcept;
    void setSustainPedal(bool down) noexcept;
    void allNotesOff() noexcept;   // release everything (CC 123)
    void allSoundOff() noexcept;   // silence immediately (CC 120)

    void control(const VoiceControl& control) noexcept;
    void render(float* left, float* right, int numSamples, const dsp::WavetableBank* bank) noexcept;

    int activeCount() const noexcept;

private:
    double nextRandom() noexcept;   // xorshift64*, [0,1)

    std::array<Voice, kMaxVoices> m_voices;
    std::uint64_t m_order = 0;
    std::uint64_t m_rng = 0x9E3779B97F4A7C15ull;
    bool m_sustainPedal = false;
};

} // namespace winerose::voice
