#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace winerose::presets {

/**
 * @brief A decoded RIFF/WAVE file: PCM 8/16/24/32-bit or float 32/64, any channel count, plus the metadata
 *        samplers use — the "smpl" chunk (root key, first loop) and the "clm " chunk text (wavetables).
 */
struct AudioFile {
    double sampleRate = 44100.0;
    std::vector<std::vector<float>> channels;   // channels[c][frame]
    int rootKey = -1;                           // smpl MIDI unity note, -1 if absent
    std::int64_t loopStart = -1, loopEnd = -1;  // smpl loop 0 (end exclusive), -1 if absent
    std::string clm;                            // raw "clm " chunk text, empty if absent

    std::size_t frames() const { return channels.empty() ? 0 : channels[0].size(); }

    static std::optional<AudioFile> read(std::span<const std::uint8_t> bytes, std::string& error);
    static std::optional<AudioFile> readFile(const std::filesystem::path& file, std::string& error);
};

/** Whole-file read helper (nullopt if the file can't be opened). */
std::optional<std::vector<std::uint8_t>> readBytes(const std::filesystem::path& file);

} // namespace winerose::presets
