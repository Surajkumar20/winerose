#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace winerose::presets {

/**
 * @brief The Serum 2 container (SPEC §2.1, confirmed byte layout):
 *
 *   0      9  "XferJson\0"
 *   9      8  u64 LE N = JSON metadata length
 *   17     N  UTF-8 JSON metadata ({"fileType":"SerumPreset","presetName":...} or a processor-state header)
 *   17+N   4  u32 LE uncompressed CBOR size
 *   21+N   4  u32 LE container version (= 2)
 *   25+N   M  zstd frame containing CBOR (RFC 8949)
 *
 * The CBOR root is a map of modules (Oscillator0..4, Env0.., LFO*, Macro*, ModSlot*, FXRack0..2, ...), each
 * usually holding "plainParams" (a map of kParam* names to numbers, or the string "default"). Byte strings
 * (embedded wavetables, samples, IRs) are kept as JSON binary values.
 */
struct SerumPresetFile {
    nlohmann::json meta;        // the JSON header
    nlohmann::json root;        // the decoded CBOR document
    std::uint32_t  version = 0; // container version (2 expected)
    std::vector<std::string> warnings;

    /** Parse a container. Returns nullopt with `error` set if the bytes aren't a readable .SerumPreset. */
    static std::optional<SerumPresetFile> read(std::span<const std::uint8_t> bytes, std::string& error);

    /** Re-encode (tests and round-trip checks only — Winerose does not write Serum presets for users). */
    std::vector<std::uint8_t> write() const;

    std::string presetName() const { return meta.value("presetName", std::string{}); }
    std::string author() const { return meta.value("presetAuthor", std::string{}); }
    bool isProcessorState() const { return meta.value("component", std::string{}) == "processor"; }
};

} // namespace winerose::presets
