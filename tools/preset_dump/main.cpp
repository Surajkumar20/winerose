// preset_dump — prints what a preset file is and everything its container holds.
//
//   preset_dump [--full] <file>...
//
// Winerose state: the JSON document. .SerumPreset: metadata + the decoded CBOR module map (each module's
// plainParams keys and values; byte strings shown as sizes). With --full the whole CBOR document is printed as
// JSON. .fxp/.fxb: VST2 header, inflated state metadata, extra stream sizes and the (inferred) parameter array.
// Wavetable .wav: frame size/count and the clm flags. This is the tool for learning real Serum key names from
// presets you own — paste its output into an issue/PR to extend presets/SerumTables.h.

#include "presets/FxpFile.h"
#include "presets/PresetFormat.h"
#include "presets/SerumPresetFile.h"
#include "presets/WavetableWav.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace winerose;
using namespace winerose::presets;

namespace {

using Bytes = std::vector<std::uint8_t>;

// Replace byte strings with a "<N bytes>" marker so the dump stays readable.
nlohmann::json summarizeBinary(const nlohmann::json& j)
{
    if (j.is_binary()) return "<" + std::to_string(j.get_binary().size()) + " bytes>";
    if (j.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        for (const auto& [k, v] : j.items()) out[k] = summarizeBinary(v);
        return out;
    }
    if (j.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& v : j) out.push_back(summarizeBinary(v));
        return out;
    }
    return j;
}

bool dumpSerumPreset(const Bytes& b, bool full)
{
    std::string error;
    const auto file = SerumPresetFile::read(b, error);
    if (!file) {
        std::cout << "  error: " << error << "\n";
        return false;
    }
    std::cout << "  container version: " << file->version << "\n  metadata:\n" << file->meta.dump(2) << "\n";
    for (const auto& w : file->warnings) std::cout << "  warning: " << w << "\n";
    if (full) {
        std::cout << "  cbor:\n" << summarizeBinary(file->root).dump(2) << "\n";
        return true;
    }
    if (!file->root.is_object()) {
        std::cout << "  cbor root is not a map\n";
        return true;
    }
    std::cout << "  modules (" << file->root.size() << "):\n";
    for (const auto& [module, body] : file->root.items()) {
        std::cout << "    " << module;
        if (!body.is_object()) { std::cout << " = " << summarizeBinary(body).dump() << "\n"; continue; }
        const auto params = body.find("plainParams");
        if (params != body.end() && params->is_string()) std::cout << "  plainParams: " << params->get<std::string>();
        std::cout << "\n";
        if (params != body.end() && params->is_object())
            for (const auto& [k, v] : params->items()) std::cout << "      " << k << " = " << v.dump() << "\n";
        for (const auto& [k, v] : body.items())
            if (k != "plainParams") std::cout << "      [" << k << "] " << summarizeBinary(v).dump() << "\n";
    }
    return true;
}

bool dumpFxp(const Bytes& b)
{
    std::string error;
    const auto file = FxpFile::read(b, error);
    if (!file) {
        std::cout << "  error: " << error << "\n";
        return false;
    }
    if (file->isBank) std::cout << "  bank with " << file->programs.size() << " program(s)\n";
    for (std::size_t i = 0; i < file->programs.size(); ++i) {
        const auto& p = file->programs[i];
        std::cout << "  program " << i << ": fxID " << p.fxId << (p.isSerum() ? " (Serum)" : "") << "\n"
                  << "    header name: \"" << p.headerName << "\"\n"
                  << "    name:        \"" << p.name << "\"\n"
                  << "    author:      \"" << p.author << "\"\n"
                  << "    category:    \"" << p.category << "\"\n"
                  << "    version:     " << p.stateVersion << "\n"
                  << "    state:       " << p.originalStateSize << " bytes (padded to " << p.state.size() << ")\n";
        for (std::size_t s = 0; s < p.extraStreams.size(); ++s)
            std::cout << "    stream " << (s + 1) << ":    " << p.extraStreams[s].size() << " bytes\n";
        if (!p.params.empty()) {
            std::cout << "    params (INFERRED layout @0x3460):";
            for (std::size_t k = 0; k < p.params.size(); ++k) std::cout << (k % 10 == 0 ? "\n      " : " ") << p.params[k];
            std::cout << "\n";
        }
        for (const auto& w : p.warnings) std::cout << "    warning: " << w << "\n";
    }
    return true;
}

bool dumpWavetable(const Bytes& b)
{
    std::string error;
    const auto wt = Wavetable::read(b, error);
    if (!wt) return false;
    std::cout << "  wavetable: " << wt->frameCount << " frame(s) x " << wt->frameSize << " samples"
              << (wt->hadClm ? ", clm chunk, morph mode " + std::to_string(wt->morphMode) : ", no clm chunk") << "\n";
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    bool full = false;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--full") == 0) full = true;
        else files.emplace_back(argv[i]);
    }
    if (files.empty()) {
        std::cerr << "usage: preset_dump [--full] <file>...\n";
        return 2;
    }
    int failures = 0;
    for (const auto& path : files) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::cerr << path << ": cannot open\n";
            ++failures;
            continue;
        }
        const Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto format = detectPresetFormat(bytes);
        std::cout << path << ": " << presetFormatName(format) << " (" << bytes.size() << " bytes)\n";

        bool ok = true;
        switch (format) {
            case PresetFormat::WineroseState: {
                const auto doc = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
                ok = !doc.is_discarded();
                if (ok) std::cout << doc.dump(2) << "\n";
                break;
            }
            case PresetFormat::SerumPreset: ok = dumpSerumPreset(bytes, full); break;
            case PresetFormat::SerumFxp:
            case PresetFormat::SerumFxb:    ok = dumpFxp(bytes); break;
            case PresetFormat::Unknown:     ok = dumpWavetable(bytes); break;
        }
        if (!ok) ++failures;
    }
    return failures == 0 ? 0 : 1;
}
