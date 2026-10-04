#pragma once

#include "params/ParamHandle.h"
#include "params/ParamRegistry.h"
#include "params/ParamTypes.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace winerose::modulation {

/**
 * @brief Every numeric parameter a voice reads, in one indexed table (SPEC §5.3 "Modulation").
 *
 * Modules register each parameter here right after registering it on their ParamRegistry, and keep the
 * returned index. Once per control tick the Engine reads every target's live value into a "base" array;
 * each voice copies it, applies its mod-matrix slots in normalized space, and builds its controls from
 * the result. Modules therefore read plain values from an array (`read(const float*)`), never from
 * handles directly — the same code path serves modulated and unmodulated values.
 *
 * Only continuous numeric parameters are modulation destinations; enums and bools are present (so modules
 * can read them from the same array) but flagged non-modulatable.
 */
class ModTargets {
public:
    struct Target {
        std::string nsKey;
        ParamHandle handle;
        ParamMeta   meta;
        bool        modulatable = false;
    };

    static constexpr int kMaxTargets = 512;

    /** Register an already-registered key of `registry`. Returns its index. Non-realtime. */
    int add(ParamRegistry& registry, const std::string& key);

    int count() const noexcept { return static_cast<int>(m_targets.size()); }
    const Target& at(int index) const noexcept { return m_targets[static_cast<std::size_t>(index)]; }

    /** Index of a namespaced key, or -1. Non-realtime. */
    int find(const std::string& nsKey) const;

    /** Read every live value (realtime-safe). out must hold count() floats. */
    void readBase(float* out) const noexcept;

private:
    std::vector<Target>                  m_targets;
    std::unordered_map<std::string, int> m_index;
};

} // namespace winerose::modulation
