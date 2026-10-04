#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace winerose::presets {

/**
 * @brief One Serum 1 program from a VST2 .fxp (or one entry of a .fxb), SPEC §2.2.
 *
 *   0x00 'CcnK'  0x04 u32 BE size  0x08 'FPCh'  0x0C u32 BE version  0x10 'XfsX' (Serum)
 *   0x14 u32 BE fxVersion  0x18 u32 BE numParams  0x1C char[28] program name  0x38 u32 BE chunk size
 *   0x3C chunk: [zlib #0 → 172,736-byte state][zlib #1..n: wavetables / noise / filter tables][u32 LE trailer]
 *
 * State fields (pycabbage/flp-extract-fxp, validated against 25 files): name @0x4972 (32 B), version f32
 * @0x4994, author @0x49A0 (48 B), category @0x49D0 (48 B). Older states (21,808 / 28,232 B) are zero-padded to
 * 172,736 as Serum 2 does. The parameter array at 0x3460 (~300 LE floats) is from a LOW-REPUTATION source and
 * is exposed as INFERRED until verified.
 */
struct FxpProgram {
    static constexpr std::size_t kStateSize = 172736;
    static constexpr std::size_t kParamOffset = 0x3460;
    static constexpr std::size_t kParamCount = 300;

    std::string fxId;                 // "XfsX" for Serum
    std::string headerName;           // the VST2 header's 28-byte program name
    std::string name, author, category;
    float       stateVersion = 0.0f;  // e.g. 0.1631
    std::vector<std::uint8_t> state;  // zero-padded to kStateSize
    std::size_t originalStateSize = 0;
    std::vector<std::vector<std::uint8_t>> extraStreams;   // inflated: wavetables, tuning, noise, filter tables
    std::vector<float> params;        // INFERRED parameter array (empty if out of range)
    std::vector<std::string> warnings;

    bool isSerum() const { return fxId == "XfsX"; }
};

struct FxpFile {
    bool isBank = false;
    std::vector<FxpProgram> programs;

    /** Parse a .fxp (one program) or .fxb (several). nullopt with `error` set on failure. */
    static std::optional<FxpFile> read(std::span<const std::uint8_t> bytes, std::string& error);

    /** Build a minimal Serum-style .fxp around a raw state (tests only). */
    static std::vector<std::uint8_t> writeForTests(const std::string& programName, const std::vector<std::uint8_t>& state);
};

} // namespace winerose::presets
