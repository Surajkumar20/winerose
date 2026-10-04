#pragma once

#include <cstdint>
#include <span>

namespace winerose {

// Which container a byte blob is. Readers/mappers for each land in feature/presets (SPEC Part 2, §5.6);
// this branch only identifies the container so IController::loadPreset can route it.
enum class PresetFormat {
    Unknown,
    WineroseState,   // ConfigManager::serialize() JSON document — the own format
    SerumPreset,     // "XferJson\0" + u64 JSON length + JSON + u32 size + u32 version + zstd(CBOR)
    SerumFxp,        // VST2 "CcnK" ... "FPCh" ... "XfsX" program chunk
    SerumFxb,        // VST2 "CcnK" ... "FBCh"/"FxBk" bank
};

PresetFormat detectPresetFormat(std::span<const std::uint8_t> bytes) noexcept;

const char* presetFormatName(PresetFormat format) noexcept;

} // namespace winerose
