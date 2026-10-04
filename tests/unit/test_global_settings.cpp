#include "params/GlobalSettings.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace winerose;

TEST_CASE("GlobalSettings writes through and reloads", "[params][settings]")
{
    const auto dir  = std::filesystem::temp_directory_path() / "winerose_settings_test";
    const auto file = dir / "nested" / "settings.ini";
    std::filesystem::remove_all(dir);

    {
        GlobalSettings s(file);
        CHECK(s.get<std::string>("serum.presetDir", "none") == "none");
        s.set("serum.presetDir", "C:/Users/me/Documents/Xfer/Serum 2 Presets");
        s.set("ui.scale", 1.25);
        s.set("quality.default", 2);
        s.set("smoothing.disabled", true);
        CHECK(std::filesystem::exists(file));   // write-through: no explicit save()
    }

    GlobalSettings reloaded(file);
    CHECK(reloaded.get<std::string>("serum.presetDir", "") == "C:/Users/me/Documents/Xfer/Serum 2 Presets");
    CHECK(reloaded.get<double>("ui.scale", 0.0) == 1.25);
    CHECK(reloaded.get<int>("quality.default", 0) == 2);
    CHECK(reloaded.get<bool>("smoothing.disabled", false));
    CHECK(reloaded.get<int>("serum.presetDir", -1) == -1);   // unparseable → default
    std::filesystem::remove_all(dir);
}
