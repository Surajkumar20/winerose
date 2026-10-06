#include "engine/midi/NoteProcessor.h"

#include <algorithm>
#include <cmath>

namespace winerose::midi {

void NoteProcessor::prepare(double sampleRate) noexcept
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    reset();
}

void NoteProcessor::reset() noexcept
{
    m_inputMap.fill(-1);
    m_down.fill(false);
    m_heldCount = 0;
    m_keysDown = 0;
    m_latchedRelease = false;
    m_offCount = 0;
    m_clipPlaying = false;
    m_clipKey = -1;
    m_hostPlaying = false;
    m_anchorSample = 0;
    m_anchorBeat = 0.0;
    m_beatsPerSample = m_bpm / 60.0 / m_sampleRate;
    m_nextStep = 0;
    m_arpCounter = 0;
}

std::int64_t NoteProcessor::sampleAt(double beat) const noexcept
{
    return m_anchorSample + static_cast<std::int64_t>(std::llround((beat - m_anchorBeat) / m_beatsPerSample));
}

void NoteProcessor::startFreeClock(std::int64_t now) noexcept
{
    if (m_hostPlaying) return;   // host time rules while the transport runs
    m_anchorSample = now;
    m_anchorBeat = 0.0;
}

void NoteProcessor::setTransport(std::int64_t blockStart, const TransportInfo& t) noexcept
{
    const double bpm = t.bpm > 0.0 ? t.bpm : m_bpm;
    if (t.isPlaying) {
        m_hostPlaying = true;
        m_anchorSample = blockStart;
        m_anchorBeat = t.ppqPosition;
    } else if (m_hostPlaying || bpm != m_bpm) {
        // Transport stopped or tempo changed: continue from the current beat on the internal clock.
        m_anchorBeat = beatAt(blockStart);
        m_anchorSample = blockStart;
        m_hostPlaying = false;
    }
    m_bpm = bpm;
    m_beatsPerSample = m_bpm / 60.0 / m_sampleRate;
}

void NoteProcessor::setSettings(const Settings& s, const std::array<const Clip*, kClipSlots>& clips, NoteOutput& out, std::int64_t now) noexcept
{
    const bool modeChanged = s.arpOn != m_settings.arpOn || s.clipOn != m_settings.clipOn || s.clipTrigger != m_settings.clipTrigger;
    if (modeChanged) {
        stopAll(out);
        m_heldCount = 0;
        m_latchedRelease = false;
        m_clipPlaying = false;
    }
    if (s.arpOn && s.arpStepBeats != m_settings.arpStepBeats && s.arpStepBeats > 0.0)
        m_nextStep = static_cast<std::int64_t>(std::ceil((beatAt(now) - halfSampleBeats()) / s.arpStepBeats));
    if (m_settings.arpLatch && !s.arpLatch && m_keysDown == 0 && m_heldCount > 0) {   // latch released
        m_heldCount = 0;
        m_latchedRelease = false;
        flushOffs(kNever, out);
    }
    m_settings = s;
    m_clips = clips;
}

// --- Output bookkeeping ----------------------------------------------------------------------------------

void NoteProcessor::emitOn(int note, int velocity, std::int64_t offTime, std::int64_t now, NoteOutput& out) noexcept
{
    if (note < 0 || note > 127) return;
    for (int i = 0; i < m_offCount; ++i) {   // retrigger: end the previous instance first
        if (m_offs[static_cast<std::size_t>(i)].note == note) {
            out.noteOff(note);
            m_offs[static_cast<std::size_t>(i)] = m_offs[static_cast<std::size_t>(--m_offCount)];
            break;
        }
    }
    if (m_offCount >= static_cast<int>(m_offs.size())) return;   // full: drop the note rather than hang one
    out.noteOn(note, std::clamp(velocity, 1, 127));
    m_offs[static_cast<std::size_t>(m_offCount++)] = {std::max(offTime, now + 1), note};
}

void NoteProcessor::flushOffs(std::int64_t upTo, NoteOutput& out) noexcept
{
    for (int i = 0; i < m_offCount;) {
        if (m_offs[static_cast<std::size_t>(i)].time <= upTo) {
            out.noteOff(m_offs[static_cast<std::size_t>(i)].note);
            m_offs[static_cast<std::size_t>(i)] = m_offs[static_cast<std::size_t>(--m_offCount)];
        } else {
            ++i;
        }
    }
}

void NoteProcessor::stopAll(NoteOutput& out) noexcept
{
    flushOffs(kNever, out);
}

// --- Input -----------------------------------------------------------------------------------------------

void NoteProcessor::noteOn(int note, int velocity, std::int64_t now, NoteOutput& out) noexcept
{
    if (note < 0 || note > 127) return;
    if (!m_down[static_cast<std::size_t>(note)]) { m_down[static_cast<std::size_t>(note)] = true; ++m_keysDown; }
    const int q = m_settings.scaleOn ? quantize(note, m_settings.key, m_settings.scale, m_settings.transpose) : note;

    if (m_settings.clipOn && m_settings.clipTrigger == ClipTrigger::Note) {
        m_clipKey = q;
        m_clipVelocity = velocity;
        if (!m_clipPlaying) {
            stopAll(out);
            startFreeClock(now);
            m_clipStartBeat = beatAt(now);
            m_clipDone = -2.0 * halfSampleBeats();
            m_clipPlaying = true;   // the first note fires from process(), after every event at this sample
        }
        return;
    }
    if (m_settings.arpOn) {
        if (m_latchedRelease) { m_heldCount = 0; m_latchedRelease = false; }
        bool present = false;
        for (int i = 0; i < m_heldCount; ++i) if (m_held[static_cast<std::size_t>(i)].note == q) present = true;
        const bool wasIdle = m_heldCount == 0;
        if (!present && m_heldCount < static_cast<int>(m_held.size())) m_held[static_cast<std::size_t>(m_heldCount++)] = {q, velocity};
        if (wasIdle) {
            m_arpCounter = 0;
            if (m_hostPlaying) {
                m_nextStep = static_cast<std::int64_t>(std::ceil((beatAt(now) - halfSampleBeats()) / m_settings.arpStepBeats));
            } else {
                startFreeClock(now);
                m_nextStep = 0;
            }
            // The first step fires from process(), so a chord whose notes share a timestamp starts complete.
        }
        return;
    }
    // Pass-through (quantized).
    if (m_inputMap[static_cast<std::size_t>(note)] >= 0) out.noteOff(m_inputMap[static_cast<std::size_t>(note)]);
    m_inputMap[static_cast<std::size_t>(note)] = static_cast<std::int16_t>(q);
    out.noteOn(q, velocity);
}

void NoteProcessor::noteOff(int note, std::int64_t now, NoteOutput& out) noexcept
{
    if (note < 0 || note > 127) return;
    if (m_down[static_cast<std::size_t>(note)]) { m_down[static_cast<std::size_t>(note)] = false; --m_keysDown; }
    // Whatever mode we are in now, a note that went through as itself is released as itself.
    if (m_inputMap[static_cast<std::size_t>(note)] >= 0) {
        out.noteOff(m_inputMap[static_cast<std::size_t>(note)]);
        m_inputMap[static_cast<std::size_t>(note)] = -1;
    }
    const int q = m_settings.scaleOn ? quantize(note, m_settings.key, m_settings.scale, m_settings.transpose) : note;

    if (m_settings.clipOn && m_settings.clipTrigger == ClipTrigger::Note) {
        if (m_keysDown == 0 && m_clipPlaying) {
            m_clipPlaying = false;
            stopAll(out);
        }
        return;
    }
    if (m_settings.arpOn) {
        if (m_settings.arpLatch) {
            if (m_keysDown == 0) m_latchedRelease = true;   // keep playing the chord until a new one starts
            return;
        }
        for (int i = 0; i < m_heldCount; ++i) {
            if (m_held[static_cast<std::size_t>(i)].note == q) {
                for (int j = i; j + 1 < m_heldCount; ++j) m_held[static_cast<std::size_t>(j)] = m_held[static_cast<std::size_t>(j + 1)];
                --m_heldCount;
                break;
            }
        }
        if (m_heldCount == 0) stopAll(out);
    }
    (void)now;
}

void NoteProcessor::allNotesOff(std::int64_t, NoteOutput& out) noexcept
{
    for (int n = 0; n < 128; ++n) {
        if (m_inputMap[static_cast<std::size_t>(n)] >= 0) out.noteOff(m_inputMap[static_cast<std::size_t>(n)]);
        m_inputMap[static_cast<std::size_t>(n)] = -1;
        m_down[static_cast<std::size_t>(n)] = false;
    }
    m_keysDown = 0;
    m_heldCount = 0;
    m_latchedRelease = false;
    m_clipPlaying = false;
    stopAll(out);
}

// --- Arpeggiator -----------------------------------------------------------------------------------------

double NoteProcessor::arpStepBeat(std::int64_t k) const noexcept
{
    const double step = m_settings.arpStepBeats;
    return static_cast<double>(k) * step + ((k & 1) != 0 ? m_settings.arpSwing * step / 3.0 : 0.0);
}

int NoteProcessor::arpSequence(std::array<int, 128>& seq, std::array<int, 128>& vel) const noexcept
{
    std::array<Held, 32> notes {};
    std::copy(m_held.begin(), m_held.begin() + m_heldCount, notes.begin());
    if (m_settings.arpMode != ArpMode::Order)
        std::sort(notes.begin(), notes.begin() + m_heldCount, [](const Held& a, const Held& b) { return a.note < b.note; });
    std::array<int, 128> up {}, upVel {};
    int n = 0;
    for (int o = 0; o < std::clamp(m_settings.arpOctaves, 1, 4); ++o)
        for (int i = 0; i < m_heldCount && n < 128; ++i) {
            const int note = notes[static_cast<std::size_t>(i)].note + 12 * o;
            if (note > 127) continue;
            up[static_cast<std::size_t>(n)] = note;
            upVel[static_cast<std::size_t>(n++)] = notes[static_cast<std::size_t>(i)].velocity;
        }
    int count = 0;
    auto push = [&](int i) { if (count < 128) { seq[static_cast<std::size_t>(count)] = up[static_cast<std::size_t>(i)]; vel[static_cast<std::size_t>(count++)] = upVel[static_cast<std::size_t>(i)]; } };
    switch (m_settings.arpMode) {
        case ArpMode::Up: case ArpMode::Order: case ArpMode::Random: case ArpMode::Chord:
            for (int i = 0; i < n; ++i) push(i);
            break;
        case ArpMode::Down:
            for (int i = n - 1; i >= 0; --i) push(i);
            break;
        case ArpMode::UpDown:
            for (int i = 0; i < n; ++i) push(i);
            for (int i = n - 2; i >= 1; --i) push(i);
            break;
        case ArpMode::DownUp:
            for (int i = n - 1; i >= 0; --i) push(i);
            for (int i = 1; i <= n - 2; ++i) push(i);
            break;
        case ArpMode::Count: break;
    }
    return count;
}

void NoteProcessor::arpStep(std::int64_t now, NoteOutput& out) noexcept
{
    std::array<int, 128> seq {}, vel {};
    const int count = arpSequence(seq, vel);
    const std::int64_t k = m_nextStep;
    const std::uint64_t position = m_arpCounter++;
    if (count == 0) return;

    m_rng ^= m_rng >> 12;
    m_rng ^= m_rng << 25;
    m_rng ^= m_rng >> 27;
    const double r = static_cast<double>((m_rng * 0x2545F4914F6CDD1Dull) >> 11) * (1.0 / 9007199254740992.0);
    if (r >= m_settings.arpChance) return;   // this step rests

    const double stepBeats = m_settings.arpStepBeats;
    const std::int64_t offTime = m_settings.arpGate >= 0.999f
        ? sampleAt(arpStepBeat(k + 1))   // legato: hold until the next step
        : now + std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(m_settings.arpGate * stepBeats / m_beatsPerSample)));

    auto velocityFor = [&](int index) {
        switch (m_settings.arpVelocityMode) {
            case VelocityMode::Fixed: return m_settings.arpVelocity;
            case VelocityMode::Ramp:
                return static_cast<int>(std::lround(m_settings.arpVelocity * (0.4 + 0.6 * (count > 1 ? static_cast<double>(index) / (count - 1) : 1.0))));
            case VelocityMode::AsPlayed: case VelocityMode::Count: break;
        }
        return vel[static_cast<std::size_t>(index)];
    };
    auto outNote = [&](int note) {
        note += m_settings.arpTranspose;
        return m_settings.scaleOn ? quantize(note, m_settings.key, m_settings.scale, 0) : note;
    };

    if (m_settings.arpMode == ArpMode::Chord) {
        for (int i = 0; i < count; ++i) emitOn(outNote(seq[static_cast<std::size_t>(i)]), velocityFor(i), offTime, now, out);
        return;
    }
    int index;
    if (m_settings.arpMode == ArpMode::Random) {
        m_rng ^= m_rng >> 12;
        m_rng ^= m_rng << 25;
        m_rng ^= m_rng >> 27;
        index = static_cast<int>(((m_rng * 0x2545F4914F6CDD1Dull) >> 33) % static_cast<std::uint64_t>(count));
    } else {
        index = static_cast<int>(position % static_cast<std::uint64_t>(count));
    }
    emitOn(outNote(seq[static_cast<std::size_t>(index)]), velocityFor(index), offTime, now, out);
}

// --- Clip sequencer --------------------------------------------------------------------------------------

const Clip* NoteProcessor::currentClip() const noexcept
{
    const Clip* c = m_clips[static_cast<std::size_t>(std::clamp(m_settings.clip, 0, kClipSlots - 1))];
    return (c != nullptr && !c->notes.empty() && c->lengthBeats > 0.0) ? c : nullptr;
}

bool NoteProcessor::clipRunning() const noexcept
{
    return m_settings.clipOn && m_clipPlaying;
}

double NoteProcessor::clipLocalBeat(std::int64_t t) const noexcept
{
    return m_settings.clipTrigger == ClipTrigger::Note ? beatAt(t) - m_clipStartBeat : beatAt(t);
}

namespace {
double swung(double start, float swing) noexcept
{
    // Notes on odd sixteenths are delayed by up to a third of a sixteenth.
    const double sixteenth = start / 0.25;
    const double idx = std::floor(sixteenth + 1e-9);
    if (std::abs(sixteenth - idx) < 1e-6 && (static_cast<long long>(idx) & 1) != 0) return start + swing * 0.25 / 3.0;
    return start;
}
}

void NoteProcessor::clipEmit(double from, double to, std::int64_t now, NoteOutput& out) noexcept
{
    const Clip* clip = currentClip();
    if (clip == nullptr || to <= from) return;
    const double L = clip->lengthBeats;
    const double origin = m_settings.clipTrigger == ClipTrigger::Note ? m_clipStartBeat : 0.0;
    const int transpose = m_settings.clipTrigger == ClipTrigger::Note && m_clipKey >= 0 ? m_clipKey - 60 : 0;
    const auto firstLoop = static_cast<long long>(std::floor(from / L));
    const auto lastLoop = std::min(static_cast<long long>(std::floor(to / L)), firstLoop + 64);
    for (long long m = firstLoop; m <= lastLoop; ++m) {
        for (const auto& n : clip->notes) {
            const double s = static_cast<double>(m) * L + swung(n.start, m_settings.clipSwing);
            if (s <= from || s > to) continue;
            int note = n.note + transpose;
            if (m_settings.scaleOn) note = quantize(note, m_settings.key, m_settings.scale, 0);
            const int velocity = m_settings.clipTrigger == ClipTrigger::Note ? (n.velocity * m_clipVelocity + 63) / 127 : n.velocity;
            emitOn(note, std::max(1, velocity), sampleAt(origin + s + n.length), now, out);
        }
    }
}

double NoteProcessor::clipNextStart(double after) const noexcept
{
    const Clip* clip = currentClip();
    if (clip == nullptr) return 1e300;
    const double L = clip->lengthBeats;
    double best = 1e300;
    const auto loop = static_cast<long long>(std::floor(after / L));
    for (long long m = loop; m <= loop + 1; ++m)
        for (const auto& n : clip->notes) {
            const double s = static_cast<double>(m) * L + swung(n.start, m_settings.clipSwing);
            if (s > after) best = std::min(best, s);
        }
    return best;
}

// --- Scheduling ------------------------------------------------------------------------------------------

void NoteProcessor::process(std::int64_t now, NoteOutput& out) noexcept
{
    flushOffs(now, out);

    if (m_settings.clipOn) {
        if (m_settings.clipTrigger == ClipTrigger::Host) {
            if (m_hostPlaying && !m_clipPlaying) {
                m_clipPlaying = true;
                m_clipDone = beatAt(now) - 2.0 * halfSampleBeats();
            } else if (!m_hostPlaying && m_clipPlaying) {
                m_clipPlaying = false;
                stopAll(out);
            }
        }
        if (clipRunning()) {
            const double local = clipLocalBeat(now) + halfSampleBeats();
            if (local < m_clipDone - 1e-6) {   // host jumped backwards (loop / seek)
                stopAll(out);
                m_clipDone = local - 2.0 * halfSampleBeats();
            }
            clipEmit(m_clipDone, local, now, out);
            m_clipDone = local;
        }
        return;
    }

    if (m_settings.arpOn && arpActive() && m_settings.arpStepBeats > 0.0) {
        const double beat = beatAt(now) + halfSampleBeats();
        const double step = m_settings.arpStepBeats;
        // Host jumps: resynchronise to the grid instead of replaying (or waiting for) many steps.
        if (arpStepBeat(m_nextStep) > beat + 2.0 * step || arpStepBeat(m_nextStep) < beat - 2.0 * step)
            m_nextStep = static_cast<std::int64_t>(std::ceil((beat - 2.0 * halfSampleBeats()) / step));
        for (int guard = 0; guard < 4 && arpActive() && arpStepBeat(m_nextStep) <= beat; ++guard) {
            arpStep(now, out);
            ++m_nextStep;
        }
    }
}

std::int64_t NoteProcessor::nextEvent(std::int64_t now) const noexcept
{
    std::int64_t t = kNever;
    for (int i = 0; i < m_offCount; ++i) t = std::min(t, m_offs[static_cast<std::size_t>(i)].time);
    if (m_settings.clipOn) {
        if (clipRunning()) {
            const double next = clipNextStart(m_clipDone);
            if (next < 1e299) {
                const double origin = m_settings.clipTrigger == ClipTrigger::Note ? m_clipStartBeat : 0.0;
                t = std::min(t, sampleAt(origin + next));
            }
        }
    } else if (m_settings.arpOn && arpActive() && m_settings.arpStepBeats > 0.0) {
        t = std::min(t, sampleAt(arpStepBeat(m_nextStep)));
    }
    return t == kNever ? kNever : std::max(t, now + 1);
}

} // namespace winerose::midi
