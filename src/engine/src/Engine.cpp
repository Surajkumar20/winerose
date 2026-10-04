#include "engine/Engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace winerose {

Engine::Engine(std::shared_ptr<ConfigManager> config)
    : m_config(std::move(config))
{
    m_global = std::make_unique<ParamRegistry>(m_config, global_keys::module);
    m_global->registerFloat(global_keys::masterVolume, 0.75f, 0.0f, 1.0f,
                            "Master", "Output level (linear gain)");
    m_global->registerEnum<Quality>(global_keys::quality,
                                    { {Quality::Good, "Good"}, {Quality::High, "High"}, {Quality::Ultra, "Ultra"} },
                                    Quality::Good, "Master",
                                    "Oversampling around nonlinear stages: Good=1x, High=2x, Ultra=4x (INFERRED)");
    m_masterVolume = m_global->handle(global_keys::masterVolume);
    publishSnapshot();
}

Engine::~Engine() = default;

void Engine::prepare(double sampleRate, int maxBlockSize)
{
    m_sampleRate   = sampleRate;
    m_maxBlockSize = maxBlockSize;
    m_volumeSmoother.prepare(sampleRate, 0.01);
    m_volumeSmoother.reset(m_masterVolume.load());
    m_snapshots.collectGarbage();
}

void Engine::process(float* const* out, int numChannels, int numSamples,
                     const MidiEvent* /*events*/, int /*numEvents*/,
                     const TransportInfo& /*transport*/) noexcept
{
    realtime::Scope rt;
    [[maybe_unused]] const EngineSnapshot* snapshot = m_snapshots.acquire();

    for (int ch = 0; ch < numChannels; ++ch)
        std::memset(out[ch], 0, sizeof(float) * static_cast<std::size_t>(numSamples));

    // Voices render here in feature/audio_engine. The master stage is already real.
    m_volumeSmoother.setTarget(m_masterVolume.load());
    float peakL = 0.0f, peakR = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
        const float g = m_volumeSmoother.next();
        for (int ch = 0; ch < numChannels; ++ch) {
            out[ch][i] *= g;
            const float a = std::abs(out[ch][i]);
            if (ch == 0) peakL = std::max(peakL, a);
            else         peakR = std::max(peakR, a);
        }
    }
    if (numChannels == 1) peakR = peakL;
    m_meters.peakLeft.store(peakL, std::memory_order_relaxed);
    m_meters.peakRight.store(peakR, std::memory_order_relaxed);
}

void Engine::drainMidiOut(MidiEventSink& /*sink*/) noexcept
{
    // Arp / clip sequencer output lands here in feature/audio_engine (SPEC §1.7).
}

void Engine::reset() noexcept
{
    m_volumeSmoother.reset(m_masterVolume.load());
    m_meters.peakLeft.store(0.0f);
    m_meters.peakRight.store(0.0f);
}

void Engine::publishSnapshot()
{
    auto snapshot = std::make_unique<EngineSnapshot>();
    snapshot->revision = ++m_snapshotRevision;
    m_snapshots.publish(std::move(snapshot));
}

} // namespace winerose
