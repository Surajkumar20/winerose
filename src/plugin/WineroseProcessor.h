#pragma once

#include "HostSync.h"
#include "RegistryParameter.h"

#include "control/Controller.h"
#include "engine/Engine.h"
#include "params/ConfigManager.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <vector>

namespace winerose::plugin {

/**
 * @class WineroseProcessor
 * @brief The JUCE host adapter (PLAN.md §3.4). Owns one Engine + its ConfigManager + a Controller, and
 *        translates between JUCE (buffers, MidiBuffer, parameters, state chunks) and the JUCE-free layers.
 *        Contains no DSP and no parameter logic of its own.
 */
class WineroseProcessor final : public juce::AudioProcessor {
public:
    WineroseProcessor();
    ~WineroseProcessor() override;

    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool   acceptsMidi() const override { return true; }
    bool   producesMidi() const override { return true; }
    bool   isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int  getNumPrograms() override { return 1; }
    int  getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    control::IController& controller() { return *m_controller; }

private:
    // Declaration order = construction order; destruction runs in reverse (HostSync first).
    std::shared_ptr<ConfigManager>       m_config;
    std::unique_ptr<Engine>              m_engine;
    std::unique_ptr<control::Controller> m_controller;
    juce::MidiKeyboardState m_keyboardState;   // the editor's on-screen keyboard
    std::unique_ptr<HostSync>            m_hostSync;

    // Preallocated in prepareToPlay(): processBlock never allocates.
    std::vector<MidiEvent> m_midiIn;
    std::vector<MidiEvent> m_chunkEvents;
    std::vector<float*>    m_channelPtrs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WineroseProcessor)
};

} // namespace winerose::plugin
