#pragma once

#include "engine/fx/FxRack.h"

#include "params/ConfigManager.h"
#include "params/ParamHandle.h"
#include "params/ParamRegistry.h"

#include <array>
#include <memory>
#include <string>

namespace winerose::modules {

// --- FX slot (FXRack<r>Slot<s>: r = 0 Main, 1 Bus 1, 2 Bus 2) ------------------------------------------
namespace fx_keys {
inline constexpr const char* type    = "type";
inline constexpr const char* enabled = "enabled";
inline constexpr const char* mix     = "mix";
inline constexpr const char* param[fx::kParamCount] = {"p0", "p1", "p2", "p3", "p4", "p5", "p6", "p7"};
}

class FxSlotModule {
public:
    FxSlotModule(std::shared_ptr<ConfigManager> config, int rack, int slot);
    fx::SlotParams read() const noexcept;        // realtime
    fx::FxType type() const noexcept;            // any thread (a handle read)
    ParamRegistry& registry() noexcept { return *m_registry; }

    static std::string registryName(int rack, int slot)
    {
        return "FXRack" + std::to_string(rack) + "Slot" + std::to_string(slot);
    }

private:
    std::unique_ptr<ParamRegistry> m_registry;
    ParamHandle m_type, m_enabled, m_mix;
    std::array<ParamHandle, fx::kParamCount> m_p;
};

// --- Mixer: output levels of the Direct bus and the two FX buses (SPEC §1.1 "Direct Vol", "Bus 1/2 Vol") --
class MixerModule {
public:
    struct Values { float direct, bus1, bus2; };
    explicit MixerModule(std::shared_ptr<ConfigManager> config);
    Values read() const noexcept;
    ParamRegistry& registry() noexcept { return *m_registry; }

private:
    std::unique_ptr<ParamRegistry> m_registry;
    ParamHandle m_direct, m_bus1, m_bus2;
};

} // namespace winerose::modules
