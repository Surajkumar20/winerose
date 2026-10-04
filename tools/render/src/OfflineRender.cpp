#include "render/OfflineRender.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>

namespace winerose::render {

namespace {

constexpr double kSilenceDb = -300.0;

double toDb(double rms) { return rms > 0.0 ? 20.0 * std::log10(rms) : kSilenceDb; }

template<typename T>
void put(std::ofstream& out, T value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));   // WAV is little-endian; so are our targets
}

template<typename T>
bool get(std::ifstream& in, T& value)
{
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(T)));
}

} // namespace

AudioData renderOffline(Engine& engine, const RenderSpec& spec)
{
    const int totalFrames = static_cast<int>(std::lround(spec.seconds * spec.sampleRate));
    AudioData audio(spec.numChannels, totalFrames, spec.sampleRate);

    engine.prepare(spec.sampleRate, spec.blockSize);

    std::vector<MidiEvent> events = spec.events;
    std::stable_sort(events.begin(), events.end(),
                     [](const MidiEvent& a, const MidiEvent& b) { return a.sampleOffset < b.sampleOffset; });

    std::vector<MidiEvent> blockEvents;
    blockEvents.reserve(events.size());
    std::vector<float*> channels(static_cast<std::size_t>(spec.numChannels));
    std::size_t next = 0;

    for (int start = 0; start < totalFrames; start += spec.blockSize) {
        const int n = std::min(spec.blockSize, totalFrames - start);
        blockEvents.clear();
        while (next < events.size() && events[next].sampleOffset < start + n) {
            MidiEvent e = events[next++];
            e.sampleOffset = std::max(0, e.sampleOffset - start);
            blockEvents.push_back(e);
        }
        for (int c = 0; c < spec.numChannels; ++c) channels[static_cast<std::size_t>(c)] = audio.channel(c) + start;
        engine.process(channels.data(), spec.numChannels, n,
                       blockEvents.data(), static_cast<int>(blockEvents.size()), spec.transport);
    }
    return audio;
}

MidiEvent noteOn(int sampleOffset, int note, int velocity, int channel)
{
    MidiEvent e;
    e.sampleOffset = sampleOffset;
    e.data[0] = static_cast<std::uint8_t>(0x90 | (channel & 0x0f));
    e.data[1] = static_cast<std::uint8_t>(note & 0x7f);
    e.data[2] = static_cast<std::uint8_t>(velocity & 0x7f);
    e.size = 3;
    return e;
}

MidiEvent noteOff(int sampleOffset, int note, int channel)
{
    MidiEvent e;
    e.sampleOffset = sampleOffset;
    e.data[0] = static_cast<std::uint8_t>(0x80 | (channel & 0x0f));
    e.data[1] = static_cast<std::uint8_t>(note & 0x7f);
    e.data[2] = 0;
    e.size = 3;
    return e;
}

bool writeWav(const std::filesystem::path& file, const AudioData& audio)
{
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    const auto channels   = static_cast<std::uint16_t>(audio.numChannels);
    const auto rate       = static_cast<std::uint32_t>(std::lround(audio.sampleRate));
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(audio.samples.size() * sizeof(float));

    out.write("RIFF", 4);
    put<std::uint32_t>(out, 4 + (8 + 16) + (8 + dataBytes));
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    put<std::uint32_t>(out, 16);
    put<std::uint16_t>(out, 3);                                   // WAVE_FORMAT_IEEE_FLOAT
    put<std::uint16_t>(out, channels);
    put<std::uint32_t>(out, rate);
    put<std::uint32_t>(out, rate * channels * 4u);                // byte rate
    put<std::uint16_t>(out, static_cast<std::uint16_t>(channels * 4u));
    put<std::uint16_t>(out, 32);
    out.write("data", 4);
    put<std::uint32_t>(out, dataBytes);
    for (int i = 0; i < audio.numFrames; ++i)
        for (int c = 0; c < audio.numChannels; ++c)
            put<float>(out, audio.channel(c)[i]);
    return static_cast<bool>(out);
}

std::optional<AudioData> readWav(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;

    char id[4];
    std::uint32_t size = 0;
    if (!in.read(id, 4) || std::memcmp(id, "RIFF", 4) != 0) return std::nullopt;
    get(in, size);
    if (!in.read(id, 4) || std::memcmp(id, "WAVE", 4) != 0) return std::nullopt;

    std::uint16_t format = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0;
    bool haveFmt = false;
    while (in.read(id, 4) && get(in, size)) {
        if (std::memcmp(id, "fmt ", 4) == 0) {
            std::uint32_t byteRate = 0;
            std::uint16_t align = 0;
            get(in, format); get(in, channels); get(in, rate); get(in, byteRate); get(in, align); get(in, bits);
            if (size > 16) in.seekg(size - 16, std::ios::cur);
            haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            if (!haveFmt || format != 3 || bits != 32 || channels == 0) return std::nullopt;
            const int frames = static_cast<int>(size / (4u * channels));
            AudioData audio(channels, frames, rate);
            for (int i = 0; i < frames; ++i)
                for (int c = 0; c < channels; ++c)
                    if (!get(in, audio.channel(c)[i])) return std::nullopt;
            return audio;
        } else {
            in.seekg(size + (size & 1u), std::ios::cur);   // RIFF chunks are padded to even length
        }
    }
    return std::nullopt;
}

double rmsDb(const AudioData& audio)
{
    if (audio.samples.empty()) return kSilenceDb;
    double sum = 0.0;
    for (float s : audio.samples) sum += static_cast<double>(s) * s;
    return toDb(std::sqrt(sum / static_cast<double>(audio.samples.size())));
}

double differenceDb(const AudioData& a, const AudioData& b)
{
    if (a.numChannels != b.numChannels || a.numFrames != b.numFrames) return -kSilenceDb;
    if (a.samples.empty()) return kSilenceDb;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.samples.size(); ++i) {
        const double d = static_cast<double>(a.samples[i]) - b.samples[i];
        sum += d * d;
    }
    return toDb(std::sqrt(sum / static_cast<double>(a.samples.size())));
}

} // namespace winerose::render
