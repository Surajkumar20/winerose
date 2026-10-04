#include "presets/SerumPresetFile.h"

#include <zstd.h>

#include <cstring>

namespace winerose::presets {

namespace {

std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t o)
{
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8)
         | (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}

std::uint64_t le64(std::span<const std::uint8_t> b, std::size_t o)
{
    return static_cast<std::uint64_t>(le32(b, o)) | (static_cast<std::uint64_t>(le32(b, o + 4)) << 32);
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void put64(std::vector<std::uint8_t>& out, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

constexpr char kMagic[] = "XferJson";   // + terminating NUL = 9 bytes
constexpr std::size_t kMaxCbor = 512u * 1024u * 1024u;   // sanity cap on the declared size

} // namespace

std::optional<SerumPresetFile> SerumPresetFile::read(std::span<const std::uint8_t> b, std::string& error)
{
    if (b.size() < 25 || std::memcmp(b.data(), kMagic, 9) != 0) {
        error = "not an XferJson container";
        return std::nullopt;
    }
    const std::uint64_t n = le64(b, 9);
    if (n > b.size() || 17 + n + 8 > b.size()) {
        error = "JSON length exceeds the file size";
        return std::nullopt;
    }
    SerumPresetFile file;
    const auto jsonBytes = b.subspan(17, static_cast<std::size_t>(n));
    file.meta = nlohmann::json::parse(jsonBytes.begin(), jsonBytes.end(), nullptr, false);
    if (file.meta.is_discarded()) {
        error = "metadata JSON does not parse";
        return std::nullopt;
    }

    const std::size_t o = static_cast<std::size_t>(17 + n);
    const std::uint32_t rawSize = le32(b, o);
    file.version = le32(b, o + 4);
    if (file.version != 2) file.warnings.push_back("container version " + std::to_string(file.version) + " (expected 2)");
    if (rawSize == 0 || rawSize > kMaxCbor) {
        error = "implausible CBOR size " + std::to_string(rawSize);
        return std::nullopt;
    }

    const auto compressed = b.subspan(o + 8);
    std::vector<std::uint8_t> cbor(rawSize);
    const std::size_t got = ZSTD_decompress(cbor.data(), cbor.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(got)) {
        error = std::string("zstd: ") + ZSTD_getErrorName(got);
        return std::nullopt;
    }
    if (got != rawSize) file.warnings.push_back("decompressed " + std::to_string(got) + " bytes, header said " + std::to_string(rawSize));
    cbor.resize(got);

    // Keep CBOR byte strings (embedded wavetables / samples / IRs) as JSON binary values.
    file.root = nlohmann::json::from_cbor(cbor, /*strict=*/true, /*allow_exceptions=*/false,
                                          nlohmann::json::cbor_tag_handler_t::store);
    if (file.root.is_discarded()) {
        error = "CBOR payload does not decode";
        return std::nullopt;
    }
    return file;
}

std::vector<std::uint8_t> SerumPresetFile::write() const
{
    const std::string json = meta.dump();
    const std::vector<std::uint8_t> cbor = nlohmann::json::to_cbor(root);
    std::vector<std::uint8_t> compressed(ZSTD_compressBound(cbor.size()));
    const std::size_t size = ZSTD_compress(compressed.data(), compressed.size(), cbor.data(), cbor.size(), 3);
    compressed.resize(ZSTD_isError(size) ? 0 : size);

    std::vector<std::uint8_t> out(kMagic, kMagic + 9);   // includes the NUL
    put64(out, json.size());
    out.insert(out.end(), json.begin(), json.end());
    put32(out, static_cast<std::uint32_t>(cbor.size()));
    put32(out, version == 0 ? 2u : version);
    out.insert(out.end(), compressed.begin(), compressed.end());
    return out;
}

} // namespace winerose::presets
