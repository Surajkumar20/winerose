#pragma once

#include <atomic>

namespace winerose {

/**
 * @brief Realtime-safe read access to one numeric/enum/bool parameter's live plain value.
 *
 * Trivially copyable: no strings, no locks, no allocation. Obtain once on a non-realtime thread
 * (ParamRegistry::handle(), typically in Engine::prepare()), then load() freely on the audio thread.
 * The slot it points to is owned by ConfigManager and has a stable address for ConfigManager's lifetime.
 */
struct ParamHandle {
    const std::atomic<float>* slot = nullptr;

    float load() const noexcept { return slot->load(std::memory_order_relaxed); }
    bool  valid() const noexcept { return slot != nullptr; }
};

} // namespace winerose
