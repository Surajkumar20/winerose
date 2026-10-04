#include "presets/WavetableWav.h"

#include <algorithm>
#include <cstdio>
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
std::uint16_t le16(std::span<const std::uint8_t> b, std::size_t o)
{
    return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8));
}
bool is(std::span<const std::uint8_t> b, std::size_t o, const char* t) { return std::memcmp(b.data() + o, t, 4) == 0; }

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
void put16(std::vector<std::uint8_t>& out, std::uint16_t v) { out.push_back(static_cast<std::uint8_t>(v)); out.push_back(static_cast<std::uint8_t>(v >> 8)); }
void putTag(std::vector<std::uint8_t>& out, const char* t) { out.insert(out.end(), t, t + 4); }

} // namespace

std::optional<Wavetable> Wavetable::read(std::span<const std::uint8_t> b, std::string& error)
{
    if (b.size() < 12 || !is(b, 0, "RIFF") || !is(b, 8, "WAVE")) { error = "not a RIFF/WAVE file"; return std::nullopt; }
    std::uint16_t format = 0, channels = 0, bits = 0;
    std::span<const std::uint8_t> data;
    Wavetable wt;
    int clmFrame = 0;

    std::size_t pos = 12;
    while (pos + 8 <= b.size()) {
        const std::uint32_t size = le32(b, pos + 4);
        const std::size_t body = pos + 8;
        if (body + size > b.size()) { error = "truncated chunk"; return std::nullopt; }
        if (is(b, pos, "fmt ") && size >= 16) {
            format = le16(b, body);
            channels = le16(b, body + 2);
            bits = le16(b, body + 14);
            if (format == 0xFFFE && size >= 26) format = le16(b, body + 24);   // WAVE_FORMAT_EXTENSIBLE sub-format
        } else if (is(b, pos, "data")) {
            data = b.subspan(body, size);
        } else if (is(b, pos, "clm ")) {
            const std::string text(reinterpret_cast<const char*>(b.data() + body), size);
            const auto at = text.find("<!>");
            if (at != std::string::npos) {
                wt.hadClm = true;
                std::size_t i = at + 3;
                while (i < text.size() && text[i] >= '0' && text[i] <= '9') clmFrame = clmFrame * 10 + (text[i++] - '0');
                while (i < text.size() && text[i] == ' ') ++i;
                if (i < text.size() && text[i] >= '0' && text[i] <= '9') wt.morphMode = text[i] - '0';
            }
        }
        pos = body + size + (size & 1u);   // chunks are padded to even length
    }
    if (channels == 0 || data.empty()) { error = "missing fmt or data chunk"; return std::nullopt; }

    const int bytesPer = bits / 8;
    if (bytesPer <= 0) { error = "bad bit depth"; return std::nullopt; }
    const std::size_t frames = data.size() / (static_cast<std::size_t>(bytesPer) * channels);
    wt.samples.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t o = i * static_cast<std::size_t>(bytesPer) * channels;   // first channel
        float v = 0.0f;
        if (format == 3 && bits == 32) { std::memcpy(&v, data.data() + o, 4); }
        else if (format == 3 && bits == 64) { double d; std::memcpy(&d, data.data() + o, 8); v = static_cast<float>(d); }
        else if (format == 1 && bits == 8) { v = (static_cast<int>(data[o]) - 128) / 128.0f; }
        else if (format == 1 && bits == 16) { v = static_cast<std::int16_t>(le16(data, o)) / 32768.0f; }
        else if (format == 1 && bits == 24) {
            std::int32_t s = data[o] | (data[o + 1] << 8) | (data[o + 2] << 16);
            if (s & 0x800000) s |= ~0xFFFFFF;
            v = static_cast<float>(s) / 8388608.0f;
        }
        else if (format == 1 && bits == 32) { v = static_cast<float>(static_cast<std::int32_t>(le32(data, o))) / 2147483648.0f; }
        else { error = "unsupported sample format " + std::to_string(format) + "/" + std::to_string(bits); return std::nullopt; }
        wt.samples[i] = v;
    }

    if (clmFrame >= 16 && clmFrame <= 8192) wt.frameSize = clmFrame;
    else if (frames % 2048 == 0 && frames >= 2048) wt.frameSize = 2048;
    else if (frames >= 16 && frames <= 8192) wt.frameSize = static_cast<int>(frames);
    else wt.frameSize = 2048;
    wt.frameCount = static_cast<int>(frames / static_cast<std::size_t>(wt.frameSize));
    if (wt.frameCount == 0) { error = "file shorter than one frame"; return std::nullopt; }
    wt.samples.resize(static_cast<std::size_t>(wt.frameCount) * static_cast<std::size_t>(wt.frameSize));
    return wt;
}

std::optional<Wavetable> Wavetable::readFile(const std::filesystem::path& file, std::string& error)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) { error = "cannot open " + file.string(); return std::nullopt; }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto wt = read(bytes, error);
    if (wt) wt->name = file.stem().string();
    return wt;
}

std::vector<std::uint8_t> Wavetable::write() const
{
    char clmText[64];
    std::snprintf(clmText, sizeof(clmText), "<!>%d %d0000000 wavetable (winerose)", frameSize, std::clamp(morphMode, 0, 9));
    std::string clm(clmText);
    if (clm.size() % 2) clm.push_back(' ');   // even length (RIFF requires it; some readers rely on it)

    const auto dataBytes = static_cast<std::uint32_t>(samples.size() * 4);
    std::vector<std::uint8_t> out;
    putTag(out, "RIFF");
    put32(out, 4 + (8 + 16) + (8 + static_cast<std::uint32_t>(clm.size())) + (8 + dataBytes));
    putTag(out, "WAVE");
    putTag(out, "fmt ");
    put32(out, 16);
    put16(out, 3);          // IEEE float
    put16(out, 1);          // mono
    put32(out, 44100);
    put32(out, 44100 * 4);
    put16(out, 4);
    put16(out, 32);
    putTag(out, "clm ");
    put32(out, static_cast<std::uint32_t>(clm.size()));
    out.insert(out.end(), clm.begin(), clm.end());
    putTag(out, "data");
    put32(out, dataBytes);
    for (float s : samples) {
        std::uint8_t b[4];
        std::memcpy(b, &s, 4);
        out.insert(out.end(), b, b + 4);
    }
    return out;
}

} // namespace winerose::presets
