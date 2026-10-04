#pragma once

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>
#include <stdexcept>
#include <sstream>
#include <algorithm> // Required for std::transform
#include <cctype>    // Required for ::tolower

// Forward declarations only — ParamRegistry.h includes this header (needs the full
// ConfigManager type), so this header must not include ParamRegistry.h back (circular).
class ParamRegistry;
struct ParamDef;

/**
 * @class ConfigManager
 * @brief Manages loading, saving, and accessing configuration settings from a file.
 *
 * This class is designed to reads and writes a simple .ini-style format (key = value).
 */
class ConfigManager {
public:
    /**
     * @brief Constructs a ConfigManager and immediately tries to load from the given file.
     * @param filepath The path to the configuration file to load.
     */
    explicit ConfigManager(const std::string& filepath);

    /**
     * @brief Default constructor.
     */
    ConfigManager() = default;

    /**
     * @brief Create default settings.
     */
    void createDefaultSettings();

    /**
     * @brief Loads configuration from a file.
     * @param filepath The path to the configuration file. If empty, it uses the
     * path provided in the constructor (if any).
     * @return true if loading was successful, false otherwise.
     */
    bool load(const std::string& filepath = "");

    /**
     * @brief Saves the current configuration back to a file.
     * @param filepath The path to save to. If empty, it uses the
     * path this manager was loaded from.
     * @return true if saving was successful, false otherwise.
     */
    bool save(const std::string& filepath = "");

    /**
     * @brief Defer the per-set() write-through until endBatch().
     *
     * set()/setRaw() normally persist to disk on every call (crash-safety). During bulk
     * registration (e.g. a plugin registering dozens of params at init) that means dozens
     * of redundant disk writes + log lines. Wrap the bulk work in beginBatch()/endBatch()
     * to coalesce them into a SINGLE save at the end. Writes made while deferred are held
     * in memory and flushed by endBatch() iff any occurred.
     *
     * Not reentrant/nested — beginBatch() simply sets the flag, endBatch() clears it and
     * flushes. Intended for single-threaded startup/bulk sections.
     */
    void beginBatch();

    /** @brief End a beginBatch() section: re-enable write-through and save once if dirty. */
    void endBatch();

    /**
     * @brief Gets a configuration value.
     * @tparam T The type to convert the value to (int, double, bool, std::string).
     * @param key The name of the configuration key.
     * @param defaultValue The value to return if the key is not found.
     * @return The retrieved value, or defaultValue if not found or conversion fails.
     */
    template<typename T>
    T get(const std::string& key, const T& defaultValue) const;

    /**
     * @brief Sets a configuration value and persists it immediately (write-through).
     * @note Unlike the old behavior, this now saves to disk on every call — creating
     * the file if it doesn't exist yet — so edits survive a crash without an explicit save().
     * @tparam T The type of the value to set (int, double, bool, std::string).
     * @param key The name of the configuration key.
     * @param value The value to set.
     */
    template<typename T>
    void set(const std::string& key, const T& value);

    /**
     * @brief Sets a configuration value (write-through) when the value is char[] type.
     * @param key The name of the configuration key.
     * @param value The char[] value to set.
     */
    void set(const std::string& key, const char* value);

    /**
     * @brief clear all data in m_data_.
     */
    void clear_all();

    /**
     * @brief The file path this manager currently reads from / writes to.
     * @note Set by the last successful load()/save() (and the constructor). Empty if this
     * manager has never been bound to a file. Thread-safe.
     */
    std::string filePath() const;

    /**
     * @brief Claims a collision-free namespace name for a new ParamRegistry instance and
     * records it in the in-memory live registry. Must be called before any key is registered
     * on that instance (the returned name becomes that instance's key-namespace prefix).
     * @param requestedName The plugin's own logical name, e.g. "blobviz".
     * @param registry Raw pointer to the ParamRegistry instance being attached. Lifetime is
     * owned by the plugin — ConfigManager does not take ownership and must have
     * detachParamRegistry called before the object is destroyed.
     * @return The resolved, collision-free name (e.g. "blobviz")
     */
    std::string attachParamRegistry(const std::string& requestedName, ParamRegistry* registry);

    /**
     * @brief Removes a previously-attached ParamRegistry from the live registry by its
     * resolved name. Idempotent — calling twice or with an unknown name is a no-op.
     */
    void detachParamRegistry(const std::string& resolvedName);

    /**
     * @brief Returns a snapshot of the resolved-name -> ParamRegistry* map of all
     * currently-attached instances. Intended for Configurator/PluginManager UI enumeration.
     */
    std::map<std::string, ParamRegistry*> getAttachedParamRegistries() const;

    /**
     * @brief Serializes one ParamRegistry's schema (ParamDef list, NOT live values) to
     * bin/params/<resolvedName>/schema.ini. Overwrites any existing schema file for that
     * name — schema always reflects the current build's param set.
     * @param resolvedName The name returned by attachParamRegistry.
     * @param defs The ParamDef list to serialize (ParamRegistry::getAll()).
     * @return true on success.
     */
    bool writeParamSchema(const std::string& resolvedName, const std::vector<ParamDef>& defs);

private:
    // Helper functions for string manipulation
    static std::string trim(const std::string& str);
    static std::pair<std::string, std::string> parseLine(const std::string& line);

    // Mutates m_data_ then persists immediately — the one place set<T> writes to disk.
    void setRaw(const std::string& key, const std::string& value);

    std::string m_filepath_;
    std::map<std::string, std::string> m_data_;
    mutable std::mutex m_mutex_; // `mutable` allows locking in const `get` method

    // Batch mode (see beginBatch/endBatch): while m_defer_save_ is true, setRaw() skips the
    // per-write save() and just marks m_batch_dirty_; endBatch() flushes once.
    std::atomic<bool> m_defer_save_ { false };
    std::atomic<bool> m_batch_dirty_ { false };

    // Live registry of currently-attached per-plugin-instance ParamRegistry objects.
    // Separate mutex from m_mutex_ — attach/detach is rare (plugin load/unload) while
    // m_mutex_ guards the hot path of every get<T>/set<T> call; no need to contend.
    std::map<std::string, ParamRegistry*> m_attached_registries_;
    mutable std::mutex m_registry_mutex_;
};

#include "configurations/ConfigManager.ipp"