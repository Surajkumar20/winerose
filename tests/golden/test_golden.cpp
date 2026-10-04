// Golden renders (SPEC §5.8): deterministic engine output compared against stored WAVs with a -100 dB RMS
// tolerance, at 44.1 / 48 / 96 kHz.
//
// Missing or changed output is written to <build>/golden-actual/ for inspection. To accept new output
// (an intentional sound change), re-run with WINEROSE_UPDATE_GOLDEN=1: the files are written into
// tests/golden/data/ and the test passes. Commit them with the change that caused them.

#include "render/OfflineRender.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdlib>
#include <string>

using namespace winerose;
using namespace winerose::render;

namespace {

constexpr double kToleranceDb = -100.0;

bool updateRequested()
{
    const char* v = std::getenv("WINEROSE_UPDATE_GOLDEN");
    return v != nullptr && std::string(v) == "1";
}

// The reference scenario: init patch, one note held for half the render.
AudioData renderInitPatch(double sampleRate, int blockSize)
{
    auto cm = std::make_shared<ConfigManager>();
    Engine engine(cm);
    RenderSpec spec;
    spec.sampleRate = sampleRate;
    spec.blockSize  = blockSize;
    spec.seconds    = 0.1;
    const int half  = static_cast<int>(sampleRate * spec.seconds / 2.0);
    spec.events     = {noteOn(0, 60, 100), noteOff(half, 60)};
    return renderOffline(engine, spec);
}

void checkAgainstGolden(const std::string& name, const AudioData& actual)
{
    const std::filesystem::path golden = std::filesystem::path(WINEROSE_GOLDEN_DIR) / (name + ".wav");
    if (updateRequested()) {
        REQUIRE(writeWav(golden, actual));
        WARN("Updated golden " << golden.string());
        return;
    }
    const auto expected = readWav(golden);
    if (!expected) {
        writeWav(std::filesystem::path(WINEROSE_GOLDEN_OUT) / (name + ".wav"), actual);
        FAIL("Missing golden " << golden.string() << " (actual written to golden-actual/; "
             "set WINEROSE_UPDATE_GOLDEN=1 to accept)");
    }
    const double diff = differenceDb(*expected, actual);
    if (diff > kToleranceDb)
        writeWav(std::filesystem::path(WINEROSE_GOLDEN_OUT) / (name + ".wav"), actual);
    INFO(name << ": difference " << diff << " dB (tolerance " << kToleranceDb << " dB)");
    CHECK(diff <= kToleranceDb);
}

} // namespace

TEST_CASE("init patch matches its golden render at every sample rate", "[golden]")
{
    const double rate = GENERATE(44100.0, 48000.0, 96000.0);
    checkAgainstGolden("init_note_" + std::to_string(static_cast<int>(rate)), renderInitPatch(rate, 512));
}

TEST_CASE("output does not depend on the host block size", "[golden]")
{
    // FL Studio sends variable blocks down to 1 sample (SPEC §5.2). The engine must sound the same
    // regardless — engine branches keep this passing as DSP lands.
    const AudioData reference = renderInitPatch(48000.0, 512);
    const int block = GENERATE(1, 17, 64, 128, 333);
    INFO("block size " << block);
    CHECK(differenceDb(reference, renderInitPatch(48000.0, block)) <= kToleranceDb);
}

TEST_CASE("WAV files round-trip exactly", "[golden][wav]")
{
    AudioData audio(2, 100, 44100.0);
    for (int i = 0; i < 100; ++i) {
        audio.channel(0)[i] = static_cast<float>(i) / 100.0f;
        audio.channel(1)[i] = -static_cast<float>(i) / 50.0f;
    }
    const auto file = std::filesystem::path(WINEROSE_GOLDEN_OUT) / "roundtrip.wav";
    REQUIRE(writeWav(file, audio));
    const auto back = readWav(file);
    REQUIRE(back.has_value());
    CHECK(back->numChannels == 2);
    CHECK(back->numFrames == 100);
    CHECK(back->sampleRate == 44100.0);
    CHECK(differenceDb(audio, *back) == -300.0);
    std::filesystem::remove(file);
}
