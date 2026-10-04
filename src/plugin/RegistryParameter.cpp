#include "RegistryParameter.h"

#include <cmath>
#include <cstdlib>

namespace winerose::plugin {

RegistryParameter::RegistryParameter(std::shared_ptr<ConfigManager> config, const std::string& nsKey,
                                     const std::string& displayName, const ParamDef& def,
                                     std::atomic<float>& slot)
    : juce::AudioProcessorParameterWithID(juce::ParameterID { nsKey, 1 }, displayName)
    , m_keepAlive(std::move(config))
    , m_nsKey(nsKey)
    , m_meta(def.meta)
    , m_slot(slot)
{
    // default_value is the stringified default: a number, or an enum label ("Good").
    const auto defaultPlain = parsePlain(m_meta, def.default_value);
    m_defaultNormalized = static_cast<float>(toNormalized(m_meta, defaultPlain.value_or(0.0)));

    if (const auto* n = std::get_if<NumericMeta>(&m_meta)) {
        m_unit = n->unit;
        if (n->subtype == NumericMeta::SubType::INT) {
            m_discrete = true;
            m_numSteps = static_cast<int>(std::llround(n->max_val - n->min_val)) + 1;
        }
    } else if (const auto* e = std::get_if<EnumMeta>(&m_meta)) {
        m_discrete = true;
        m_boolean  = e->is_bool;
        m_numSteps = static_cast<int>(e->choices.size());
        for (const auto& c : e->choices) m_valueStrings.add(c.label);
    }
}

float RegistryParameter::getValue() const
{
    return static_cast<float>(toNormalized(m_meta, m_slot.load(std::memory_order_relaxed)));
}

void RegistryParameter::setValue(float newValue)
{
    if (s_ownWriteDepth > 0) return;
    m_slot.store(static_cast<float>(fromNormalized(m_meta, newValue)), std::memory_order_relaxed);
    m_hostChanged.store(true, std::memory_order_release);
}

juce::String RegistryParameter::getText(float normalizedValue, int maximumStringLength) const
{
    const juce::String text(formatPlain(m_meta, fromNormalized(m_meta, normalizedValue)));
    return maximumStringLength > 0 ? text.substring(0, maximumStringLength) : text;
}

float RegistryParameter::getValueForText(const juce::String& text) const
{
    const auto plain = parsePlain(m_meta, text.trim().toStdString());
    return plain ? static_cast<float>(toNormalized(m_meta, *plain)) : getValue();
}

int RegistryParameter::getNumSteps() const
{
    return m_discrete && m_numSteps > 1 ? m_numSteps : juce::AudioProcessor::getDefaultNumParameterSteps();
}

} // namespace winerose::plugin
