#include "engine/modules/FxModules.h"

#include <cmath>
#include <vector>

namespace winerose::modules {

FxSlotModule::FxSlotModule(std::shared_ptr<ConfigManager> config, int rack, int slot)
    : m_registry(std::make_unique<ParamRegistry>(std::move(config), registryName(rack, slot)))
{
    auto& r = *m_registry;
    static const char* kRackNames[] = {"Main", "Bus 1", "Bus 2"};
    const std::string g = std::string("FX ") + kRackNames[rack];
    std::vector<EnumChoice> types;
    for (int t = 0; t < static_cast<int>(fx::FxType::Count); ++t) types.push_back({t, fx::kFxTypeNames[t]});
    // The effect type is rack topology (patch state), not automation (SPEC §5.3).
    r.registerEnum (fx_keys::type, std::move(types), 0, g, "Effect type (splitters feed their bands to the next slots)",
                    ParamOpts{.automatable = false});
    r.registerBool (fx_keys::enabled, false, g, "Slot on/off");
    r.registerFloat(fx_keys::mix, 1.0f, 0.0f, 1.0f, g, "Dry/wet");
    for (int i = 0; i < fx::kParamCount; ++i)
        r.registerFloat(fx_keys::param[i], 0.5f, 0.0f, 1.0f, g, "Effect parameter (meaning depends on the type)");

    m_type = r.handle(fx_keys::type);
    m_enabled = r.handle(fx_keys::enabled);
    m_mix = r.handle(fx_keys::mix);
    for (int i = 0; i < fx::kParamCount; ++i) m_p[static_cast<std::size_t>(i)] = r.handle(fx_keys::param[i]);
}

fx::FxType FxSlotModule::type() const noexcept
{
    return static_cast<fx::FxType>(std::clamp(static_cast<int>(std::lround(m_type.load())), 0, static_cast<int>(fx::FxType::Count) - 1));
}

fx::SlotParams FxSlotModule::read() const noexcept
{
    fx::SlotParams s;
    s.type = type();
    s.enabled = m_enabled.load() >= 0.5f;
    s.mix = m_mix.load();
    for (int i = 0; i < fx::kParamCount; ++i) s.p[static_cast<std::size_t>(i)] = m_p[static_cast<std::size_t>(i)].load();
    return s;
}

MixerModule::MixerModule(std::shared_ptr<ConfigManager> config)
    : m_registry(std::make_unique<ParamRegistry>(std::move(config), "Mixer"))
{
    auto& r = *m_registry;
    r.registerFloat("directLevel", 1.0f, 0.0f, 1.0f, "Mixer", "Level of the Direct bus (bypasses all FX)");
    r.registerFloat("bus1Level", 1.0f, 0.0f, 1.0f, "Mixer", "Output level of FX Bus 1");
    r.registerFloat("bus2Level", 1.0f, 0.0f, 1.0f, "Mixer", "Output level of FX Bus 2");
    m_direct = r.handle("directLevel");
    m_bus1 = r.handle("bus1Level");
    m_bus2 = r.handle("bus2Level");
}

MixerModule::Values MixerModule::read() const noexcept
{
    return {m_direct.load(), m_bus1.load(), m_bus2.load()};
}

} // namespace winerose::modules
