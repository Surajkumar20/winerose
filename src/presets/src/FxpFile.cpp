#include "presets/FxpFile.h"

#include <zlib.h>

#include <cstring>

namespace winerose::presets {

namespace {

std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t o)
{
    return (static_cast<std::uint32_t>(b[o]) << 24) | (static_cast<std::uint32_t>(b[o + 1]) << 16)
         | (static_cast<std::uint32_t>(b[o + 2]) << 8) | static_cast<std::uint32_t>(b[o + 3]);
}

std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t o)
{
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8)
         | (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}

std::string tag(std::span<const std::uint8_t> b, std::size_t o) { return std::string(reinterpret_cast<const char*>(b.data() + o), 4); }

std::string cString(std::span<const std::uint8_t> b, std::size_t o, std::size_t len)
{
    std::string s;
    for (std::size_t i = o; i < o + len && i < b.size() && b[i] != 0; ++i) s.push_back(static_cast<char>(b[i]));
    return s;
}

// Inflate one zlib stream starting at `data`; returns false on error. `consumed` = compressed bytes used.
bool inflateStream(std::span<const std::uint8_t> data, std::vector<std::uint8_t>& out, std::size_t& consumed)
{
    z_stream zs {};
    if (inflateInit(&zs) != Z_OK) return false;
    zs.next_in = const_cast<Bytef*>(data.data());
    zs.avail_in = static_cast<uInt>(data.size());
    std::uint8_t buf[16384];
    int ret = Z_OK;
    out.clear();
    while (ret != Z_STREAM_END) {
        zs.next_out = buf;
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zs);
            return false;
        }
        out.insert(out.end(), buf, buf + (sizeof(buf) - zs.avail_out));
        if (zs.avail_in == 0 && ret != Z_STREAM_END) break;   // truncated
        if (out.size() > 64u * 1024u * 1024u) break;          // sanity cap
    }
    consumed = data.size() - zs.avail_in;
    inflateEnd(&zs);
    return ret == Z_STREAM_END;
}

std::optional<FxpProgram> parseProgram(std::span<const std::uint8_t> b, std::size_t& used, std::string& error)
{
    if (b.size() < 0x3C || tag(b, 0) != "CcnK") { error = "missing CcnK"; return std::nullopt; }
    const std::string kind = tag(b, 8);
    if (kind != "FPCh") { error = "unsupported program chunk '" + kind + "' (only opaque FPCh programs are supported)"; return std::nullopt; }

    FxpProgram p;
    p.fxId = tag(b, 0x10);
    p.headerName = cString(b, 0x1C, 28);
    const std::uint32_t chunkSize = be32(b, 0x38);
    if (0x3C + static_cast<std::size_t>(chunkSize) > b.size()) { error = "chunk size exceeds the file"; return std::nullopt; }
    used = 0x3C + chunkSize;
    const auto chunk = b.subspan(0x3C, chunkSize);
    if (!p.isSerum()) p.warnings.push_back("fxID '" + p.fxId + "' is not Serum (XfsX)");

    // Serum chunk: zlib stream 0 (the state), more zlib streams, then a u32 LE trailer.
    std::size_t consumed = 0;
    if (chunk.size() < 8 || !inflateStream(chunk, p.state, consumed)) { error = "state stream does not inflate"; return std::nullopt; }
    p.originalStateSize = p.state.size();
    if (chunk.size() >= 4) {
        const std::uint32_t trailer = le32(chunk, chunk.size() - 4);
        if (trailer != consumed) p.warnings.push_back("trailer " + std::to_string(trailer) + " != state stream size " + std::to_string(consumed));
    }
    std::size_t pos = consumed;
    while (pos + 4 < chunk.size()) {
        std::vector<std::uint8_t> extra;
        std::size_t c = 0;
        if (!inflateStream(chunk.subspan(pos, chunk.size() - 4 - pos), extra, c) || c == 0) break;
        p.extraStreams.push_back(std::move(extra));
        pos += c;
    }

    if (p.state.size() < FxpProgram::kStateSize) p.state.resize(FxpProgram::kStateSize, 0);   // older layouts
    const std::span<const std::uint8_t> st(p.state);
    p.name = cString(st, 0x4972, 32);
    float version = 0.0f;
    std::memcpy(&version, p.state.data() + 0x4994, sizeof(float));
    p.stateVersion = version;
    p.author = cString(st, 0x49A0, 48);
    p.category = cString(st, 0x49D0, 48);
    if (p.originalStateSize >= FxpProgram::kParamOffset + FxpProgram::kParamCount * 4) {
        p.params.resize(FxpProgram::kParamCount);
        std::memcpy(p.params.data(), p.state.data() + FxpProgram::kParamOffset, FxpProgram::kParamCount * 4);
    }
    return p;
}

} // namespace

std::optional<FxpFile> FxpFile::read(std::span<const std::uint8_t> b, std::string& error)
{
    if (b.size() < 0x3C || tag(b, 0) != "CcnK") { error = "not a VST2 preset (no CcnK)"; return std::nullopt; }
    FxpFile file;
    const std::string kind = tag(b, 8);
    if (kind == "FPCh") {
        std::size_t used = 0;
        auto p = parseProgram(b, used, error);
        if (!p) return std::nullopt;
        file.programs.push_back(std::move(*p));
        return file;
    }
    if (kind == "FBCh" || kind == "FxBk") {
        // Banks: programs follow the 0xA0-byte (FBCh: after its chunk-size word) / 0x9C-byte (FxBk) header.
        file.isBank = true;
        std::size_t pos = kind == "FBCh" ? 0xA0 : 0x9C;
        while (pos + 0x3C < b.size()) {
            // Find the next program header (FBCh chunks are opaque; FxBk lists programs back to back).
            if (tag(b, pos) != "CcnK") { ++pos; continue; }
            std::size_t used = 0;
            std::string programError;
            auto p = parseProgram(b.subspan(pos), used, programError);
            if (!p) { pos += 4; continue; }
            file.programs.push_back(std::move(*p));
            pos += used;
        }
        if (file.programs.empty()) { error = "bank contains no readable FPCh programs"; return std::nullopt; }
        return file;
    }
    error = "unsupported VST2 chunk '" + kind + "'";
    return std::nullopt;
}

std::vector<std::uint8_t> FxpFile::writeForTests(const std::string& programName, const std::vector<std::uint8_t>& state)
{
    uLongf bound = compressBound(static_cast<uLong>(state.size()));
    std::vector<std::uint8_t> z(bound);
    compress(z.data(), &bound, state.data(), static_cast<uLong>(state.size()));
    z.resize(bound);

    std::vector<std::uint8_t> chunk = z;
    const std::uint32_t trailer = static_cast<std::uint32_t>(z.size());
    for (int i = 0; i < 4; ++i) chunk.push_back(static_cast<std::uint8_t>(trailer >> (8 * i)));

    std::vector<std::uint8_t> out;
    auto putTag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
    auto putBe = [&](std::uint32_t v) { for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i))); };
    putTag("CcnK");
    putBe(static_cast<std::uint32_t>(0x3C + chunk.size() - 8));
    putTag("FPCh");
    putBe(1);
    putTag("XfsX");
    putBe(1);
    putBe(1);
    std::string name = programName.substr(0, 27);
    name.resize(28, '\0');
    out.insert(out.end(), name.begin(), name.end());
    putBe(static_cast<std::uint32_t>(chunk.size()));
    out.insert(out.end(), chunk.begin(), chunk.end());
    return out;
}

} // namespace winerose::presets
