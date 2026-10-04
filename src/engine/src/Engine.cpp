#include "engine/Engine.h"

#include "engine/Denormals.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace winerose {

namespace {

// Fixed LFO shapes as one band-limited bank: frames in LfoShape order Sine, Triangle, Saw Up, Saw Down,
// Square. Stored bipolar (-1..1); the LFO maps to 0..1. DC is kept (shapes are symmetric anyway).
std::shared_ptr<const dsp::WavetableBank> makeLfoShapes()
{
    constexpr int N = dsp::WavetableBank::kFrameSize;
    constexpr double kPi = 3.14159265358979323846;
    std::vector<float> frames(static_cast<std::size_t>(5 * N));
    for (int i = 0; i < N; ++i) {
        const double p = (i + 0.5) / N;
        frames[static_cast<std::size_t>(i)]         = static_cast<float>(std::sin(2.0 * kPi * i / N));
        frames[static_cast<std::size_t>(N + i)]     = static_cast<float>(p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p);
        frames[static_cast<std::size_t>(2 * N + i)] = static_cast<float>(2.0 * p - 1.0);
        frames[static_cast<std::size_t>(3 * N + i)] = static_cast<float>(1.0 - 2.0 * p);
        frames[static_cast<std::size_t>(4 * N + i)] = p < 0.5 ? 1.0f : -1.0f;
    }
    return dsp::WavetableBank::build(frames, N, "LFO Shapes", /*removeDc=*/false);
}

std::shared_ptr<const dsp::WavetableBank> makeLfoPath(const dsp::Curve& curve)
{
    constexpr int N = dsp::WavetableBank::kFrameSize;
    std::vector<float> frame(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i)
        frame[static_cast<std::size_t>(i)] = static_cast<float>(2.0 * curve.evaluate(static_cast<double>(i) / N) - 1.0);
    return dsp::WavetableBank::build(frame, N, "LFO Path", /*removeDc=*/false);
}

} // namespace

Engine::Engine(std::shared_ptr<ConfigManager> config)
    : m_config(std::move(config))
    , m_global(std::make_unique<ParamRegistry>(m_config, global_keys::module))
    , m_modules(std::make_unique<modules::EngineModules>(m_config))
{
    m_global->registerFloat(global_keys::masterVolume, 0.75f, 0.0f, 1.0f,
                            "Master", "Output level (linear gain)");
    m_global->registerEnum<Quality>(global_keys::quality,
                                    { {Quality::Good, "Good"}, {Quality::High, "High"}, {Quality::Ultra, "Ultra"} },
                                    Quality::Good, "Master",
                                    "Oversampling around nonlinear stages: Good=1x, High=2x, Ultra=4x (INFERRED)");
    m_global->registerInt(global_keys::polyphony, 16, 1, voice::VoiceManager::kMaxVoices, "Master",
                          "Maximum simultaneous notes");
    m_global->registerInt(global_keys::bendUp, 2, 0, 48, "Master", "Pitch-bend range up", ParamOpts{.unit = "st"});
    m_global->registerInt(global_keys::bendDown, 2, 0, 48, "Master", "Pitch-bend range down", ParamOpts{.unit = "st"});
    m_masterVolume = m_global->handle(global_keys::masterVolume);
    m_polyphony    = m_global->handle(global_keys::polyphony);
    m_quality      = m_global->handle(global_keys::quality);
    m_bendUp       = m_global->handle(global_keys::bendUp);
    m_bendDown     = m_global->handle(global_keys::bendDown);

    m_noSlots.fill(-1);
    const auto basic = dsp::makeBasicShapesTable();
    m_oscTables   = {basic, basic, basic};
    m_subTable    = dsp::makeSubShapesTable();
    m_noiseTables = dsp::NoiseTables::make();
    m_lfoShapes   = makeLfoShapes();
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
    m_pitchBendRaw = 0.0f;
    m_context = voice::ControlContext{};
    m_context.sampleRate = sampleRate;
    adoptSnapshot();   // safe: the audio thread isn't running during prepare()
    controlTick();
    m_snapshots.collectGarbage();
}

void Engine::controlTick() noexcept
{
    const auto& mods = *m_modules;
    mods.targets.readBase(m_base.data());
    mods.matrix->read(m_slots);

    bool warpWantsOversampling = false;
    for (int o = 0; o < voice::kOscCount; ++o) {
        const auto v = mods.osc[static_cast<std::size_t>(o)]->read(m_base.data());
        if (v.enabled && (dsp::warpNeedsOversampling(v.warp1) || dsp::warpNeedsOversampling(v.warp2)))
            warpWantsOversampling = true;
    }
    // Quality: Good = 1x, High = 2x, Ultra = 4x, applied only while a warp needs it (Serum 1 behaviour:
    // "oversampling applied only to warps", SPEC §1.1). Factors INFERRED.
    static constexpr int kFactor[] = {1, 2, 4};
    const int quality = std::clamp(static_cast<int>(std::lround(m_quality.load())), 0, 2);

    auto& ctx = m_context;
    ctx.base       = m_base.data();
    ctx.modules    = m_modules.get();
    ctx.slots      = &m_slots;
    ctx.sampleRate = m_sampleRate;
    ctx.oversample = warpWantsOversampling ? kFactor[quality] : 1;
    ctx.global.pitchBend = 0.5f + 0.5f * m_pitchBendRaw;
    ctx.global.bendSemis = m_pitchBendRaw >= 0.0f ? m_pitchBendRaw * m_bendUp.load() : m_pitchBendRaw * m_bendDown.load();

    m_polyphonyLimit = static_cast<int>(std::lround(m_polyphony.load()));
    m_masterGain.setTarget(m_masterVolume.load());
    m_voices.control(ctx);
}

void Engine::adoptSnapshot() noexcept
{
    // Everything voices read from the snapshot is re-pointed as soon as it is adopted: once acquire()
    // swaps snapshots, the previous one may be freed by the message thread at any moment.
    m_current = m_snapshots.acquire();
    auto& ctx = m_context;
    ctx.slotDest = m_current != nullptr ? &m_current->slotDest : &m_noSlots;
    ctx.tables = voice::VoiceTables{};
    if (m_current == nullptr) return;
    for (int o = 0; o < voice::kOscCount; ++o) {
        ctx.tables.osc[static_cast<std::size_t>(o)]   = m_current->oscTables[static_cast<std::size_t>(o)].get();
        ctx.tables.remap[static_cast<std::size_t>(o)] = m_current->remapCurves[static_cast<std::size_t>(o)].get();
    }
    ctx.tables.sub       = m_current->subTable.get();
    ctx.tables.noise     = m_current->noiseTables.get();
    ctx.tables.lfoShapes = m_current->lfoShapes.get();
    for (int l = 0; l < modulation::kLfoCount; ++l)
        ctx.tables.lfoPaths[static_cast<std::size_t>(l)] = m_current->lfoPaths[static_cast<std::size_t>(l)].get();
}

void Engine::advanceFreeLfos(int numSamples) noexcept
{
    // Free-running LFOs share one phase per LFO across voices; voices pick it up at note-on.
    for (int l = 0; l < modulation::kLfoCount; ++l) {
        const auto s = m_modules->lfo[static_cast<std::size_t>(l)]->read(m_base.data());
        auto& p = m_context.lfoFreePhase[static_cast<std::size_t>(l)];
        p += s.frequency(m_context.global.bpm) / m_sampleRate * numSamples;
        p -= std::floor(p);
    }
}

void Engine::handleMidi(const MidiEvent& e) noexcept
{
    if (e.size < 1) return;
    const std::uint8_t status = e.data[0] & 0xF0;
    const int d1 = e.size > 1 ? e.data[1] : 0;
    const int d2 = e.size > 2 ? e.data[2] : 0;
    switch (status) {
        case 0x90:
            if (d2 > 0) m_voices.noteOn(d1, d2, m_polyphonyLimit, m_context);
            else        m_voices.noteOff(d1);
            break;
        case 0x80:
            m_voices.noteOff(d1);
            break;
        case 0xB0:
            if (d1 == 1)        m_context.global.modWheel = static_cast<float>(d2) / 127.0f;
            else if (d1 == 64)  m_voices.setSustainPedal(d2 >= 64);
            else if (d1 == 120) m_voices.allSoundOff();
            else if (d1 == 123) m_voices.allNotesOff();
            break;
        case 0xD0:
            m_context.global.aftertouch = static_cast<float>(d1) / 127.0f;
            break;
        case 0xE0: {
            const int value = (d2 << 7) | d1;   // 0..16383, centre 8192
            m_pitchBendRaw = std::clamp(static_cast<float>(value - 8192) / 8191.0f, -1.0f, 1.0f);
            m_context.global.pitchBend = 0.5f + 0.5f * m_pitchBendRaw;   // takes effect at the next tick
            break;
        }
        default:
            break;
    }
}

void Engine::process(float* const* out, int numChannels, int numSamples,
                     const MidiEvent* events, int numEvents,
                     const TransportInfo& transport) noexcept
{
    realtime::Scope rt;
    ScopedFlushDenormals ftz;
    adoptSnapshot();
    if (transport.bpm > 0.0) m_context.global.bpm = transport.bpm;
    const voice::VoiceTables& tables = m_context.tables;

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
        m_voices.render(left, right, n, tables);
        advanceFreeLfos(n);

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

void Engine::setOscillatorTable(int index, std::shared_ptr<const dsp::WavetableBank> table)
{
    if (index < 0 || index >= voice::kOscCount || table == nullptr) return;
    m_oscTables[static_cast<std::size_t>(index)] = std::move(table);
    publishSnapshot();
}

void Engine::publishSnapshot()
{
    const auto& mods = *m_modules;
    auto snapshot = std::make_unique<EngineSnapshot>();
    snapshot->revision    = ++m_snapshotRevision;
    snapshot->oscTables   = m_oscTables;
    snapshot->subTable    = m_subTable;
    snapshot->noiseTables = m_noiseTables;
    snapshot->lfoShapes   = m_lfoShapes;

    // Drawable curves → tables, rebuilt only when their text changes.
    for (int l = 0; l < modulation::kLfoCount; ++l) {
        const std::string text = mods.lfo[static_cast<std::size_t>(l)]->path();
        if (!m_lfoPaths[static_cast<std::size_t>(l)] || text != m_lfoPathText[static_cast<std::size_t>(l)]) {
            m_lfoPathText[static_cast<std::size_t>(l)] = text;
            m_lfoPaths[static_cast<std::size_t>(l)] = makeLfoPath(dsp::Curve::parse(text, dsp::Curve::triangle()));
        }
    }
    for (int o = 0; o < voice::kOscCount; ++o) {
        const std::string text = mods.osc[static_cast<std::size_t>(o)]->remapCurve();
        if (!m_remapCurves[static_cast<std::size_t>(o)] || text != m_remapText[static_cast<std::size_t>(o)]) {
            m_remapText[static_cast<std::size_t>(o)] = text;
            m_remapCurves[static_cast<std::size_t>(o)] = std::make_shared<const dsp::CurveTable>(
                dsp::CurveTable::from(dsp::Curve::parse(text, dsp::Curve::identity())));
        }
    }
    snapshot->lfoPaths    = m_lfoPaths;
    snapshot->remapCurves = m_remapCurves;

    // Matrix destinations: namespaced key → target index; anything unknown or non-modulatable is ignored.
    for (int k = 0; k < modulation::kSlotCount; ++k) {
        const int index = mods.targets.find(mods.matrix->destination(k));
        snapshot->slotDest[static_cast<std::size_t>(k)] =
            (index >= 0 && mods.targets.at(index).modulatable) ? static_cast<std::int16_t>(index) : std::int16_t{-1};
    }
    m_snapshots.publish(std::move(snapshot));
}

} // namespace winerose
