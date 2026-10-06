#include "presets/AudioFile.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace winerose::presets {

namespace {

std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t o)
{
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8)
         | (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}
std::uint16_t le16(std::span<const std::uint8_t> b, std::size_t o) { return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8)); }
bool is(std::span<const std::uint8_t> b, std::size_t o, const char* t) { return std::memcmp(b.data() + o, t, 4) == 0; }

} // namespace

std::optional<std::vector<std::uint8_t>> readBytes(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::optional<AudioFile> AudioFile::read(std::span<const std::uint8_t> b, std::string& error)
{
    if (b.size() < 12 || !is(b, 0, "RIFF") || !is(b, 8, "WAVE")) { error = "not a RIFF/WAVE file"; return std::nullopt; }
    std::uint16_t format = 0, channels = 0, bits = 0;
    std::span<const std::uint8_t> data;
    AudioFile out;

    std::size_t pos = 12;
    while (pos + 8 <= b.size()) {
        const std::uint32_t size = le32(b, pos + 4);
        const std::size_t body = pos + 8;
        if (body + size > b.size()) {
            if (is(b, pos, "data")) { data = b.subspan(body); break; }   // tolerate a truncated final data chunk
            error = "truncated chunk";
            return std::nullopt;
        }
        if (is(b, pos, "fmt ") && size >= 16) {
            format = le16(b, body);
            channels = le16(b, body + 2);
            out.sampleRate = static_cast<double>(le32(b, body + 4));
            bits = le16(b, body + 14);
            if (format == 0xFFFE && size >= 26) format = le16(b, body + 24);   // WAVE_FORMAT_EXTENSIBLE
        } else if (is(b, pos, "data")) {
            data = b.subspan(body, size);
        } else if (is(b, pos, "clm ")) {
            out.clm.assign(reinterpret_cast<const char*>(b.data() + body), size);
        } else if (is(b, pos, "smpl") && size >= 36) {
            out.rootKey = static_cast<int>(le32(b, body + 12));
            const std::uint32_t loops = le32(b, body + 28);
            if (loops > 0 && size >= 36 + 24) {
                out.loopStart = le32(b, body + 36 + 8);
                out.loopEnd = static_cast<std::int64_t>(le32(b, body + 36 + 12)) + 1;   // smpl end is inclusive
            }
        }
        pos = body + size + (size & 1u);
    }
    if (channels == 0 || data.empty()) { error = "missing fmt or data chunk"; return std::nullopt; }
    const int bytesPer = bits / 8;
    if (bytesPer <= 0 || (format != 1 && format != 3)) { error = "unsupported sample format " + std::to_string(format) + "/" + std::to_string(bits); return std::nullopt; }
    if ((format == 3 && bits != 32 && bits != 64) || (format == 1 && bits > 32)) { error = "unsupported bit depth " + std::to_string(bits); return std::nullopt; }

    const std::size_t stride = static_cast<std::size_t>(bytesPer) * channels;
    const std::size_t frames = data.size() / stride;
    out.channels.assign(channels, std::vector<float>(frames));
    for (std::size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c) {
            const std::size_t o = i * stride + static_cast<std::size_t>(c * bytesPer);
            float v = 0.0f;
            if (format == 3 && bits == 32) std::memcpy(&v, data.data() + o, 4);
            else if (format == 3) { double d; std::memcpy(&d, data.data() + o, 8); v = static_cast<float>(d); }
            else if (bits == 8) v = (static_cast<int>(data[o]) - 128) / 128.0f;
            else if (bits == 16) v = static_cast<std::int16_t>(le16(data, o)) / 32768.0f;
            else if (bits == 24) {
                std::int32_t s = data[o] | (data[o + 1] << 8) | (data[o + 2] << 16);
                if (s & 0x800000) s |= ~0xFFFFFF;
                v = static_cast<float>(s) / 8388608.0f;
            } else v = static_cast<float>(static_cast<std::int32_t>(le32(data, o))) / 2147483648.0f;
            out.channels[static_cast<std::size_t>(c)][i] = v;
        }
    }
    if (out.loopEnd > static_cast<std::int64_t>(frames)) out.loopEnd = static_cast<std::int64_t>(frames);
    if (out.loopStart >= out.loopEnd) out.loopStart = out.loopEnd = -1;
    return out;
}

std::optional<AudioFile> AudioFile::readFile(const std::filesystem::path& file, std::string& error)
{
    const auto bytes = readBytes(file);
    if (!bytes) { error = "cannot open " + file.string(); return std::nullopt; }
    return read(*bytes, error);
}

} // namespace winerose::presets
