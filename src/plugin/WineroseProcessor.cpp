#include "WineroseProcessor.h"

#include "ui/WineroseEditor.h"

#include <algorithm>
#include <cstring>

namespace winerose::plugin {

namespace {

constexpr std::size_t kMaxMidiEventsPerBlock = 4096;

class MidiBufferSink final : public MidiEventSink {
public:
    MidiBufferSink(juce::MidiBuffer& buffer, int offset) : m_buffer(buffer), m_offset(offset) {}
    void push(const MidiEvent& e) noexcept override { m_buffer.addEvent(e.data, e.size, e.sampleOffset + m_offset); }
private:
    juce::MidiBuffer& m_buffer;
    int m_offset;
};

} // namespace

WineroseProcessor::WineroseProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
    , m_config(std::make_shared<ConfigManager>())
    , m_engine(std::make_unique<Engine>(m_config))
    , m_controller(std::make_unique<control::Controller>(*m_engine, m_config))
{
    // The engine has registered every module by now; the host's parameter list is fixed from here on.
    std::vector<RegistryParameter*> exposed;
    for (const auto& [module, registry] : m_config->getAttachedParamRegistries()) {
        for (const auto& def : registry->getAll()) {
            if (!def.automatable || std::holds_alternative<StringMeta>(def.meta)) continue;
            const std::string nsKey = registry->namespacedKey(def.key);
            auto* slot = m_config->hostWritableSlot(nsKey);
            if (slot == nullptr) continue;
            auto param = std::make_unique<RegistryParameter>(m_config, nsKey, module + " " + def.key, def, *slot);
            exposed.push_back(param.get());
            addParameter(param.release());
        }
    }
    m_hostSync = std::make_unique<HostSync>(*this, *m_config, std::move(exposed));
    m_controller->setGestureSink(m_hostSync.get());

    // Where imported Serum presets usually reference their wavetables from (the user's own content folders).
    {
        const auto docs = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        std::vector<std::filesystem::path> roots;
        for (const char* sub : {"Xfer/Serum 2 Presets", "Xfer/Serum Presets", "Winerose/Tables"})
            roots.emplace_back(docs.getChildFile(sub).getFullPathName().toStdString());
        m_controller->setAssetSearchPaths(std::move(roots));
    }
}

WineroseProcessor::~WineroseProcessor()
{
    m_controller->setGestureSink(nullptr);
}

void WineroseProcessor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock)
{
    m_engine->prepare(sampleRate, std::max(1, maximumExpectedSamplesPerBlock));
    m_midiIn.reserve(kMaxMidiEventsPerBlock);
    m_chunkEvents.reserve(kMaxMidiEventsPerBlock);
    m_channelPtrs.assign(static_cast<std::size_t>(std::max(2, getTotalNumOutputChannels())), nullptr);
    setLatencySamples(m_engine->latencySamples());
}

void WineroseProcessor::releaseResources()
{
    m_engine->reset();
}

bool WineroseProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    return layouts.getMainInputChannelSet().isDisabled();
}

void WineroseProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples  = buffer.getNumSamples();
    const int numChannels = std::min(buffer.getNumChannels(), static_cast<int>(m_channelPtrs.size()));
    const int maxBlock    = m_engine->maxBlockSize();

    if (maxBlock <= 0 || numChannels <= 0) {   // called before prepareToPlay: stay silent
        buffer.clear();
        midi.clear();
        return;
    }

    // The on-screen keyboard's notes join the host's.
    m_keyboardState.processNextMidiBuffer(midi, 0, numSamples, true);

    // JUCE MidiBuffer → POD events (capacity reserved in prepareToPlay; overflow is dropped, never allocated).
    m_midiIn.clear();
    for (const auto meta : midi) {
        if (meta.numBytes < 1 || meta.numBytes > 3) continue;   // SysEx is not carried
        if (m_midiIn.size() == m_midiIn.capacity()) break;
        MidiEvent e;
        e.sampleOffset = meta.samplePosition;
        e.size = static_cast<std::uint8_t>(meta.numBytes);
        std::memcpy(e.data, meta.data, static_cast<std::size_t>(meta.numBytes));
        m_midiIn.push_back(e);
    }
    midi.clear();

    TransportInfo transport;
    if (auto* hostPlayHead = getPlayHead()) {
        if (const auto pos = hostPlayHead->getPosition()) {
            if (const auto bpm = pos->getBpm())         transport.bpm = *bpm;
            if (const auto ppq = pos->getPpqPosition()) transport.ppqPosition = *ppq;
            transport.isPlaying = pos->getIsPlaying();
        }
    }

    // Hosts (FL Studio especially) may send blocks larger than announced, or as small as 1 sample:
    // feed the engine in chunks of at most maxBlock.
    float* const* channels = buffer.getArrayOfWritePointers();
    std::size_t nextEvent = 0;
    for (int start = 0; start < numSamples; start += maxBlock) {
        const int n = std::min(maxBlock, numSamples - start);
        m_chunkEvents.clear();
        while (nextEvent < m_midiIn.size() && m_midiIn[nextEvent].sampleOffset < start + n) {
            MidiEvent e = m_midiIn[nextEvent++];
            e.sampleOffset = std::max(0, e.sampleOffset - start);
            m_chunkEvents.push_back(e);
        }
        for (int ch = 0; ch < numChannels; ++ch)
            m_channelPtrs[static_cast<std::size_t>(ch)] = channels[ch] + start;
        m_engine->process(m_channelPtrs.data(), numChannels, n,
                          m_chunkEvents.data(), static_cast<int>(m_chunkEvents.size()), transport);
        // Arp / clip output of this chunk (the engine keeps one chunk's worth), at its place in the block.
        MidiBufferSink sink(midi, start);
        m_engine->drainMidiOut(sink);
        transport.ppqPosition += transport.bpm / 60.0 * n / getSampleRate();
    }
    for (int ch = numChannels; ch < buffer.getNumChannels(); ++ch) buffer.clear(ch, 0, numSamples);

}

juce::AudioProcessorEditor* WineroseProcessor::createEditor()
{
    // The one line to change when the UI is replaced (e.g. a WebBrowserComponent editor) — PLAN.md §1.4.
    return new ui::WineroseEditor(*this, *m_controller, &m_keyboardState);
}

void WineroseProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    const std::string state = m_controller->saveState();
    destData.replaceAll(state.data(), state.size());
}

void WineroseProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0) return;
    const std::string state(static_cast<const char*>(data), static_cast<std::size_t>(sizeInBytes));
    m_controller->loadState(state);   // invalid blobs are rejected without touching the current patch
}

} // namespace winerose::plugin

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new winerose::plugin::WineroseProcessor();
}
