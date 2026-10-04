#pragma once

#include "engine/voice/Voice.h"

#include <cstdint>
#include <vector>

namespace winerose::voice {

/**
 * @class VoiceManager
 * @brief Fixed 64-voice pool (SPEC §5.3) with polyphony limit, stealing and sustain pedal. Realtime-safe.
 *
 * Stealing order when the polyphony limit is reached: released voices first (oldest first), then the
 * oldest held voice. "Limit same note" is off, matching Serum 2's default.
 * Random start phases / detunes come from a generator re-seeded in prepare(), so renders are deterministic.
 */
class VoiceManager {
public:
    static constexpr int kMaxVoices = 64;

    VoiceManager() : m_voices(kMaxVoices) {}   // heap pool: a Voice carries its modulation state (~10 KB)

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    void noteOn(int note, int velocity, int polyphony, const ControlContext& ctx) noexcept;
    void noteOff(int note) noexcept;
    void setSustainPedal(bool down) noexcept;
    void allNotesOff() noexcept;   // release everything (CC 123)
    void allSoundOff() noexcept;   // silence immediately (CC 120)

    void control(const ControlContext& ctx) noexcept;
    void render(float* mainL, float* mainR, float* directL, float* directR, int numSamples,
                const VoiceTables& tables) noexcept;

    int activeCount() const noexcept;

private:
    std::vector<Voice> m_voices;   // sized once in the constructor, never resized
    std::uint64_t m_order = 0;
    Rng m_rng;
    bool m_sustainPedal = false;
};

} // namespace winerose::voice
