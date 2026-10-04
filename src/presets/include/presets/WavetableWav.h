#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace winerose::presets {

/**
 * @brief Wavetable .wav files (SPEC §1.3): RIFF/WAVE with an optional "clm " chunk whose ASCII payload starts
 *        "<!>2048 01000000 ..." — the digits after "<!>" are the frame size, then eight flag digits (the first is
 *        the interpolation/morph mode, the second the factory flag, which third parties must write as 0).
 *
 * Reading accepts PCM 8/16/24/32-bit and float 32/64, any channel count (the first channel is used). Without
 * a clm chunk the frame size is guessed: 2048 if the length divides evenly, otherwise one frame of the whole
 * file (up to 8192 samples). Writing produces mono 32-bit float, 44.1 kHz, with a clm chunk.
 */
struct Wavetable {
    std::vector<float> samples;   // frameCount × frameSize, concatenated
    int frameSize = 2048;
    int frameCount = 0;
    int morphMode = 0;            // clm flag digit 1
    bool hadClm = false;
    std::string name;

    static std::optional<Wavetable> read(std::span<const std::uint8_t> bytes, std::string& error);
    static std::optional<Wavetable> readFile(const std::filesystem::path& file, std::string& error);
    std::vector<std::uint8_t> write() const;
};

} // namespace winerose::presets
