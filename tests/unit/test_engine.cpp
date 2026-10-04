#include "engine/Engine.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace winerose;

TEST_CASE("engine registers the Global module on its own ConfigManager", "[engine]")
{
    auto cm = std::make_shared<ConfigManager>();
    Engine engine(cm);
    auto* global = cm->findParamRegistry("Global");
    REQUIRE(global != nullptr);
    CHECK(global->find("masterVolume").has_value());
    CHECK(global->get<Quality>("quality") == Quality::Good);
}

TEST_CASE("two engines never share parameter namespaces", "[engine]")
{
    // Two plugin instances in one DAW process: each has its own ConfigManager, so both get "Global".
    auto a = std::make_shared<ConfigManager>();
    auto b = std::make_shared<ConfigManager>();
    Engine ea(a), eb(b);
    CHECK(a->findParamRegistry("Global") != nullptr);
    CHECK(b->findParamRegistry("Global") != nullptr);
    a->findParamRegistry("Global")->set("masterVolume", 0.1f);
    CHECK(b->findParamRegistry("Global")->get<float>("masterVolume") == 0.75f);
}

TEST_CASE("engine renders silence for any block size up to the maximum", "[engine]")
{
    auto cm = std::make_shared<ConfigManager>();
    Engine engine(cm);
    engine.prepare(48000.0, 512);

    std::vector<float> l(512, 1.0f), r(512, 1.0f);
    float* chans[] = {l.data(), r.data()};
    for (int n : {1, 7, 64, 511, 512}) {
        std::fill(l.begin(), l.end(), 1.0f);
        std::fill(r.begin(), r.end(), 1.0f);
        engine.process(chans, 2, n, nullptr, 0, TransportInfo{});
        for (int i = 0; i < n; ++i) {
            REQUIRE(l[static_cast<std::size_t>(i)] == 0.0f);
            REQUIRE(r[static_cast<std::size_t>(i)] == 0.0f);
        }
    }
    CHECK(engine.meters().peakLeft.load() == 0.0f);
    CHECK(engine.latencySamples() == 0);
}
