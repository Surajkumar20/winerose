#include "engine/Engine.h"

#include "engine/Denormals.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace winerose {

Engine::Engine(std::shared_ptr<ConfigManager> config)
    : m_config(std::move(config))
    , m_global(std::make_unique<ParamRegistry>(m_config, global_keys::module))
    , m_osc0(m_config, 0)
    , m_filter0(m_config, 0)
    , m_env0(m_config, 0)
{
    m_global->registerFloat(global_keys::masterVolume, 0.75f, 0.0f, 1.0f,
                            "Master", "Output level (linear gain)");
    m_global->registerEnum<Quality>(global_keys::quality,
                                    { {Quality::Good, "Good"}, {Quality::High, "High"}, {Quality::Ultra, "Ultra"} },
                                    Quality::Good, "Master",
                                    "Oversampling around nonlinear stages: Good=1x, High=2x, Ultra=4x (INFERRED)");
    m_global->registerInt(global_keys::polyphony, 16, 1, voice::VoiceManager::kMaxVoices, "Master",
                          "Maximum simultaneous notes");
    m_masterVolume = m_global->handle(global_keys::masterVolume);
    m_polyphony    = m_global->handle(global_keys::polyphony);

    m_oscTable = dsp::makeBasicShapesTable();
    publishSnapshot();
}

Engine::~Engine() = default;

void Engine::prepare(double sampleRate, int maxBlockSize)
{
    m_sampleRate   = sampleRate;
    m_maxBlockSize = maxBlockSize;
    m_masterGain.setRampSamples(voice::kControlBlock);
    m_masterGain.reset(m_masterVolume.load());
    m_voices.prepare(sampleRate);
    m_sampleClock = 0;
    m_control.sampleRate = sampleRate;
    controlTick();
    m_snapshots.collectGarbage();
}

void Engine::controlTick() noexcept
{
    m_control.osc    = m_osc0.read();
    m_control.filter = m_filter0.read();
    m_control.env    = m_env0.read();
    m_control.filterCoefs = dsp::Svf::compute(m_control.filter.cutoffHz, m_control.filter.resonance, m_sampleRate);
    m_polyphonyLimit = static_cast<int>(std::lround(m_polyphony.load()));
    m_masterGain.setTarget(m_masterVolume.load());
    m_voices.control(m_control);
}

void Engine::handleMidi(const MidiEvent& e) noexcept
{
    if (e.size < 1) return;
    const std::uint8_t status = e.data[0] & 0xF0;
    const int d1 = e.size > 1 ? e.data[1] : 0;
    const int d2 = e.size > 2 ? e.data[2] : 0;
    switch (status) {
        case 0x90:
            if (d2 > 0) m_voices.noteOn(d1, m_polyphonyLimit, m_control);
            else        m_voices.noteOff(d1);
            break;
        case 0x80:
            m_voices.noteOff(d1);
            break;
        case 0xB0:
            if (d1 == 64)       m_voices.setSustainPedal(d2 >= 64);
            else if (d1 == 120) m_voices.allSoundOff();
            else if (d1 == 123) m_voices.allNotesOff();
            break;
        default:
            break;   // pitch bend, aftertouch, CCs → modulation sources in Phase 3
    }
}

void Engine::process(float* const* out, int numChannels, int numSamples,
                     const MidiEvent* events, int numEvents,
                     const TransportInfo& /*transport*/) noexcept
{
    realtime::Scope rt;
    ScopedFlushDenormals ftz;
    const EngineSnapshot* snapshot = m_snapshots.acquire();
    const dsp::WavetableBank* table = snapshot != nullptr ? snapshot->oscTable.get() : nullptr;

    float peakL = 0.0f, peakR = 0.0f;
    float left[voice::kControlBlock], right[voice::kControlBlock];
    int pos = 0, ev = 0;

    while (pos < numSamples || ev < numEvents) {
        if (m_sampleClock % voice::kControlBlock == 0 && pos < numSamples) controlTick();
        while (ev < numEvents && events[ev].sampleOffset <= pos) handleMidi(events[ev++]);
        if (pos >= numSamples) break;   // trailing events at/after the block end were handled above

        const int toBoundary = voice::kControlBlock - static_cast<int>(m_sampleClock % voice::kControlBlock);
        int end = std::min(numSamples, pos + toBoundary);
        if (ev < numEvents) end = std::min(end, std::max(pos + 1, events[ev].sampleOffset));
        const int n = end - pos;

        std::memset(left, 0, sizeof(float) * static_cast<std::size_t>(n));
        std::memset(right, 0, sizeof(float) * static_cast<std::size_t>(n));
        m_voices.render(left, right, n, table);

        for (int i = 0; i < n; ++i) {
            const float g = m_masterGain.next();
            const float l = left[i] * g, r = right[i] * g;
            peakL = std::max(peakL, std::abs(l));
            peakR = std::max(peakR, std::abs(r));
            if (numChannels >= 2) {
                out[0][pos + i] = l;
                out[1][pos + i] = r;
            } else if (numChannels == 1) {
                out[0][pos + i] = 0.5f * (l + r);
            }
        }
        for (int ch = 2; ch < numChannels; ++ch)
            std::memset(out[ch] + pos, 0, sizeof(float) * static_cast<std::size_t>(n));

        pos = end;
        m_sampleClock += static_cast<std::uint64_t>(n);
    }

    m_meters.peakLeft.store(peakL, std::memory_order_relaxed);
    m_meters.peakRight.store(numChannels == 1 ? peakL : peakR, std::memory_order_relaxed);
}

void Engine::drainMidiOut(MidiEventSink& /*sink*/) noexcept
{
    // Arp / clip sequencer output lands here (SPEC §1.7, Phase 7).
}

void Engine::reset() noexcept
{
    m_voices.reset();
    m_masterGain.reset(m_masterVolume.load());
    m_meters.peakLeft.store(0.0f);
    m_meters.peakRight.store(0.0f);
}

void Engine::setOscillatorTable(std::shared_ptr<const dsp::WavetableBank> table)
{
    m_oscTable = std::move(table);
    publishSnapshot();
}

void Engine::publishSnapshot()
{
    auto snapshot = std::make_unique<EngineSnapshot>();
    snapshot->revision = ++m_snapshotRevision;
    snapshot->oscTable = m_oscTable;
    m_snapshots.publish(std::move(snapshot));
}

} // namespace winerose
