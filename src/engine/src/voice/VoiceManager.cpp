#include "engine/voice/VoiceManager.h"

#include <algorithm>

namespace winerose::voice {

void VoiceManager::prepare(double sampleRate) noexcept
{
    Voice::initDownsamplerCoefs();
    for (auto& v : m_voices) v.prepare(sampleRate);
    reset();
}

void VoiceManager::reset() noexcept
{
    for (auto& v : m_voices) v.kill();
    m_order = 0;
    m_rng = Rng{};
    m_sustainPedal = false;
}

void VoiceManager::noteOn(int note, int velocity, int polyphony, const ControlContext& ctx) noexcept
{
    polyphony = std::clamp(polyphony, 1, kMaxVoices);
    const std::uint64_t order = ++m_order;

    int active = 0;
    for (const auto& v : m_voices) active += v.active() ? 1 : 0;

    if (active < polyphony) {
        for (auto& v : m_voices) {
            if (!v.active()) {
                v.start(note, velocity, order, ctx, m_rng);
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
    if (victim != nullptr) victim->steal(note, velocity, order, ctx);
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

void VoiceManager::control(const ControlContext& ctx) noexcept
{
    for (auto& v : m_voices)
        if (v.active()) v.control(ctx);
}

void VoiceManager::render(const VoiceOutputs& out, int numSamples, const VoiceTables& tables) noexcept
{
    for (auto& v : m_voices)
        if (v.active()) v.render(out, numSamples, tables);
}

int VoiceManager::activeCount() const noexcept
{
    int n = 0;
    for (const auto& v : m_voices) n += v.active() ? 1 : 0;
    return n;
}

} // namespace winerose::voice
