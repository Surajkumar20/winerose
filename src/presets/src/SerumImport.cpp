#include "presets/SerumImport.h"
#include "presets/SerumTables.h"

#include "params/ParamRegistry.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace winerose::presets {

namespace {

std::string normalizeKey(std::string_view key)
{
    std::string s;
    for (char c : key)
        if (std::isalnum(static_cast<unsigned char>(c))) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (s.rfind("kparam", 0) == 0) s.erase(0, 6);
    return s;
}

// "Oscillator2" → {"Oscillator", "2"} for a known family; empty family otherwise.
std::pair<std::string, std::string> splitFamily(const std::string& module)
{
    for (const auto& f : serum::kFamilies) {
        if (module.rfind(f.serumPrefix, 0) != 0) continue;
        const std::string suffix = module.substr(f.serumPrefix.size());
        if (!suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            return {std::string(f.serumPrefix), suffix};
    }
    return {};
}

std::string numberText(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

bool endsWithWav(const std::string& s)
{
    if (s.size() < 4) return false;
    std::string ext = s.substr(s.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".wav";
}

void jsonEscape(std::string& out, const std::string& s)
{
    out.push_back('"');
    for (char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out.push_back(c);
    }
    out.push_back('"');
}

} // namespace

// --- Report ----------------------------------------------------------------------------------------------

std::string ImportReport::summary() const
{
    std::string s = "Imported " + format;
    if (!name.empty()) s += " '" + name + "'";
    if (!author.empty()) s += " by " + author;
    s += ": " + std::to_string(mappedExplicit) + " mapped from the table, " + std::to_string(mappedBySynonym)
       + " by name (unverified), " + std::to_string(unmapped.size()) + " not yet mapped (kept in the patch)";
    if (!wavetables.empty()) s += ", " + std::to_string(wavetables.size()) + " wavetable(s)";
    if (!warnings.empty()) s += ", " + std::to_string(warnings.size()) + " warning(s)";
    return s;
}

std::string ImportReport::toJson() const
{
    std::string out = "{\"format\":";
    jsonEscape(out, format);
    out += ",\"name\":"; jsonEscape(out, name);
    out += ",\"author\":"; jsonEscape(out, author);
    out += ",\"mappedExplicit\":" + std::to_string(mappedExplicit);
    out += ",\"mappedBySynonym\":" + std::to_string(mappedBySynonym);
    out += ",\"defaultsModules\":" + std::to_string(defaultsModules);
    auto list = [&](const char* key, const std::vector<std::string>& v) {
        out += ",\""; out += key; out += "\":[";
        for (std::size_t i = 0; i < v.size(); ++i) { if (i) out += ','; jsonEscape(out, v[i]); }
        out += ']';
    };
    list("mapped", mapped);
    list("unmapped", unmapped);
    list("warnings", warnings);
    out += ",\"wavetables\":[";
    for (std::size_t i = 0; i < wavetables.size(); ++i) {
        if (i) out += ',';
        out += "{\"oscillator\":" + std::to_string(wavetables[i].oscillator) + ",\"frames\":" + std::to_string(wavetables[i].table.frameCount) + ",\"source\":";
        jsonEscape(out, wavetables[i].source);
        out += '}';
    }
    out += "]}";
    return out;
}

// --- Importer --------------------------------------------------------------------------------------------

SerumImporter::SerumImporter(std::shared_ptr<ConfigManager> config, std::vector<std::filesystem::path> assetRoots)
    : m_config(std::move(config)), m_roots(std::move(assetRoots)) {}

std::filesystem::path SerumImporter::resolveAsset(const std::string& reference) const
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path ref(reference);
    if (ref.is_absolute() && fs::is_regular_file(ref, ec)) return ref;
    for (const auto& root : m_roots) {
        if (fs::is_regular_file(root / ref, ec)) return root / ref;
        if (fs::is_regular_file(root / ref.filename(), ec)) return root / ref.filename();
    }
    // Bounded recursive search by file name (user content folders can be large).
    const auto wanted = ref.filename();
    for (const auto& root : m_roots) {
        if (!fs::is_directory(root, ec)) continue;
        int visited = 0;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && visited < 20000; it.increment(ec), ++visited) {
            if (ec) break;
            if (it.depth() > 6) { it.disable_recursion_pending(); continue; }
            if (it->is_regular_file(ec) && it->path().filename() == wanted) return it->path();
        }
    }
    return {};
}

void SerumImporter::collectWavetables(const std::string& module, const nlohmann::json& body, ImportReport& report)
{
    const auto [family, suffix] = splitFamily(module);
    if (family != "Oscillator") return;
    const int osc = std::stoi(suffix);
    if (osc > 2) return;   // A/B/C only
    for (const auto& w : report.wavetables) if (w.oscillator == osc) return;

    // Depth-first: an embedded table (byte string that parses as WAV) wins, else a referenced .wav path.
    // The exact CBOR keys are unknown (SPEC §5.10 Q5), so every value is inspected. TODO-MEASURE.
    std::vector<const nlohmann::json*> stack {&body};
    std::string path;
    while (!stack.empty()) {
        const auto* j = stack.back();
        stack.pop_back();
        if (j->is_binary()) {
            const auto& bin = j->get_binary();
            std::string error;
            if (auto wt = Wavetable::read(std::span<const std::uint8_t>(bin.data(), bin.size()), error)) {
                report.wavetables.push_back({osc, std::move(*wt), "embedded"});
                return;
            }
        } else if (j->is_string()) {
            const auto s = j->get<std::string>();
            if (path.empty() && endsWithWav(s)) path = s;
        } else if (j->is_object() || j->is_array()) {
            for (const auto& child : *j) stack.push_back(&child);
        }
    }
    if (path.empty()) return;
    const auto file = resolveAsset(path);
    if (file.empty()) {
        report.warnings.push_back(module + ": wavetable '" + path + "' not found (set your Serum folders in the settings)");
        return;
    }
    std::string error;
    if (auto wt = Wavetable::readFile(file, error)) report.wavetables.push_back({osc, std::move(*wt), file.string()});
    else report.warnings.push_back(module + ": cannot read '" + file.string() + "': " + error);
}

void SerumImporter::importModule(const std::string& module, const nlohmann::json& body, ImportReport& report)
{
    const auto [family, suffix] = splitFamily(module);
    ParamRegistry* registry = nullptr;
    if (!family.empty()) {
        for (const auto& f : serum::kFamilies)
            if (f.serumPrefix == family) registry = m_config->findParamRegistry(std::string(f.wineroseRegistry) + suffix);
    }

    if (!body.is_object()) return;
    const auto params = body.find("plainParams");
    if (params == body.end()) return;
    if (params->is_string() && params->get<std::string>() == "default") {
        ++report.defaultsModules;   // registries were reset to defaults before the import
        return;
    }
    if (!params->is_object()) return;

    for (const auto& [key, value] : params->items()) {
        if (!value.is_number() && !value.is_boolean()) {
            if (value.is_string()) m_config->set<std::string>("Serum2." + module + "." + key, value.get<std::string>());
            continue;
        }
        const double v = value.is_boolean() ? (value.get<bool>() ? 1.0 : 0.0) : value.get<double>();

        std::string target;
        serum::Transform transform = serum::Transform::Identity;
        bool fromTable = false;
        if (registry != nullptr) {
            for (const auto& e : serum::kExplicit) {
                if (e.key.empty() || e.key != key) continue;
                if (e.module != "*" && e.module != module) continue;
                target = std::string(e.target);
                transform = e.transform;
                fromTable = true;
                break;
            }
            if (target.empty()) {
                const std::string norm = normalizeKey(key);
                for (const auto& syn : serum::kSynonyms)
                    if (syn.family == family && syn.key == norm) { target = std::string(syn.target); break; }
            }
        }

        if (!target.empty() && registry->find(target).has_value()) {
            const double plain = transform == serum::Transform::Normalized ? registry->fromNormalized(target, v) : v;
            registry->set<double>(target, plain);
            if (fromTable) ++report.mappedExplicit;
            else           ++report.mappedBySynonym;
            report.mapped.push_back(module + "." + key + " -> " + registry->namespacedKey(target) + (fromTable ? "" : " (by name, unverified)"));
        } else {
            // Preserved verbatim (as text, so a later import can clear it) — lossless even when unmapped.
            m_config->set<std::string>("Serum2." + module + "." + key, numberText(v));
            report.unmapped.push_back(module + "." + key);
        }
    }
}

ImportReport SerumImporter::importSerum2(const SerumPresetFile& file)
{
    ImportReport report;
    report.format = file.isProcessorState() ? "Serum 2 state" : ".SerumPreset";
    report.name = file.presetName();
    report.author = file.author();
    report.warnings = file.warnings;
    m_config->erasePrefix("Serum2.");
    m_config->erasePrefix("Serum1.");
    if (!report.name.empty()) m_config->set<std::string>("Serum2.meta.presetName", report.name);

    if (!file.root.is_object()) {
        report.warnings.push_back("CBOR root is not a map");
        return report;
    }
    for (const auto& [module, body] : file.root.items()) {
        importModule(module, body, report);
        collectWavetables(module, body, report);
    }
    return report;
}

ImportReport SerumImporter::importSerum1(const FxpProgram& program)
{
    ImportReport report;
    report.format = ".fxp";
    report.name = program.name.empty() ? program.headerName : program.name;
    report.author = program.author;
    report.category = program.category;
    report.warnings = program.warnings;
    m_config->erasePrefix("Serum2.");
    m_config->erasePrefix("Serum1.");
    if (!program.isSerum()) report.warnings.push_back("not a Serum program; nothing mapped");

    if (!program.params.empty()) {
        // INFERRED (low-reputation source, SPEC §2.2): 45 = filter cutoff, 46 = resonance, normalized 0..1.
        if (auto* filter = m_config->findParamRegistry("Filter0")) {
            filter->set<double>("cutoff", filter->fromNormalized("cutoff", program.params[45]));
            filter->set<double>("resonance", program.params[46]);
            report.mappedBySynonym += 2;
            report.mapped.push_back("param 45 -> Filter0.cutoff (unverified)");
            report.mapped.push_back("param 46 -> Filter0.resonance (unverified)");
        }
        for (std::size_t i = 0; i < program.params.size(); ++i) {
            if (i == 45 || i == 46) continue;
            m_config->set<std::string>("Serum1.param" + std::to_string(i), numberText(program.params[i]));
            report.unmapped.push_back("param " + std::to_string(i));
        }
    } else {
        report.warnings.push_back("state too short for the (inferred) parameter array");
    }

    // Embedded wavetables: one observed file has a 136-byte header before float32 frames (TODO-MEASURE).
    constexpr std::size_t kHeader = 136;
    for (std::size_t s = 0; s < program.extraStreams.size() && report.wavetables.size() < 2; ++s) {
        const auto& st = program.extraStreams[s];
        if (st.size() <= kHeader || (st.size() - kHeader) % (2048 * 4) != 0) continue;
        Wavetable wt;
        wt.frameSize = 2048;
        wt.frameCount = static_cast<int>((st.size() - kHeader) / (2048 * 4));
        wt.samples.resize(static_cast<std::size_t>(wt.frameCount) * 2048);
        std::memcpy(wt.samples.data(), st.data() + kHeader, wt.samples.size() * 4);
        report.wavetables.push_back({static_cast<int>(report.wavetables.size()), std::move(wt), "embedded (unverified layout)"});
    }
    return report;
}

} // namespace winerose::presets
