#pragma once

#include "engine/dsp/Multisample.h"
#include "engine/dsp/SampleData.h"
#include "engine/dsp/WavetableBank.h"
#include "presets/Sfz.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace winerose::control {

// File → engine asset conversion (message thread). Lives in the control layer because it is the only layer
// that sees both the file readers (presets) and the engine's asset types.

std::shared_ptr<const dsp::SampleData> loadSampleFile(const std::filesystem::path& file, std::string& error);

/** True if the .wav is a wavetable (clm chunk). */
bool isWavetableFile(const std::filesystem::path& file);

std::shared_ptr<const dsp::WavetableBank> loadWavetableFile(const std::filesystem::path& file, std::string& error);

/** SFZ → Multisample: parses, resolves sample paths (default_path, relative to the .sfz), decodes each
 *  sample once. Regions whose sample can't be loaded are skipped and reported in `warnings`. */
std::shared_ptr<const dsp::Multisample> loadSfzFile(const std::filesystem::path& file, std::string& error,
                                                    std::vector<std::string>& warnings);

/** Same, from SFZ text with sample paths relative to `baseDir` (tests). */
std::shared_ptr<const dsp::Multisample> buildMultisample(const presets::SfzFile& sfz, const std::filesystem::path& baseDir,
                                                         std::vector<std::string>& warnings);

} // namespace winerose::control
