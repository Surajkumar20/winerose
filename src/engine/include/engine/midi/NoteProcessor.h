#pragma once

#include "engine/EngineTypes.h"
#include "engine/midi/Midi.h"

#include <array>
#include <cstdint>
#include <limits>

namespace winerose::midi {

/** Receives the processor's output notes (the Engine forwards them to the voices and to MIDI out). */
class NoteOutput {
public:
    virtual ~NoteOutput() = default;
    virtual void noteOn(int note, int velocity) noexcept = 0;
    virtual void noteOff(int note) noexcept = 0;
};

/**
 * @class NoteProcessor
 * @brief Key/scale quantizer → (clip sequencer | arpeggiator | pass-through) → voices. Realtime, no allocation.
 *
 * Time is the engine's ABSOLUTE sample clock. Musical time comes from a beat anchor: the host's ppq while
 * its transport plays, otherwise an internal clock started by the first note. Generated events are scheduled
 * at exact samples; nextEvent() tells the Engine where to split its render chunk, so timing is sample-accurate
 * and independent of the host block size.
 */
class NoteProcessor {
public:
    static constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::max();

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    /** Once per control tick (clips: the current snapshot's parsed clips; null = empty slot). */
    void setSettings(const Settings& s, const std::array<const Clip*, kClipSlots>& clips, NoteOutput& out, std::int64_t now) noexcept;

    /** Once per host block, before anything else. */
    void setTransport(std::int64_t blockStart, const TransportInfo& transport) noexcept;

    void noteOn(int note, int velocity, std::int64_t now, NoteOutput& out) noexcept;
    void noteOff(int note, std::int64_t now, NoteOutput& out) noexcept;
    void allNotesOff(std::int64_t now, NoteOutput& out) noexcept;

    /** Emit everything due at or before `now`. */
    void process(std::int64_t now, NoteOutput& out) noexcept;

    /** Absolute sample of the next generated event after `now`, or kNever. */
    std::int64_t nextEvent(std::int64_t now) const noexcept;

    bool generating() const noexcept { return m_settings.arpOn || m_settings.clipOn; }

private:
    struct Held { int note; int velocity; };
    struct PendingOff { std::int64_t time; int note; };

    double beatAt(std::int64_t t) const noexcept { return m_anchorBeat + static_cast<double>(t - m_anchorSample) * m_beatsPerSample; }
    std::int64_t sampleAt(double beat) const noexcept;
    double halfSampleBeats() const noexcept { return 0.5 * m_beatsPerSample; }

    void emitOn(int note, int velocity, std::int64_t offTime, std::int64_t now, NoteOutput& out) noexcept;
    void flushOffs(std::int64_t upTo, NoteOutput& out) noexcept;
    void stopAll(NoteOutput& out) noexcept;
    void startFreeClock(std::int64_t now) noexcept;

    // Arp
    bool   arpActive() const noexcept { return m_heldCount > 0; }
    double arpStepBeat(std::int64_t k) const noexcept;
    void   arpStep(std::int64_t now, NoteOutput& out) noexcept;
    int    arpSequence(std::array<int, 128>& seq, std::array<int, 128>& vel) const noexcept;

    // Clip
    const Clip* currentClip() const noexcept;
    bool   clipRunning() const noexcept;
    double clipLocalBeat(std::int64_t t) const noexcept;
    void   clipEmit(double fromLocal, double toLocal, std::int64_t now, NoteOutput& out) noexcept;
    double clipNextStart(double afterLocal) const noexcept;

    double   m_sampleRate = 48000.0;
    Settings m_settings {};
    std::array<const Clip*, kClipSlots> m_clips {};

    // Musical time
    double       m_bpm = 120.0;
    double       m_beatsPerSample = 120.0 / 60.0 / 48000.0;
    bool         m_hostPlaying = false;
    std::int64_t m_anchorSample = 0;
    double       m_anchorBeat = 0.0;

    // Input / quantizer
    std::array<std::int16_t, 128> m_inputMap {};   // input note → output note (pass-through), -1 = none
    std::array<bool, 128> m_down {};               // keys currently held
    std::array<Held, 32> m_held {};
    int  m_heldCount = 0;
    bool m_latchedRelease = false;   // latch on and every key is up: the next press starts a new chord
    int  m_keysDown = 0;

    // Generated notes
    std::array<PendingOff, 128> m_offs {};
    int m_offCount = 0;

    // Arp state
    std::int64_t m_nextStep = 0;     // index of the next step to play
    std::uint64_t m_arpCounter = 0;  // steps played since the arp started
    std::uint64_t m_rng = 0x2545F4914F6CDD1Dull;

    // Clip state
    int    m_clipKey = -1, m_clipVelocity = 100;
    double m_clipStartBeat = 0.0;    // beat at which local clip time is 0 (note mode)
    double m_clipDone = 0.0;         // local beat up to which notes were emitted
    bool   m_clipPlaying = false;
};

} // namespace winerose::midi
