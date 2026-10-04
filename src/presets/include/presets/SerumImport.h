#pragma once

#include "presets/FxpFile.h"
#include "presets/SerumPresetFile.h"
#include "presets/WavetableWav.h"

#include "params/ConfigManager.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace winerose::presets {

/** A wavetable the import wants installed on oscillator A/B/C (the caller hands it to the engine). */
struct WavetableAssignment {
    int oscillator = 0;
    Wavetable table;
    std::string source;   // "embedded", or the resolved file path
};

/** What an import did — always returned, so users and developers can see how faithful it was. */
struct ImportReport {
    std::string format;                    // ".SerumPreset", ".fxp", ...
    std::string name, author, category;
    int mappedExplicit = 0;                // via VERIFIED/INFERRED table rows
    int mappedBySynonym = 0;               // via name guesses (INFERRED)
    int defaultsModules = 0;               // modules stored as "default"
    std::vector<std::string> mapped;       // "Oscillator0.kParamVolume -> Oscillator0.level (synonym)"
    std::vector<std::string> unmapped;     // "Oscillator0.kParamFoo" (preserved in the state as Serum2.*)
    std::vector<std::string> warnings;
    std::vector<WavetableAssignment> wavetables;

    std::string summary() const;
    std::string toJson() const;            // for UIs / the control layer
};

/**
 * @brief Maps decoded Serum presets onto Winerose's registries (SPEC §5.6). Writes plain values through the
 *        ParamRegistries found on the ConfigManager; never touches the engine. The caller wraps the call in a
 *        ConfigManager batch and resets registries to defaults first (Controller::loadPreset does both).
 *
 * Unknown keys are kept as ConfigManager values under "Serum2.<module>.<key>" (and "Serum1.param<N>") so they
 * survive in the Winerose state — import is lossless even where the mapping is not yet known (SPEC §5.6:
 * "Preserve unknown keys").
 */
class SerumImporter {
public:
    SerumImporter(std::shared_ptr<ConfigManager> config, std::vector<std::filesystem::path> assetRoots);

    ImportReport importSerum2(const SerumPresetFile& file);
    ImportReport importSerum1(const FxpProgram& program);

    /** AssetResolver (SPEC §5.6): absolute path, then each root + path, then each root + file name, then a
     *  bounded recursive search for the file name. Empty if not found. */
    std::filesystem::path resolveAsset(const std::string& reference) const;

private:
    void importModule(const std::string& module, const nlohmann::json& body, ImportReport& report);
    void collectWavetables(const std::string& module, const nlohmann::json& body, ImportReport& report);

    std::shared_ptr<ConfigManager> m_config;
    std::vector<std::filesystem::path> m_roots;
};

} // namespace winerose::presets
