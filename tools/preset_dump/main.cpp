// preset_dump — prints what a preset file is, plus whatever its container exposes without decoding.
//
//   preset_dump <file>...
//
// Today: container detection, the Winerose state document, the .SerumPreset header + JSON metadata block
// (SPEC §2.1), and the .fxp/.fxb VST2 header (SPEC §2.2). Decompressing the Serum 2 zstd/CBOR payload and
// the Serum 1 zlib state arrives with feature/presets, which extends this tool to dump the module map.

#include "presets/PresetFormat.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

std::uint32_t le32(const Bytes& b, std::size_t o)
{
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8)
         | (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}

std::uint64_t le64(const Bytes& b, std::size_t o)
{
    return static_cast<std::uint64_t>(le32(b, o)) | (static_cast<std::uint64_t>(le32(b, o + 4)) << 32);
}

std::uint32_t be32(const Bytes& b, std::size_t o)
{
    return (static_cast<std::uint32_t>(b[o]) << 24) | (static_cast<std::uint32_t>(b[o + 1]) << 16)
         | (static_cast<std::uint32_t>(b[o + 2]) << 8) | static_cast<std::uint32_t>(b[o + 3]);
}

std::string fixedString(const Bytes& b, std::size_t o, std::size_t len)
{
    std::string s;
    for (std::size_t i = o; i < o + len && i < b.size() && b[i] != 0; ++i) s.push_back(static_cast<char>(b[i]));
    return s;
}

std::string tag(const Bytes& b, std::size_t o) { return std::string(reinterpret_cast<const char*>(b.data() + o), 4); }

bool dumpSerumPreset(const Bytes& b)
{
    // 0: "XferJson\0" | 9: u64 N | 17: JSON[N] | 17+N: u32 CBOR size | 21+N: u32 version | 25+N: zstd(CBOR)
    const std::uint64_t n = le64(b, 9);
    if (17 + n + 8 > b.size()) {
        std::cout << "  truncated: JSON length " << n << " exceeds file size\n";
        return false;
    }
    const std::size_t o = static_cast<std::size_t>(17 + n);
    const std::string jsonText(reinterpret_cast<const char*>(b.data() + 17), static_cast<std::size_t>(n));
    const auto meta = nlohmann::json::parse(jsonText, nullptr, false);
    std::cout << "  metadata (" << n << " bytes):\n";
    if (meta.is_discarded()) std::cout << "    <invalid JSON>\n";
    else                     std::cout << meta.dump(2) << "\n";
    std::cout << "  cbor uncompressed size: " << le32(b, o) << "\n"
              << "  container version:      " << le32(b, o + 4) << (le32(b, o + 4) == 2 ? "" : "  (expected 2)") << "\n"
              << "  zstd payload:           " << (b.size() - o - 8) << " bytes\n";
    return !meta.is_discarded();
}

bool dumpFxp(const Bytes& b, bool bank)
{
    if (b.size() < 60) {
        std::cout << "  truncated VST2 header\n";
        return false;
    }
    std::cout << "  byteSize:   " << be32(b, 4) << "\n"
              << "  chunkMagic: " << tag(b, 8) << "\n"
              << "  version:    " << be32(b, 12) << "\n"
              << "  fxID:       " << tag(b, 16) << (tag(b, 16) == "XfsX" ? "  (Serum)" : "") << "\n"
              << "  fxVersion:  " << be32(b, 20) << "\n";
    if (bank) {
        std::cout << "  numPrograms: " << be32(b, 24) << "\n";
    } else {
        std::cout << "  numParams:  " << be32(b, 24) << "\n"
                  << "  prgName:    \"" << fixedString(b, 28, 28) << "\"\n"
                  << "  chunkSize:  " << be32(b, 56) << "\n";
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: preset_dump <file>...\n";
        return 2;
    }
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        if (!in) {
            std::cerr << argv[i] << ": cannot open\n";
            ++failures;
            continue;
        }
        const Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto format = winerose::detectPresetFormat(bytes);
        std::cout << argv[i] << ": " << winerose::presetFormatName(format) << " (" << bytes.size() << " bytes)\n";

        bool ok = true;
        switch (format) {
            case winerose::PresetFormat::WineroseState: {
                const auto doc = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
                ok = !doc.is_discarded();
                if (ok) std::cout << doc.dump(2) << "\n";
                break;
            }
            case winerose::PresetFormat::SerumPreset: ok = dumpSerumPreset(bytes); break;
            case winerose::PresetFormat::SerumFxp:    ok = dumpFxp(bytes, false); break;
            case winerose::PresetFormat::SerumFxb:    ok = dumpFxp(bytes, true); break;
            case winerose::PresetFormat::Unknown:     ok = false; break;
        }
        if (!ok) ++failures;
    }
    return failures == 0 ? 0 : 1;
}
