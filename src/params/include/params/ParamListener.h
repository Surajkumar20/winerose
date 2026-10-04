#pragma once

#include <string>

namespace winerose {

/**
 * @brief Change notifications from ConfigManager.
 *
 * This is how the layers above (control layer, host adapter, any UI) hear about parameter changes
 * without the core knowing they exist. Callbacks run synchronously on the thread that made the change,
 * which by contract is the message/UI thread — never the audio thread.
 */
class IParamListener {
public:
    virtual ~IParamListener() = default;

    /** One namespaced key changed (outside of a batch). */
    virtual void onParamChanged(const std::string& namespacedKey) = 0;

    /** A beginBatch()/endBatch() section that changed anything has ended. Per-key notifications are
     *  suppressed inside a batch — listeners should re-read everything they care about. */
    virtual void onBatchEnd() {}
};

} // namespace winerose
