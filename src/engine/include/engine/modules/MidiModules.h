#pragma once

#include "engine/midi/Midi.h"

#include "params/ConfigManager.h"
#include "params/ParamHandle.h"
#include "params/ParamRegistry.h"

#include <array>
#include <memory>
#include <string>

namespace winerose::modules {

// MIDI generator parameters (SPEC §1.7), named after Serum 2's CBOR modules: PitchQuantizer0, Arp0,
// ClipPlayer and MidiClip0..11. They are channel-wide (not per-voice modulation targets) and are read through
// ParamHandles once per control tick.
namespace scale_keys {
inline constexpr const char* module    = "PitchQuantizer0";
inline constexpr const char* enabled   = "enabled";
inline constexpr const char* key       = "key";
inline constexpr const char* scale     = "scale";
inline constexpr const char* transpose = "transpose";
}
namespace arp_keys {
inline constexpr const char* module       = "Arp0";
inline constexpr const char* enabled      = "enabled";
inline constexpr const char* mode         = "mode";
inline constexpr const char* rate         = "rate";
inline constexpr const char* octaves      = "octaves";
inline constexpr const char* gate         = "gate";
inline constexpr const char* swing        = "swing";
inline constexpr const char* chance       = "chance";
inline constexpr const char* velocityMode = "velocityMode";
inline constexpr const char* velocity     = "velocity";
inline constexpr const char* latch        = "latch";
inline constexpr const char* transpose    = "transpose";
}
namespace clip_keys {
inline constexpr const char* module  = "ClipPlayer";
inline constexpr const char* enabled = "enabled";
inline constexpr const char* clip    = "clip";
inline constexpr const char* trigger = "trigger";
inline constexpr const char* swing   = "swing";
// MidiClip{n}
inline constexpr const char* notes   = "notes";    // string: "start,length,note,velocity;..." (beats)
inline constexpr const char* length  = "length";   // beats
}

class MidiModules {
public:
    explicit MidiModules(std::shared_ptr<ConfigManager> config);

    midi::Settings read() const noexcept;   // realtime

    /** Message thread: clip slot text and length. */
    std::string clipText(int slot) const;
    double      clipLength(int slot) const;

private:
    std::unique_ptr<ParamRegistry> m_scale, m_arp, m_clipPlayer;
    std::array<std::unique_ptr<ParamRegistry>, midi::kClipSlots> m_clips;
    ParamHandle m_scaleOn, m_key, m_scaleType, m_transpose;
    ParamHandle m_arpOn, m_arpMode, m_arpRate, m_arpOctaves, m_arpGate, m_arpSwing, m_arpChance, m_arpVelMode,
                m_arpVelocity, m_arpLatch, m_arpTranspose;
    ParamHandle m_clipOn, m_clipSlot, m_clipTrigger, m_clipSwing;
};

} // namespace winerose::modules
