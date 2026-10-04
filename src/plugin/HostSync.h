#pragma once

#include "RegistryParameter.h"

#include "control/IController.h"
#include "params/ConfigManager.h"
#include "params/ParamListener.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace winerose::plugin {

/**
 * @class HostSync
 * @brief Keeps the host and ConfigManager in agreement, in both directions (PLAN.md §3.6).
 *
 *  ConfigManager → host: a change made through the control layer (UI, undo, preset load) is pushed with
 *                        setValueNotifyingHost(); a batch end pushes everything + updateHostDisplay().
 *  host → ConfigManager: automation lands in the slot on the audio thread (RegistryParameter::setValue);
 *                        a 30 Hz message-thread timer turns those flags into notifyChanged() so the
 *                        control layer and UIs see them. Those are NOT echoed back to the host.
 *  Gestures:             IGestureSink → begin/endChangeGesture on the matching parameter.
 */
class HostSync final : public IParamListener, public control::IGestureSink, private juce::Timer {
public:
    HostSync(juce::AudioProcessor& processor, ConfigManager& config, std::vector<RegistryParameter*> params);
    ~HostSync() override;

    HostSync(const HostSync&) = delete;
    HostSync& operator=(const HostSync&) = delete;

private:
    // IParamListener
    void onParamChanged(const std::string& nsKey) override;
    void onBatchEnd() override;

    // control::IGestureSink
    void beginGesture(const std::string& nsKey) override;
    void endGesture(const std::string& nsKey) override;

    void timerCallback() override;
    static void pushToHost(RegistryParameter& param);

    juce::AudioProcessor&                                m_processor;
    ConfigManager&                                       m_config;
    std::vector<RegistryParameter*>                      m_params;
    std::unordered_map<std::string, RegistryParameter*>  m_byKey;
    bool                                                 m_applyingHostChanges = false;
};

} // namespace winerose::plugin
