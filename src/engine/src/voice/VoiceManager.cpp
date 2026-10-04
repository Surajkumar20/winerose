#include "engine/voice/VoiceManager.h"

#include <algorithm>

namespace winerose::voice {

void VoiceManager::prepare(double sampleRate) noexcept
{
    for (auto& v : m_voices) v.prepare(sampleRate);
    reset();
}

void VoiceManager::reset() noexcept
{
    for (auto& v : m_voices) v.kill();
    m_order = 0;
    m_rng = 0x9E3779B97F4A7C15ull;
    m_sustainPedal = false;
}

double VoiceManager::nextRandom() noexcept
{
    m_rng ^= m_rng >> 12;
    m_rng ^= m_rng << 25;
    m_rng ^= m_rng >> 27;
    const std::uint64_t r = m_rng * 0x2545F4914F6CDD1Dull;
    return static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0);   // 53 bits → [0,1)
}

void VoiceManager::noteOn(int note, int polyphony, const VoiceControl& control) noexcept
{
    polyphony = std::clamp(polyphony, 1, kMaxVoices);
    const double startPhase = control.osc.phase + control.osc.random * nextRandom();
    const std::uint64_t order = ++m_order;

    int active = 0;
    for (const auto& v : m_voices) active += v.active() ? 1 : 0;

    if (active < polyphony) {
        for (auto& v : m_voices) {
            if (!v.active()) {
                v.start(note, order, startPhase, control);
                return;
            }
        }
    }

    // Steal: oldest released voice first, else the oldest voice.
    Voice* victim = nullptr;
    for (auto& v : m_voices) {
        if (!v.active() || !v.released()) continue;
        if (victim == nullptr || v.order() < victim->order()) victim = &v;
    }
    if (victim == nullptr) {
        for (auto& v : m_voices) {
            if (!v.active()) continue;
            if (victim == nullptr || v.order() < victim->order()) victim = &v;
        }
    }
    if (victim != nullptr) victim->steal(note, order, control);
}

void VoiceManager::noteOff(int note) noexcept
{
    for (auto& v : m_voices) {
        if (!v.active() || v.released() || v.note() != note) continue;
        if (m_sustainPedal) v.setSustained(true);
        else                v.release();
    }
}

void VoiceManager::setSustainPedal(bool down) noexcept
{
    m_sustainPedal = down;
    if (down) return;
    for (auto& v : m_voices) {
        if (v.active() && v.sustained()) {
            v.setSustained(false);
            v.release();
        }
    }
}

void VoiceManager::allNotesOff() noexcept
{
    m_sustainPedal = false;
    for (auto& v : m_voices) {
        v.setSustained(false);
        if (v.active()) v.release();
    }
}

void VoiceManager::allSoundOff() noexcept
{
    m_sustainPedal = false;
    for (auto& v : m_voices) {
        v.setSustained(false);
        v.kill();
    }
}

void VoiceManager::control(const VoiceControl& control) noexcept
{
    for (auto& v : m_voices)
        if (v.active()) v.control(control);
}

void VoiceManager::render(float* left, float* right, int numSamples, const dsp::WavetableBank* bank) noexcept
{
    for (auto& v : m_voices)
        if (v.active()) v.render(left, right, numSamples, bank);
}

int VoiceManager::activeCount() const noexcept
{
    int n = 0;
    for (const auto& v : m_voices) n += v.active() ? 1 : 0;
    return n;
}

} // namespace winerose::voice
