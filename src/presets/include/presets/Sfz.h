#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace winerose::presets {

/**
 * @brief SFZ text parser (SPEC §1.4 Multisample; SFZ is an open, human-readable format).
 *
 * Produces flattened regions: each region's opcodes merged over its <group>, <master> and <global> (later
 * headers override earlier ones). Handles <control> default_path, #define $NAME value substitution, // and
 * block comments, and values with spaces (sample paths run until the next "name=" token). #include is not
 * followed (reported as a warning). Turning opcodes into playable regions is the control layer's job.
 */
struct SfzFile {
    struct Region {
        std::map<std::string, std::string> opcodes;
        std::string get(const std::string& key, const std::string& fallback = {}) const
        {
            const auto it = opcodes.find(key);
            return it == opcodes.end() ? fallback : it->second;
        }
    };

    std::vector<Region> regions;
    std::string defaultPath;
    std::vector<std::string> warnings;

    static SfzFile parse(const std::string& text);
};

/** SFZ note value: a MIDI number ("60") or a name ("c4", "C#4", "db3"; c4 = 60). nullopt if neither. */
std::optional<int> parseSfzNote(const std::string& value);

} // namespace winerose::presets
