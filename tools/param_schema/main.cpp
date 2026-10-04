// param_schema — dumps every engine module's parameter schema without loading the plugin (PLAN.md §3.7).
//
//   param_schema [--out <dir>] [--json <file>] [--state <file>]
//
//   --out   <dir>   write <dir>/<Module>/schema.ini per registry (default: ./params)
//   --json  <file>  also write the full IController::schema() as JSON (what a web UI consumes)
//   --state <file>  also write the default patch as a Winerose state document
//
// Output is deterministic for a given build: the same build always produces identical files.

#include "control/Controller.h"
#include "control/Json.h"
#include "engine/Engine.h"
#include "params/ConfigManager.h"
#include "params/ParamRegistry.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int usage()
{
    std::cerr << "usage: param_schema [--out <dir>] [--json <file>] [--state <file>]\n";
    return 2;
}

bool writeText(const std::filesystem::path& file, const std::string& text)
{
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

} // namespace

int main(int argc, char** argv)
{
    std::filesystem::path outDir = "params";
    std::filesystem::path jsonFile, stateFile;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) return usage();
        if      (arg == "--out")   outDir    = argv[++i];
        else if (arg == "--json")  jsonFile  = argv[++i];
        else if (arg == "--state") stateFile = argv[++i];
        else return usage();
    }

    auto config = std::make_shared<winerose::ConfigManager>();
    winerose::Engine engine(config);
    winerose::control::Controller controller(engine, config);

    int modules = 0, params = 0;
    for (const auto& [name, registry] : config->getAttachedParamRegistries()) {
        if (!config->writeParamSchema(outDir, name, registry->getAll())) {
            std::cerr << "param_schema: failed to write schema for " << name << " under " << outDir.string() << "\n";
            return 1;
        }
        ++modules;
        params += static_cast<int>(registry->getAll().size());
    }

    if (!jsonFile.empty()) {
        const nlohmann::json schema = controller.schema();
        if (!writeText(jsonFile, schema.dump(2) + "\n")) {
            std::cerr << "param_schema: failed to write " << jsonFile.string() << "\n";
            return 1;
        }
    }
    if (!stateFile.empty() && !writeText(stateFile, controller.saveState())) {
        std::cerr << "param_schema: failed to write " << stateFile.string() << "\n";
        return 1;
    }

    std::cout << "param_schema: " << params << " parameters in " << modules << " modules -> " << outDir.string() << "\n";
    return 0;
}
