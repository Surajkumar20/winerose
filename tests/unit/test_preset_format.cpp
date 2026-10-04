#include "presets/PresetFormat.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace winerose;

namespace {
std::vector<std::uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }
}

TEST_CASE("container formats are detected by magic", "[presets]")
{
    auto serum = bytes(std::string("XferJson\0", 9));
    serum.resize(40, 0);
    CHECK(detectPresetFormat(serum) == PresetFormat::SerumPreset);

    auto truncated = bytes(std::string("XferJson\0", 9));
    CHECK(detectPresetFormat(truncated) == PresetFormat::Unknown);

    auto fxp = bytes(std::string("CcnK\0\0\0\0FPCh", 12));
    CHECK(detectPresetFormat(fxp) == PresetFormat::SerumFxp);
    auto fxb = bytes(std::string("CcnK\0\0\0\0FBCh", 12));
    CHECK(detectPresetFormat(fxb) == PresetFormat::SerumFxb);

    CHECK(detectPresetFormat(bytes(R"(  {"format":"winerose.state","version":1,"values":{}})")) == PresetFormat::WineroseState);
    CHECK(detectPresetFormat(bytes(R"({"something":"else"})")) == PresetFormat::Unknown);
    CHECK(detectPresetFormat({}) == PresetFormat::Unknown);
}
