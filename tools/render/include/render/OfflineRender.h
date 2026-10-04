#pragma once

#include "engine/Engine.h"
#include "engine/EngineTypes.h"

#include <filesystem>
#include <optional>
#include <vector>

namespace winerose::render {

/** Planar float audio owned in one allocation. */
struct AudioData {
    int                numChannels = 0;
    int                numFrames   = 0;
    double             sampleRate  = 48000.0;
    std::vector<float> samples;   // channel-major: channel c occupies [c*numFrames, (c+1)*numFrames)

    AudioData() = default;
    AudioData(int channels, int frames, double rate)
        : numChannels(channels), numFrames(frames), sampleRate(rate),
          samples(static_cast<std::size_t>(channels) * static_cast<std::size_t>(frames), 0.0f) {}

    float*       channel(int c)       { return samples.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(numFrames); }
    const float* channel(int c) const { return samples.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(numFrames); }
};

struct RenderSpec {
    double sampleRate  = 48000.0;
    int    blockSize   = 512;
    int    numChannels = 2;
    double seconds     = 1.0;
    std::vector<MidiEvent> events;   // sampleOffset is ABSOLUTE from the start of the render; any order
    TransportInfo transport;
};

/**
 * @brief Drive an Engine offline exactly as a host would: prepare(), then process() block by block with
 *        each MIDI event delivered in the block that contains it. No JUCE — used by golden tests, the
 *        measurement tooling and benchmarks.
 */
AudioData renderOffline(Engine& engine, const RenderSpec& spec);

/** MIDI helpers producing events for RenderSpec::events. */
MidiEvent noteOn(int sampleOffset, int note, int velocity = 100, int channel = 0);
MidiEvent noteOff(int sampleOffset, int note, int channel = 0);

/** 32-bit float WAV (WAVE_FORMAT_IEEE_FLOAT), interleaved on disk. */
bool writeWav(const std::filesystem::path& file, const AudioData& audio);
std::optional<AudioData> readWav(const std::filesystem::path& file);

/** RMS level in dBFS over all channels (-300 for digital silence). */
double rmsDb(const AudioData& audio);

/** RMS level of (a - b) in dBFS (-300 if identical). Shapes must match, else returns +300. */
double differenceDb(const AudioData& a, const AudioData& b);

} // namespace winerose::render
