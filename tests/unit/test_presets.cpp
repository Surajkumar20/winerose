// Phase 6 (SPEC §5.4, §5.6): Serum 2 container, Serum 1 .fxp, wavetable .wav, mapping and lossless import.

#include "control/Controller.h"
#include "engine/Engine.h"
#include "params/ConfigManager.h"
#include "params/ParamRegistry.h"
#include "presets/FxpFile.h"
#include "presets/PresetFormat.h"
#include "presets/SerumImport.h"
#include "presets/SerumPresetFile.h"
#include "presets/WavetableWav.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace winerose;
using namespace winerose::presets;
using Catch::Approx;

namespace {

// A synthetic Serum-2-shaped document: real container, plausible module/key layout (key names are guesses).
SerumPresetFile makeSerum2(const nlohmann::json& root)
{
    SerumPresetFile f;
    f.meta = {{"fileType", "SerumPreset"}, {"presetName", "Test Lead"}, {"presetAuthor", "Winerose Tests"}, {"tags", {"lead"}}};
    f.root = root;
    f.version = 2;
    return f;
}

std::vector<float> sawFrames(int frames, int size)
{
    std::vector<float> s(static_cast<std::size_t>(frames * size));
    for (int f = 0; f < frames; ++f)
        for (int i = 0; i < size; ++i) s[static_cast<std::size_t>(f * size + i)] = (1.0f - 2.0f * (i + 0.5f) / size) * (1.0f - 0.1f * f);
    return s;
}

struct Fixture {
    std::shared_ptr<ConfigManager> cm = std::make_shared<ConfigManager>();
    Engine engine { cm };
    control::Controller ctl { engine, cm };
};

} // namespace

// --- .SerumPreset container ------------------------------------------------------------------------------

TEST_CASE(".SerumPreset round-trips through the XferJson + zstd + CBOR container", "[presets][serum2]")
{
    const auto original = makeSerum2({{"Oscillator0", {{"plainParams", {{"kParamVolume", 0.5}}}}}, {"Env0", {{"plainParams", "default"}}}});
    const auto bytes = original.write();
    CHECK(detectPresetFormat(bytes) == PresetFormat::SerumPreset);
    CHECK(std::memcmp(bytes.data(), "XferJson\0", 9) == 0);

    std::string error;
    const auto back = SerumPresetFile::read(bytes, error);
    REQUIRE(back.has_value());
    CHECK(back->presetName() == "Test Lead");
    CHECK(back->author() == "Winerose Tests");
    CHECK(back->version == 2);
    CHECK(back->root == original.root);
    CHECK(back->warnings.empty());
}

TEST_CASE("unreadable .SerumPreset data is rejected with a reason", "[presets][serum2]")
{
    std::string error;
    std::vector<std::uint8_t> junk(40, 0);
    CHECK_FALSE(SerumPresetFile::read(junk, error).has_value());
    auto bytes = makeSerum2({{"A", 1}}).write();
    bytes.resize(bytes.size() - 5);   // truncated zstd frame
    CHECK_FALSE(SerumPresetFile::read(bytes, error).has_value());
    CHECK(error.find("zstd") != std::string::npos);
}

TEST_CASE("CBOR byte strings survive as binary (embedded assets)", "[presets][serum2]")
{
    nlohmann::json root;
    root["Oscillator1"]["blob"] = nlohmann::json::binary({1, 2, 3, 4});
    std::string error;
    const auto back = SerumPresetFile::read(makeSerum2(root).write(), error);
    REQUIRE(back.has_value());
    REQUIRE(back->root["Oscillator1"]["blob"].is_binary());
    CHECK(back->root["Oscillator1"]["blob"].get_binary().size() == 4);
}

// --- Import / mapping -----------------------------------------------------------------------------------

TEST_CASE("Serum 2 import maps known names, keeps unknown keys, and reports both", "[presets][import]")
{
    Fixture f;
    const auto file = makeSerum2({
        {"Oscillator0", {{"plainParams", {{"kParamVolume", 0.4}, {"kParamCoarsePitch", 7.0}, {"kParamMysteryKnob", 0.33}}}}},
        {"Oscillator1", {{"plainParams", {{"kParamEnable", 1.0}, {"kParamUnison", 5.0}}}}},
        {"Env0", {{"plainParams", {{"kParamAttack", 0.25}, {"kParamRelease", 1.5}}}}},
        {"Macro2", {{"plainParams", {{"kParamValue", 0.8}}}}},
        {"ModSlot3", {{"plainParams", {{"destModuleID", 5.0}, {"kParamAmount", 0.5}}}}},
        {"LFO0", {{"plainParams", "default"}}},
    });
    const auto result = f.ctl.loadPreset(file.write());
    REQUIRE(result.ok);
    INFO(result.message);
    CHECK(result.message.find("Test Lead") != std::string::npos);

    CHECK(f.ctl.get("Oscillator0.level").number() == Approx(0.4));
    CHECK(f.ctl.get("Oscillator0.coarse").number() == Approx(7.0));
    CHECK(f.ctl.get("Oscillator1.enabled").number() == 1.0);
    CHECK(f.ctl.get("Oscillator1.unison").number() == 5.0);
    CHECK(f.ctl.get("Env0.attack").number() == Approx(0.25));
    CHECK(f.ctl.get("Macro2.value").number() == Approx(0.8));

    // Unknown keys are preserved verbatim in the state (lossless import).
    const std::string state = f.ctl.saveState();
    CHECK(state.find("Serum2.Oscillator0.kParamMysteryKnob") != std::string::npos);
    CHECK(state.find("Serum2.ModSlot3.destModuleID") != std::string::npos);

    const auto report = nlohmann::json::parse(f.ctl.importReport());
    CHECK(report["mappedBySynonym"].get<int>() == 7);
    CHECK(report["unmapped"].size() == 3);
    CHECK(report["defaultsModules"].get<int>() == 1);
}

TEST_CASE("an import starts from defaults and clears the previous import's leftovers", "[presets][import]")
{
    Fixture f;
    f.ctl.set("Filter0.cutoff", 1234.0);
    REQUIRE(f.ctl.loadPreset(makeSerum2({{"Oscillator0", {{"plainParams", {{"kParamOddKey", 1.0}}}}}}).write()).ok);
    CHECK(f.ctl.get("Filter0.cutoff").number() == Approx(425.0));   // reset to the default first
    REQUIRE(f.ctl.loadPreset(makeSerum2({{"Oscillator0", {{"plainParams", {{"kParamOtherKey", 1.0}}}}}}).write()).ok);
    const auto state = f.ctl.saveState();
    CHECK(state.find("kParamOddKey") == std::string::npos);
    CHECK(state.find("kParamOtherKey") != std::string::npos);
}

TEST_CASE("Serum import -> own export -> import gives an identical patch (SPEC Phase 6)", "[presets][import]")
{
    Fixture a;
    const auto file = makeSerum2({
        {"Oscillator0", {{"plainParams", {{"kParamVolume", 0.6}, {"kParamUnknownA", 0.25}}}}},
        {"FXRack0", {{"plainParams", {{"kParamWhatever", 3.0}}}}},
    });
    REQUIRE(a.ctl.loadPreset(file.write()).ok);
    const std::string exported = a.ctl.saveState();

    Fixture b;
    const std::vector<std::uint8_t> bytes(exported.begin(), exported.end());
    REQUIRE(b.ctl.loadPreset(bytes).ok);
    CHECK(b.ctl.saveState() == exported);
}

TEST_CASE("embedded and referenced wavetables are installed on the oscillators", "[presets][import][wavetable]")
{
    const auto dir = std::filesystem::temp_directory_path() / "winerose_assets";
    std::filesystem::create_directories(dir / "Tables");
    Wavetable onDisk;
    onDisk.samples = sawFrames(4, 2048);
    onDisk.frameCount = 4;
    {
        const auto bytes = onDisk.write();
        std::ofstream out(dir / "Tables" / "MySaw.wav", std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    Wavetable embedded;
    embedded.samples = sawFrames(2, 2048);
    embedded.frameCount = 2;
    const auto embeddedBytes = embedded.write();

    nlohmann::json root;
    root["Oscillator0"]["plainParams"] = {{"kParamVolume", 0.7}};
    root["Oscillator0"]["somePathKey"] = "C:/Users/someone/Documents/Xfer/Serum 2 Presets/Tables/MySaw.wav";
    root["Oscillator1"]["plainParams"] = {{"kParamEnable", 1.0}};
    root["Oscillator1"]["someDataKey"] = nlohmann::json::binary(std::vector<std::uint8_t>(embeddedBytes.begin(), embeddedBytes.end()));

    Fixture f;
    f.ctl.setAssetSearchPaths({dir});
    const auto result = f.ctl.loadPreset(makeSerum2(root).write());
    REQUIRE(result.ok);
    const auto report = nlohmann::json::parse(f.ctl.importReport());
    REQUIRE(report["wavetables"].size() == 2);
    std::filesystem::remove_all(dir);
}

TEST_CASE("missing referenced wavetables produce a warning, not a failure", "[presets][import][wavetable]")
{
    nlohmann::json root;
    root["Oscillator0"]["plainParams"] = {{"kParamVolume", 0.7}};
    root["Oscillator0"]["path"] = "Tables/DoesNotExist.wav";
    Fixture f;
    const auto result = f.ctl.loadPreset(makeSerum2(root).write());
    REQUIRE(result.ok);
    CHECK(nlohmann::json::parse(f.ctl.importReport())["warnings"].size() == 1);
}

// --- Serum 1 .fxp ----------------------------------------------------------------------------------------

TEST_CASE(".fxp: header, zlib state, metadata offsets and zero-padding", "[presets][fxp]")
{
    std::vector<std::uint8_t> state(28232, 0);   // an older, shorter state layout
    auto putString = [&](std::size_t at, const char* s) { std::memcpy(state.data() + at, s, std::strlen(s)); };
    putString(0x4972, "Old Bass");
    const float version = 0.1531f;
    std::memcpy(state.data() + 0x4994, &version, 4);
    putString(0x49A0, "Someone");
    putString(0x49D0, "Bass");
    float params[300] = {};
    params[45] = 0.25f;   // (inferred) cutoff
    params[46] = 0.6f;    // (inferred) resonance
    std::memcpy(state.data() + 0x3460, params, sizeof(params));

    const auto bytes = FxpFile::writeForTests("Old Bass", state);
    CHECK(detectPresetFormat(bytes) == PresetFormat::SerumFxp);
    std::string error;
    const auto file = FxpFile::read(bytes, error);
    REQUIRE(file.has_value());
    REQUIRE(file->programs.size() == 1);
    const auto& p = file->programs.front();
    CHECK(p.isSerum());
    CHECK(p.headerName == "Old Bass");
    CHECK(p.name == "Old Bass");
    CHECK(p.author == "Someone");
    CHECK(p.category == "Bass");
    CHECK(p.stateVersion == Approx(0.1531f));
    CHECK(p.originalStateSize == 28232);
    CHECK(p.state.size() == FxpProgram::kStateSize);
    REQUIRE(p.params.size() == 300);
    CHECK(p.warnings.empty());

    Fixture f;
    const auto result = f.ctl.loadPreset(bytes);
    REQUIRE(result.ok);
    CHECK(f.ctl.get("Filter0.resonance").number() == Approx(0.6));
    CHECK(f.ctl.saveState().find("Serum1.param0") != std::string::npos);
}

// --- Wavetable .wav --------------------------------------------------------------------------------------

TEST_CASE("wavetable .wav round-trips with its clm chunk", "[presets][wav]")
{
    Wavetable wt;
    wt.samples = sawFrames(8, 2048);
    wt.frameCount = 8;
    wt.morphMode = 1;
    const auto bytes = wt.write();
    CHECK(bytes.size() % 2 == 0);
    std::string error;
    const auto back = Wavetable::read(bytes, error);
    REQUIRE(back.has_value());
    CHECK(back->hadClm);
    CHECK(back->frameSize == 2048);
    CHECK(back->frameCount == 8);
    CHECK(back->morphMode == 1);
    CHECK(back->samples == wt.samples);
}

TEST_CASE("wavetables in other formats and frame sizes are read", "[presets][wav]")
{
    // 16-bit PCM stereo, 3 frames of 2048, no clm chunk → frame size inferred as 2048, first channel used.
    const int frames = 3 * 2048;
    std::vector<std::uint8_t> b;
    auto put = [&](const void* p, std::size_t n) { const auto* c = static_cast<const std::uint8_t*>(p); b.insert(b.end(), c, c + n); };
    auto u32 = [&](std::uint32_t v) { put(&v, 4); };
    auto u16 = [&](std::uint16_t v) { put(&v, 2); };
    put("RIFF", 4); u32(36 + frames * 4); put("WAVE", 4);
    put("fmt ", 4); u32(16); u16(1); u16(2); u32(44100); u32(44100 * 4); u16(4); u16(16);
    put("data", 4); u32(frames * 4);
    for (int i = 0; i < frames; ++i) { const std::int16_t l = static_cast<std::int16_t>((i % 2048) * 8 - 8192); const std::int16_t r = 0; put(&l, 2); put(&r, 2); }
    std::string error;
    const auto wt = Wavetable::read(b, error);
    REQUIRE(wt.has_value());
    CHECK_FALSE(wt->hadClm);
    CHECK(wt->frameSize == 2048);
    CHECK(wt->frameCount == 3);
    CHECK(wt->samples[1] == Approx((8.0f - 8192.0f) / 32768.0f));

    // A bare wavetable dropped on the synth goes to oscillator A.
    Fixture f;
    const auto result = f.ctl.loadPreset(b);
    REQUIRE(result.ok);
    CHECK(result.message.find("3-frame") != std::string::npos);
}

TEST_CASE("another synth's .fxp is refused and leaves the patch alone", "[presets][fxp]")
{
    auto bytes = FxpFile::writeForTests("Not Serum", std::vector<std::uint8_t>(1000, 0));
    std::memcpy(bytes.data() + 0x10, "OBXd", 4);
    Fixture f;
    f.ctl.set("Filter0.cutoff", 1234.0);
    const auto r = f.ctl.loadPreset(bytes);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("not a Serum preset") != std::string::npos);
    CHECK(f.ctl.get("Filter0.cutoff").number() == Approx(1234.0));
}
