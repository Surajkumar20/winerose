#pragma once

#include "params/ParamListener.h"
#include "params/Realtime.h"

#include <atomic>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace winerose {

// Forward declarations only — ParamRegistry.h includes this header (needs the full ConfigManager
// type), so this header must not include ParamRegistry.h back (circular).
class ParamRegistry;
struct ParamDef;

/**
 * @class ConfigManager
 * @brief Owns every parameter value of ONE engine instance, and is the only class that serializes them.
 *
 * Adapted from the desktop-app ConfigManager (example-configmanager.h) for life inside a plugin
 * (PLAN.md §3.2):
 *  - One per Engine instance, not per process — a DAW runs many Winerose instances in one process.
 *  - Values live in memory: one std::atomic<float> slot per numeric/enum/bool key (the plain value,
 *    readable on the audio thread through ParamHandle), plus a text map for string params and for
 *    restored-but-unclaimed keys. "Write-through" here means the in-memory state is always current;
 *    the host persists it by asking for serialize().
 *  - beginBatch()/endBatch() coalesce change notifications (one onBatchEnd instead of N onParamChanged).
 *  - No JUCE. Serialization is JSON via nlohmann.
 *
 * Every accessor except the slot pointers is non-realtime: it locks and may allocate.
 */
class ConfigManager {
public:
    ConfigManager() = default;
    ~ConfigManager();

    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    // --- Values -------------------------------------------------------------------------------------

    /**
     * @brief Gets a value. Arithmetic/bool T read the numeric slot (falling back to parsing a stored
     *        string); std::string T reads the text value (falling back to formatting a numeric slot).
     * @return The stored value, or defaultValue if the key is absent or not convertible.
     */
    template<typename T>
    T get(const std::string& key, const T& defaultValue) const;

    /**
     * @brief Sets a value and notifies listeners (or marks the batch dirty).
     *        Arithmetic/bool T create/update a numeric slot; std::string T stores text — unless the
     *        key already has a numeric slot and the text parses as a number.
     */
    template<typename T>
    void set(const std::string& key, const T& value);

    void set(const std::string& key, const char* value) { set<std::string>(key, std::string(value)); }

    bool contains(const std::string& key) const;

    /** Read-only slot for ParamHandle. nullptr if the key has no numeric slot. Stable address. */
    const std::atomic<float>* numericSlot(const std::string& key) const;

    /**
     * @brief Writable slot for the host adapter ONLY: host automation arrives on the audio thread and
     *        stores straight into the slot (no lock, no notification). The adapter must later call
     *        notifyChanged(key) from the message thread so listeners catch up.
     */
    std::atomic<float>* hostWritableSlot(const std::string& key);

    /** Tell listeners a key changed behind ConfigManager's back (see hostWritableSlot). */
    void notifyChanged(const std::string& key);

    /**
     * @brief Remove every key starting with `prefix` (e.g. values preserved from a previous preset import).
     *        Only meant for keys no ParamRegistry owns: a removed numeric key's slot stays allocated but
     *        unlinked. Counts as a change inside the current batch.
     */
    void erasePrefix(const std::string& prefix);

    /** Drops every text value and zeroes every numeric slot. Slots stay linked to their keys, so
     *  ParamHandles already handed out remain valid. Callers re-hydrate by resetting registries. */
    void clear_all();

    // --- Batching -----------------------------------------------------------------------------------

    /**
     * @brief Suppress per-key notifications until the matching endBatch(). Nestable: only the
     *        outermost endBatch() fires onBatchEnd(), and only if something changed inside.
     */
    void beginBatch();
    void endBatch();
    bool inBatch() const { return m_batch_depth_.load() > 0; }

    // --- Listeners ----------------------------------------------------------------------------------

    void addListener(IParamListener* listener);
    void removeListener(IParamListener* listener);

    // --- State (host chunk / own preset format) ----------------------------------------------------

    /** @brief Every value as a JSON document: {"format":"winerose.state","version":1,"values":{...}}. */
    std::string serialize() const;

    /** @brief true if blob is a state document restore() would accept. Does not modify anything. */
    static bool isValidState(const std::string& blob);

    /**
     * @brief Apply a serialize()d document inside a batch. Keys that aren't registered (e.g. from a newer
     *        build) are kept, so they survive the next serialize(). Keys absent from the blob are left
     *        untouched — callers wanting a clean load reset registries to defaults first.
     * @return false (and nothing changed) if the blob doesn't parse.
     */
    bool restore(const std::string& blob);

    // --- Registry bookkeeping (unchanged from the example) ----------------------------------------

    /**
     * @brief Claims a collision-free namespace name for a new ParamRegistry. "Oscillator" twice gives
     *        "Oscillator" and "Oscillator_2". Must be called before any key is registered on it.
     */
    std::string attachParamRegistry(const std::string& requestedName, ParamRegistry* registry);

    /** Idempotent. */
    void detachParamRegistry(const std::string& resolvedName);

    /** Snapshot of resolved-name → registry for every attached registry, sorted by name. */
    std::map<std::string, ParamRegistry*> getAttachedParamRegistries() const;

    ParamRegistry* findParamRegistry(const std::string& resolvedName) const;

    // --- Schema export (tools only — never called by the plugin at runtime) ------------------------

    /**
     * @brief Writes <rootDir>/<resolvedName>/schema.ini: one [key] section per ParamDef (schema, not live
     *        values). Overwrites — the schema always reflects the current build.
     */
    bool writeParamSchema(const std::filesystem::path& rootDir,
                          const std::string& resolvedName,
                          const std::vector<ParamDef>& defs) const;

private:
    void setNumeric(const std::string& key, double value);
    void setText(const std::string& key, const std::string& value);
    std::optional<double>      numericValue(const std::string& key) const;
    std::optional<std::string> textValue(const std::string& key) const;
    void fireChanged(const std::string& key);
    void fireBatchEnd();

    mutable std::mutex                           m_mutex_;     // guards m_slots_ growth, m_numeric_, m_text_
    std::deque<std::atomic<float>>               m_slots_;     // deque: push_back never moves existing slots
    std::unordered_map<std::string, std::size_t> m_numeric_;   // key → index into m_slots_
    std::map<std::string, std::string>           m_text_;      // string params + unclaimed restored values

    std::atomic<int>  m_batch_depth_ { 0 };
    std::atomic<bool> m_batch_dirty_ { false };

    // Separate mutex — attach/detach happens at construction; no reason to contend with value access.
    std::map<std::string, ParamRegistry*> m_attached_registries_;
    mutable std::mutex                    m_registry_mutex_;

    std::vector<IParamListener*> m_listeners_;
    mutable std::mutex           m_listener_mutex_;
};

} // namespace winerose

#include "params/ConfigManager.ipp"
