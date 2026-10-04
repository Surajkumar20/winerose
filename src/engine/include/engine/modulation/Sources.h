#pragma once

namespace winerose::modulation {

inline constexpr int kEnvCount   = 4;
inline constexpr int kLfoCount   = 10;
inline constexpr int kMacroCount = 8;
inline constexpr int kSlotCount  = 64;

/**
 * @brief Modulation sources (SPEC §1.6). Every source is normalized to [0,1]; a slot's "bipolar" switch maps
 *        it to [-1,1]. Pitch bend rests at 0.5. The order is the "source" enum of every ModSlot and is
 *        TODO-MEASURE against Serum 2's source menu.
 */
enum class Source : int {
    None = 0,
    Env1, Env2, Env3, Env4,
    Lfo1, Lfo2, Lfo3, Lfo4, Lfo5, Lfo6, Lfo7, Lfo8, Lfo9, Lfo10,
    Macro1, Macro2, Macro3, Macro4, Macro5, Macro6, Macro7, Macro8,
    Velocity,
    Note,          // note number / 127
    ModWheel,
    PitchBend,
    Aftertouch,
    NoteRandom1,   // drawn per note
    NoteRandom2,
    Fixed,         // constant 1
    Count
};

inline constexpr const char* kSourceNames[] = {
    "None",
    "Env 1", "Env 2", "Env 3", "Env 4",
    "LFO 1", "LFO 2", "LFO 3", "LFO 4", "LFO 5", "LFO 6", "LFO 7", "LFO 8", "LFO 9", "LFO 10",
    "Macro 1", "Macro 2", "Macro 3", "Macro 4", "Macro 5", "Macro 6", "Macro 7", "Macro 8",
    "Velocity", "Note", "Mod Wheel", "Pitch Bend", "Aftertouch", "NoteOn Rand 1", "NoteOn Rand 2", "Fixed",
};
static_assert(sizeof(kSourceNames) / sizeof(kSourceNames[0]) == static_cast<int>(Source::Count));

constexpr bool isEnv(Source s) noexcept   { return s >= Source::Env1 && s <= Source::Env4; }
constexpr bool isLfo(Source s) noexcept   { return s >= Source::Lfo1 && s <= Source::Lfo10; }
constexpr bool isMacro(Source s) noexcept { return s >= Source::Macro1 && s <= Source::Macro8; }
constexpr int  envIndex(Source s) noexcept   { return static_cast<int>(s) - static_cast<int>(Source::Env1); }
constexpr int  lfoIndex(Source s) noexcept   { return static_cast<int>(s) - static_cast<int>(Source::Lfo1); }
constexpr int  macroIndex(Source s) noexcept { return static_cast<int>(s) - static_cast<int>(Source::Macro1); }

} // namespace winerose::modulation
