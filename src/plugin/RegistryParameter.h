#pragma once

#include "params/ConfigManager.h"
#include "params/ParamTypes.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <string>

namespace winerose::plugin {

/**
 * @class RegistryParameter
 * @brief Exposes one registry parameter to the host. The only JUCE ↔ params seam (PLAN.md §3.4).
 *
 * Holds a copy of the ParamDef's meta and a reference to the parameter's ConfigManager slot, so every
 * host call is lock-free (getValue/setValue may arrive on the audio thread). Normalization uses the
 * same pure functions (winerose::toNormalized/fromNormalized) as every other layer.
 *
 * Host writes store straight into the slot and raise a flag; HostSync later tells ConfigManager on the
 * message thread so the control layer and UIs catch up.
 */
class RegistryParameter final : public juce::AudioProcessorParameterWithID {
public:
    RegistryParameter(std::shared_ptr<ConfigManager> config, const std::string& nsKey,
                      const std::string& displayName, const ParamDef& def, std::atomic<float>& slot);

    float getValue() const override;
    void  setValue(float newValue) override;
    float getDefaultValue() const override { return m_defaultNormalized; }

    juce::String getText(float normalizedValue, int maximumStringLength) const override;
    float        getValueForText(const juce::String& text) const override;
    juce::String getLabel() const override { return m_unit; }

    int  getNumSteps() const override;
    bool isDiscrete() const override { return m_discrete; }
    bool isBoolean() const override { return m_boolean; }
    juce::StringArray getAllValueStrings() const override { return m_valueStrings; }

    const std::string& nsKey() const noexcept { return m_nsKey; }

    /** true once after the host changed the value (consumed by HostSync on the message thread). */
    bool consumeHostChange() noexcept { return m_hostChanged.exchange(false, std::memory_order_acq_rel); }

    /**
     * While alive on this thread, setValue() doesn't store (the slot already holds the exact plain value;
     * re-deriving it from a normalized float would lose precision) and doesn't flag a host change.
     * Used when pushing our own changes to the host with setValueNotifyingHost().
     */
    struct ScopedOwnWrite {
        ScopedOwnWrite() noexcept { ++s_ownWriteDepth; }
        ~ScopedOwnWrite() { --s_ownWriteDepth; }
    };

private:
    static inline thread_local int s_ownWriteDepth = 0;

    std::shared_ptr<ConfigManager> m_keepAlive;   // the slot lives in ConfigManager
    std::string                    m_nsKey;
    ParamMeta                      m_meta;
    std::atomic<float>&            m_slot;
    std::atomic<bool>              m_hostChanged { false };
    float                          m_defaultNormalized = 0.0f;
    juce::String                   m_unit;
    bool                           m_discrete = false;
    bool                           m_boolean  = false;
    int                            m_numSteps = 0;
    juce::StringArray              m_valueStrings;
};

} // namespace winerose::plugin
