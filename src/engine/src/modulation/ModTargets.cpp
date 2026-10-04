#include "engine/modulation/ModTargets.h"

#include <cassert>
#include <stdexcept>

namespace winerose::modulation {

int ModTargets::add(ParamRegistry& registry, const std::string& key)
{
    const auto def = registry.find(key);
    if (!def || std::holds_alternative<StringMeta>(def->meta))
        throw std::logic_error("ModTargets::add: '" + key + "' is not a registered numeric parameter");
    if (static_cast<int>(m_targets.size()) >= kMaxTargets)
        throw std::logic_error("ModTargets: kMaxTargets exceeded");

    Target t;
    t.nsKey       = registry.namespacedKey(key);
    t.handle      = registry.handle(key);
    t.meta        = def->meta;
    t.modulatable = std::holds_alternative<NumericMeta>(def->meta);
    const int index = static_cast<int>(m_targets.size());
    m_index.emplace(t.nsKey, index);
    m_targets.push_back(std::move(t));
    return index;
}

int ModTargets::find(const std::string& nsKey) const
{
    const auto it = m_index.find(nsKey);
    return it == m_index.end() ? -1 : it->second;
}

void ModTargets::readBase(float* out) const noexcept
{
    for (std::size_t i = 0; i < m_targets.size(); ++i) out[i] = m_targets[i].handle.load();
}

} // namespace winerose::modulation
