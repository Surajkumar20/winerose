#include "presets/WavetableWav.h"
#include "presets/AudioFile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace winerose::presets {

namespace {

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
void put16(std::vector<std::uint8_t>& out, std::uint16_t v) { out.push_back(static_cast<std::uint8_t>(v)); out.push_back(static_cast<std::uint8_t>(v >> 8)); }
void putTag(std::vector<std::uint8_t>& out, const char* t) { out.insert(out.end(), t, t + 4); }

} // namespace

std::optional<Wavetable> Wavetable::read(std::span<const std::uint8_t> b, std::string& error)
{
    const auto audio = AudioFile::read(b, error);
    if (!audio) return std::nullopt;
    Wavetable wt;
    int clmFrame = 0;
    const auto at = audio->clm.find("<!>");
    if (at != std::string::npos) {
        wt.hadClm = true;
        const std::string& text = audio->clm;
        std::size_t i = at + 3;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') clmFrame = clmFrame * 10 + (text[i++] - '0');
        while (i < text.size() && text[i] == ' ') ++i;
        if (i < text.size() && text[i] >= '0' && text[i] <= '9') wt.morphMode = text[i] - '0';
    }
    wt.samples = audio->channels[0];   // first channel
    const std::size_t frames = wt.samples.size();

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
