#include "presets/PresetFormat.h"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace winerose {

namespace {

bool hasAt(std::span<const std::uint8_t> b, std::size_t offset, std::string_view tag) noexcept
{
    return b.size() >= offset + tag.size() && std::memcmp(b.data() + offset, tag.data(), tag.size()) == 0;
}

} // namespace

PresetFormat detectPresetFormat(std::span<const std::uint8_t> bytes) noexcept
{
    // SPEC §2.1: magic "XferJson\0", then at least the u64 length + u32 size + u32 version.
    if (hasAt(bytes, 0, std::string_view("XferJson\0", 9)) && bytes.size() >= 25)
        return PresetFormat::SerumPreset;

    // SPEC §2.2: 'CcnK' at 0x00, chunk magic at 0x08.
    if (hasAt(bytes, 0, "CcnK")) {
        if (hasAt(bytes, 8, "FPCh") || hasAt(bytes, 8, "FxCk")) return PresetFormat::SerumFxp;
        if (hasAt(bytes, 8, "FBCh") || hasAt(bytes, 8, "FxBk")) return PresetFormat::SerumFxb;
        return PresetFormat::Unknown;
    }

    // Own format: a JSON object containing the state format tag near the start.
    std::size_t i = 0;
    while (i < bytes.size() && (bytes[i] == ' ' || bytes[i] == '\n' || bytes[i] == '\r' || bytes[i] == '\t')) ++i;
    if (i < bytes.size() && bytes[i] == '{') {
        const std::string_view text(reinterpret_cast<const char*>(bytes.data()), std::min<std::size_t>(bytes.size(), 256));
        if (text.find("\"winerose.state\"") != std::string_view::npos) return PresetFormat::WineroseState;
    }
    return PresetFormat::Unknown;
}

const char* presetFormatName(PresetFormat format) noexcept
{
    switch (format) {
        case PresetFormat::WineroseState: return "Winerose state";
        case PresetFormat::SerumPreset:   return ".SerumPreset";
        case PresetFormat::SerumFxp:      return ".fxp";
        case PresetFormat::SerumFxb:      return ".fxb";
        case PresetFormat::Unknown:       break;
    }
    return "unknown";
}

} // namespace winerose
