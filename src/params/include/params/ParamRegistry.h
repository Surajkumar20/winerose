#pragma once

#include "params/ConfigManager.h"
#include "params/ParamHandle.h"
#include "params/ParamTypes.h"

#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace winerose {

/**
 * @class ParamRegistry
 * @brief The tunable parameters of ONE engine module (an oscillator, a filter, an LFO, …).
 *
 * Same contract as example-paramregistry.txt, applied per engine module instead of per plugin
 * (PLAN.md §3): registries are named after Serum 2's CBOR module keys ("Oscillator0", "Filter1",
 * "LFO3", "Global", …) so preset import maps module → registry 1:1.
 *
 * It never stores a value itself — it holds a shared_ptr<ConfigManager> (composition, not inheritance)
 * and asks ConfigManager to (a) claim a collision-free namespace name, (b) read/write live values,
 * (c) export this registry's schema.
 *
 * Threading: everything here is non-realtime (asserted). The audio thread reads values through
 * handle() → ParamHandle, obtained once in prepare(). Registration happens during engine construction,
 * before the host sees the parameter list; after that the set of keys is fixed.
 */
class ParamRegistry {
public:
    /**
     * @param configManager The owning engine's ConfigManager.
     * @param requestedName Module name, e.g. "Oscillator0". Becomes the namespace prefix of every key,
     *                      after collision resolution against other registries on the same ConfigManager.
     */
    ParamRegistry(std::shared_ptr<ConfigManager> configManager, const std::string& requestedName);

    /** @brief Detaches from ConfigManager's live registry. */
    ~ParamRegistry();

    ParamRegistry(const ParamRegistry&) = delete;
    ParamRegistry& operator=(const ParamRegistry&) = delete;

    /** @return The collision-resolved name, e.g. "Oscillator0". */
    const std::string& resolvedName() const { return m_resolved_name_; }

    /** @return resolvedName() + "." + key — the host parameter ID and the state-document key. */
    std::string namespacedKey(const std::string& key) const { return m_resolved_name_ + "." + key; }

    std::shared_ptr<ConfigManager> configManager() const { return m_config_; }

    // --- Registration ---
    // Each of these hydrates immediately: reads the stored value from ConfigManager (falling back to
    // default_value if absent), clamps it, writes it back so the state always contains the key, then
    // records the ParamDef (with the ORIGINAL default, not the hydrated value).

    // NOTE the argument order: default value first, then min, then max (min-to-max order).
    void registerInt(const std::string& key,
                     int default_value, int min_val, int max_val,
                     const std::string& group = "", const std::string& tooltip = "",
                     const ParamOpts& opts = {});

    void registerFloat(const std::string& key,
                       float default_value, float min_val, float max_val,
                       const std::string& group = "", const std::string& tooltip = "",
                       const ParamOpts& opts = {});

    void registerDouble(const std::string& key,
                        double default_value, double min_val, double max_val,
                        const std::string& group = "", const std::string& tooltip = "",
                        const ParamOpts& opts = {});

    // Stored as an integer MILLISECOND count (INT numeric meta, unit "ms"). Read it back typed with
    // get<std::chrono::milliseconds>(key) — or any std::chrono::duration.
    void registerDuration(const std::string& key,
                          std::chrono::milliseconds default_value,
                          std::chrono::milliseconds min_val,
                          std::chrono::milliseconds max_val,
                          const std::string& group = "", const std::string& tooltip = "",
                          const ParamOpts& opts = {});

    // Strings are never host-automatable (opts.automatable is forced false).
    void registerString(const std::string& key,
                        const std::string& default_value,
                        const std::string& group = "", const std::string& tooltip = "",
                        const ParamOpts& opts = {});

    void registerBool(const std::string& key,
                      bool default_value,
                      const std::string& group = "", const std::string& tooltip = "",
                      const ParamOpts& opts = {});

    void registerEnum(const std::string& key,
                      std::vector<EnumChoice> choices, int default_value,
                      const std::string& group = "", const std::string& tooltip = "",
                      const ParamOpts& opts = {});

    /**
     * @brief Template wrapper for typed enums.
     *   registry.registerEnum<Quality>("quality",
     *       { {Quality::Good, "Good"}, {Quality::High, "High"} }, Quality::Good, "Global");
     */
    template<typename EnumT>
    void registerEnum(const std::string& key,
                      std::initializer_list<std::pair<EnumT, const char*>> choices,
                      EnumT default_value,
                      const std::string& group = "", const std::string& tooltip = "",
                      const ParamOpts& opts = {})
    {
        std::vector<EnumChoice> ec;
        ec.reserve(choices.size());
        for (const auto& [val, label] : choices)
            ec.push_back({static_cast<int>(val), std::string(label)});
        registerEnum(key, std::move(ec), static_cast<int>(default_value), group, tooltip, opts);
    }

    // --- Live value accessors (non-realtime). Namespace the key internally. ---

    /** Unregistered keys return T{}. Enums may be read as their EnumT. */
    template<typename T> T get(const std::string& key) const;

    /** Clamped / snapped to the registered meta. Unregistered keys are ignored. */
    template<typename T> void set(const std::string& key, const T& value);

    /**
     * @brief Set a param from a user-typed STRING (what an editor UI produces), interpreting/clamping
     *        it according to the param's REGISTERED type. Numeric: parsed and clamped. Enum/bool: only a
     *        legal choice value or label. String: stored verbatim.
     * @return false — value untouched — if the key isn't registered, the text doesn't parse, or it
     *         isn't a legal enum choice.
     */
    bool modify(const std::string& key, const std::string& rawValue);

    /** Every param back to its registered default, inside one ConfigManager batch. */
    void resetToDefaults();

    // --- Realtime access ---

    /** ParamHandle for a numeric/enum/bool key; invalid handle for strings or unknown keys. */
    ParamHandle handle(const std::string& key) const;

    // --- Normalization (0..1 ↔ plain), per the registered curve ---

    double toNormalized(const std::string& key, double plain) const;
    double fromNormalized(const std::string& key, double normalized) const;

    // --- Query / tooling ---

    /** Registration is construction-time only, so the returned reference is stable afterwards. */
    const std::vector<ParamDef>& getAll() const;

    /** Copy of one definition (thread-safe). */
    std::optional<ParamDef> find(const std::string& key) const;

    // --- Validation ---

    std::vector<ValidationError> validate() const;

    /** Clamp the live value of def into range. @return true if the value had to change. */
    bool clampAndApply(const ParamDef& def);

private:
    void registerNumeric(const std::string& key, double default_value, double min_val, double max_val,
                         NumericMeta::SubType subtype, const std::string& group, const std::string& tooltip,
                         const ParamOpts& opts);
    void addDef(ParamDef def);
    std::optional<ParamMeta> metaFor(const std::string& key) const;
    void setPlain(const std::string& key, const ParamMeta& meta, double plain);

    std::shared_ptr<ConfigManager> m_config_;
    std::string                    m_resolved_name_;
    std::vector<ParamDef>          m_params_;
    mutable std::mutex             m_mutex_;
    bool                           m_detached_ = false;
};

} // namespace winerose

#include "params/ParamRegistry.ipp"
