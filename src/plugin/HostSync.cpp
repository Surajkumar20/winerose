#include "HostSync.h"

namespace winerose::plugin {

HostSync::HostSync(juce::AudioProcessor& processor, ConfigManager& config, std::vector<RegistryParameter*> params)
    : m_processor(processor)
    , m_config(config)
    , m_params(std::move(params))
{
    for (auto* p : m_params) m_byKey.emplace(p->nsKey(), p);
    m_config.addListener(this);
    startTimerHz(30);
}

HostSync::~HostSync()
{
    stopTimer();
    m_config.removeListener(this);
}

void HostSync::pushToHost(RegistryParameter& param)
{
    RegistryParameter::ScopedOwnWrite ownWrite;
    param.setValueNotifyingHost(param.getValue());
}

void HostSync::onParamChanged(const std::string& nsKey)
{
    if (m_applyingHostChanges) return;   // came from the host: don't echo it back
    if (const auto it = m_byKey.find(nsKey); it != m_byKey.end()) pushToHost(*it->second);
}

void HostSync::onBatchEnd()
{
    if (m_applyingHostChanges) return;
    for (auto* p : m_params) pushToHost(*p);
    m_processor.updateHostDisplay();
}

void HostSync::beginGesture(const std::string& nsKey)
{
    if (const auto it = m_byKey.find(nsKey); it != m_byKey.end()) it->second->beginChangeGesture();
}

void HostSync::endGesture(const std::string& nsKey)
{
    if (const auto it = m_byKey.find(nsKey); it != m_byKey.end()) it->second->endChangeGesture();
}

void HostSync::timerCallback()
{
    m_applyingHostChanges = true;
    for (auto* p : m_params)
        if (p->consumeHostChange()) m_config.notifyChanged(p->nsKey());
    m_applyingHostChanges = false;
}

} // namespace winerose::plugin
