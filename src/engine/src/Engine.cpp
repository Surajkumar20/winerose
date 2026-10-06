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

    for (int r = 0; r < fx::kRackCount; ++r)
        for (int s = 0; s < fx::kSlotsPerRack; ++s)
            m_fxSlots[static_cast<std::size_t>(r * fx::kSlotsPerRack + s)] = std::make_unique<modules::FxSlotModule>(m_config, r, s);
    m_mixer = std::make_unique<modules::MixerModule>(m_config);
    m_midi = std::make_unique<modules::MidiModules>(m_config);
    m_config->addListener(this);
    syncFx();

    m_noSlots.fill(-1);
    const auto basic = dsp::makeBasicShapesTable();
    m_oscTables   = {basic, basic, basic};
    m_subTable    = dsp::makeSubShapesTable();
    m_noiseTables = dsp::NoiseTables::make();
    m_lfoShapes   = makeLfoShapes();
    m_spectralFft = std::make_unique<dsp::RealFft>(dsp::SpectralData::kFft);
    publishSnapshot();
}

Engine::~Engine()
{
    m_config->removeListener(this);
}

void Engine::syncFx()
{
    for (int r = 0; r < fx::kRackCount; ++r) {
        auto& rack = m_racks[static_cast<std::size_t>(r)];
        for (int s = 0; s < fx::kSlotsPerRack; ++s) {
            const fx::FxType type = m_fxSlots[static_cast<std::size_t>(r * fx::kSlotsPerRack + s)]->type();
            if (type != rack.publishedType(s)) rack.setSlotType(s, type, m_sampleRate);
        }
        rack.collectGarbage();
    }
}

void Engine::onParamChanged(const std::string& key)
{
    // Only slot types are structural; everything else is read through handles at control rate.
    if (key.rfind("FXRack", 0) == 0 && key.size() > 5 && key.compare(key.size() - 5, 5, ".type") == 0) syncFx();
}

void Engine::onBatchEnd()
{
    syncFx();
}

void Engine::prepare(double sampleRate, int maxBlockSize)
{
    m_sampleRate   = sampleRate;
    m_maxBlockSize = maxBlockSize;
    m_masterGain.setRampSamples(voice::kControlBlock);
    m_masterGain.reset(m_masterVolume.load());
    m_voices.prepare(sampleRate);
    m_notes.prepare(sampleRate);
    m_sampleClock = 0;
    m_pitchBendRaw = 0.0f;
    m_context = voice::ControlContext{};
    m_context.sampleRate = sampleRate;
    for (auto& rack : m_racks) rack.prepare(sampleRate);   // re-prepares effects for this rate
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
        if (v.enabled && v.type == modules::OscType::Wavetable
            && (dsp::warpNeedsOversampling(v.warp1) || dsp::warpNeedsOversampling(v.warp2)))
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

    const fx::FxContext fxContext {ctx.global.bpm};
    for (int r = 0; r < fx::kRackCount; ++r) {
        auto& params = m_rackParams[static_cast<std::size_t>(r)];
        for (int s = 0; s < fx::kSlotsPerRack; ++s)
            params[static_cast<std::size_t>(s)] = m_fxSlots[static_cast<std::size_t>(r * fx::kSlotsPerRack + s)]->read();
        m_racks[static_cast<std::size_t>(r)].setParams(params, fxContext);
        m_rackActive[static_cast<std::size_t>(r)] = m_racks[static_cast<std::size_t>(r)].active();
    }
    m_mixLevels = m_mixer->read();

    std::array<const midi::Clip*, midi::kClipSlots> clips {};
    if (m_current != nullptr)
        for (int c = 0; c < midi::kClipSlots; ++c) clips[static_cast<std::size_t>(c)] = m_current->clips[static_cast<std::size_t>(c)].get();
    m_notes.setSettings(m_midi->read(), clips, m_noteSink, static_cast<std::int64_t>(m_sampleClock));
}

void Engine::NoteSink::noteOn(int note, int velocity) noexcept
{
    m_engine.m_voices.noteOn(note, velocity, m_engine.m_polyphonyLimit, m_engine.m_context);
    if (m_engine.m_notes.generating()) m_engine.pushMidiOut(0x90, note, velocity);
}

void Engine::NoteSink::noteOff(int note) noexcept
{
    m_engine.m_voices.noteOff(note);
    if (m_engine.m_notes.generating()) m_engine.pushMidiOut(0x80, note, 0);
}

void Engine::pushMidiOut(std::uint8_t status, int d1, int d2) noexcept
{
    if (m_midiOutCount >= static_cast<int>(m_midiOut.size())) return;
    MidiEvent& e = m_midiOut[static_cast<std::size_t>(m_midiOutCount++)];
    e.sampleOffset = m_blockPos;
    e.data[0] = status;
    e.data[1] = static_cast<std::uint8_t>(std::clamp(d1, 0, 127));
    e.data[2] = static_cast<std::uint8_t>(std::clamp(d2, 0, 127));
    e.size = 3;
}

void Engine::adoptSnapshot() noexcept
{
    // Everything voices read from the snapshot is re-pointed as soon as it is adopted: once acquire()
    // swaps snapshots, the previous one may be freed by the message thread at any moment.
    m_current = m_snapshots.acquire();
    for (auto& rack : m_racks) rack.beginBlock();
    auto& ctx = m_context;
    ctx.slotDest = m_current != nullptr ? &m_current->slotDest : &m_noSlots;
    ctx.tables = voice::VoiceTables{};
    ctx.tables.fft = m_spectralFft.get();
    ctx.tables.roundRobin = m_roundRobin.data();
    if (m_current == nullptr) return;
    for (int o = 0; o < voice::kOscCount; ++o) {
        const auto i = static_cast<std::size_t>(o);
        ctx.tables.osc[i]      = m_current->oscTables[i].get();
        ctx.tables.remap[i]    = m_current->remapCurves[i].get();
        ctx.tables.sample[i]   = m_current->oscSamples[i].get();
        ctx.tables.spectral[i] = m_current->oscSpectral[i].get();
        ctx.tables.multi[i]    = m_current->oscMulti[i].get();
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
            if (d2 > 0) m_notes.noteOn(d1, d2, static_cast<std::int64_t>(m_sampleClock), m_noteSink);
            else        m_notes.noteOff(d1, static_cast<std::int64_t>(m_sampleClock), m_noteSink);
            break;
        case 0x80:
            m_notes.noteOff(d1, static_cast<std::int64_t>(m_sampleClock), m_noteSink);
            break;
        case 0xB0:
            if (d1 == 1)        m_context.global.modWheel = static_cast<float>(d2) / 127.0f;
            else if (d1 == 64)  m_voices.setSustainPedal(d2 >= 64);
            else if (d1 == 120) m_voices.allSoundOff();
            else if (d1 == 123) { m_notes.allNotesOff(static_cast<std::int64_t>(m_sampleClock), m_noteSink); m_voices.allNotesOff(); }
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
    m_notes.setTransport(static_cast<std::int64_t>(m_sampleClock), transport);
    m_midiOutCount = 0;
    const voice::VoiceTables& tables = m_context.tables;

    float peakL = 0.0f, peakR = 0.0f;
    float left[voice::kControlBlock], right[voice::kControlBlock];
    float directL[voice::kControlBlock], directR[voice::kControlBlock];
    float bus1L[voice::kControlBlock], bus1R[voice::kControlBlock];
    float bus2L[voice::kControlBlock], bus2R[voice::kControlBlock];
    const voice::VoiceOutputs outputs {left, right, directL, directR, bus1L, bus1R, bus2L, bus2R};
    int pos = 0, ev = 0;

    while (pos < numSamples || ev < numEvents) {
        if (m_sampleClock % voice::kControlBlock == 0 && pos < numSamples) controlTick();
        m_blockPos = std::min(pos, std::max(0, numSamples - 1));
        while (ev < numEvents && events[ev].sampleOffset <= pos) handleMidi(events[ev++]);
        if (pos >= numSamples) break;   // trailing events at/after the block end were handled above
        m_notes.process(static_cast<std::int64_t>(m_sampleClock), m_noteSink);   // arp / clip events due now

        const int toBoundary = voice::kControlBlock - static_cast<int>(m_sampleClock % voice::kControlBlock);
        int end = std::min(numSamples, pos + toBoundary);
        if (ev < numEvents) end = std::min(end, std::max(pos + 1, events[ev].sampleOffset));
        const std::int64_t next = m_notes.nextEvent(static_cast<std::int64_t>(m_sampleClock));
        if (next != midi::NoteProcessor::kNever)
            end = static_cast<int>(std::min<std::int64_t>(end, pos + std::max<std::int64_t>(1, next - static_cast<std::int64_t>(m_sampleClock))));
        const int n = end - pos;

        const std::size_t bytes = sizeof(float) * static_cast<std::size_t>(n);
        for (float* b : {left, right, directL, directR, bus1L, bus1R, bus2L, bus2R}) std::memset(b, 0, bytes);
        m_voices.render(outputs, n, tables);

        // FX racks on Main and the two buses (in that order), then the mix: Main + Bus 1/2 + Direct.
        if (m_rackActive[0]) m_racks[0].process(left, right, n);
        if (m_rackActive[1]) m_racks[1].process(bus1L, bus1R, n);
        if (m_rackActive[2]) m_racks[2].process(bus2L, bus2R, n);
        for (int i = 0; i < n; ++i) {
            left[i]  += bus1L[i] * m_mixLevels.bus1 + bus2L[i] * m_mixLevels.bus2 + directL[i] * m_mixLevels.direct;
            right[i] += bus1R[i] * m_mixLevels.bus1 + bus2R[i] * m_mixLevels.bus2 + directR[i] * m_mixLevels.direct;
        }
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

void Engine::drainMidiOut(MidiEventSink& sink) noexcept
{
    // Arp / clip output generated by the last process() call (SPEC §1.7), in time order.
    for (int i = 0; i < m_midiOutCount; ++i) sink.push(m_midiOut[static_cast<std::size_t>(i)]);
    m_midiOutCount = 0;
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

void Engine::setOscillatorSample(int index, std::shared_ptr<const dsp::SampleData> sample)
{
    if (index < 0 || index >= voice::kOscCount) return;
    m_oscSpectral[static_cast<std::size_t>(index)] = sample != nullptr ? dsp::SpectralData::analyze(*sample) : nullptr;
    m_oscSamples[static_cast<std::size_t>(index)] = std::move(sample);
    publishSnapshot();
}

void Engine::setOscillatorMultisample(int index, std::shared_ptr<const dsp::Multisample> instrument)
{
    if (index < 0 || index >= voice::kOscCount) return;
    m_oscMulti[static_cast<std::size_t>(index)] = std::move(instrument);
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
    snapshot->oscSamples  = m_oscSamples;
    snapshot->oscSpectral = m_oscSpectral;
    snapshot->oscMulti    = m_oscMulti;

    // Clips: re-parsed only when their text or length changes.
    for (int c = 0; c < midi::kClipSlots; ++c) {
        const auto i = static_cast<std::size_t>(c);
        const std::string text = m_midi->clipText(c);
        const double length = m_midi->clipLength(c);
        if (!m_clipObjects[i] || text != m_clipText[i] || length != m_clipLength[i]) {
            m_clipText[i] = text;
            m_clipLength[i] = length;
            m_clipObjects[i] = std::make_shared<const midi::Clip>(midi::Clip::parse(text, length));
        }
    }
    snapshot->clips = m_clipObjects;

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
