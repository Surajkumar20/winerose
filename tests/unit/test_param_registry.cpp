#include "params/ParamRegistry.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace winerose;
using Catch::Approx;

namespace {
enum class Mode { A = 0, B = 1, C = 2 };
}

TEST_CASE("registration namespaces keys and hydrates defaults", "[params][registry]")
{
    auto cm = std::make_shared<ConfigManager>();
    ParamRegistry reg(cm, "Filter0");
    reg.registerFloat("cutoff", 425.0f, 8.18f, 22050.0f, "Filter", "Cutoff frequency",
                      ParamOpts{NumericMeta::Curve::Exp, 1.0, "Hz"});

    CHECK(reg.resolvedName() == "Filter0");
    CHECK(cm->contains("Filter0.cutoff"));
    CHECK(reg.get<float>("cutoff") == Approx(425.0f));

    const auto def = reg.find("cutoff");
    REQUIRE(def.has_value());
    CHECK(def->default_value == "425");
    CHECK(std::get<NumericMeta>(def->meta).unit == "Hz");
}

TEST_CASE("hydration prefers an already-stored value but clamps it", "[params][registry]")
{
    auto cm = std::make_shared<ConfigManager>();
    cm->set<double>("Osc.level", 0.2);
    cm->set<double>("Osc.semi", 99.0);
    ParamRegistry reg(cm, "Osc");
    reg.registerFloat("level", 0.75f, 0.0f, 1.0f);
    reg.registerInt("semi", 0, -12, 12);

    CHECK(reg.get<float>("level") == Approx(0.2f));
    CHECK(reg.get<int>("semi") == 12);
    CHECK(reg.find("level")->default_value == "0.75");   // ORIGINAL default recorded
}

TEST_CASE("same requested name resolves to distinct namespaces", "[params][registry]")
{
    auto cm = std::make_shared<ConfigManager>();
    ParamRegistry a(cm, "LFO");
    ParamRegistry b(cm, "LFO");
    ParamRegistry c(cm, "LFO");
    CHECK(a.resolvedName() == "LFO");
    CHECK(b.resolvedName() == "LFO_2");
    CHECK(c.resolvedName() == "LFO_3");
    CHECK(cm->getAttachedParamRegistries().size() == 3);
}

TEST_CASE("destruction detaches from ConfigManager", "[params][registry]")
{
    auto cm = std::make_shared<ConfigManager>();
    {
        ParamRegistry reg(cm, "Env0");
        CHECK(cm->findParamRegistry("Env0") == &reg);
    }
    CHECK(cm->findParamRegistry("Env0") == nullptr);
    ParamRegistry again(cm, "Env0");
    CHECK(again.resolvedName() == "Env0");   // name is reusable once released
}

TEST_CASE("set clamps to the registered range and ignores unknown keys", "[params][registry]")
{
    auto cm = std::make_shared<ConfigManager>();
    ParamRegistry reg(cm, "Osc");
    reg.registerInt("unison", 1, 1, 16);
    reg.set("unison", 40);
    CHECK(reg.get<int>("unison") == 16);
    reg.set("unison", 3.7);
    CHECK(reg.get<int>("unison") == 4);
    reg.set("nope", 5);
    CHECK_FALSE(cm->contains("Osc.nope"));
}

TEST_CASE("modify parses user text per registered type", "[params][registry]")
{
    auto cm = std::make_shared<ConfigManager>();
    ParamRegistry reg(cm, "Global");
    reg.registerInt("poly", 8, 1, 32);
    reg.registerEnum<Mode>("mode", {{Mode::A, "Alpha"}, {Mode::B, "Beta"}, {Mode::C, "Gamma"}}, Mode::A);
    reg.registerBool("mono", false);
    reg.registerString("name", "Init");

    CHECK(reg.modify("poly", "16.0"));
    CHECK(reg.get<int>("poly") == 16);
    CHECK_FALSE(reg.modify("poly", "lots"));
    CHECK(reg.get<int>("poly") == 16);   // untouched on failure

    CHECK(reg.modify("mode", "Gamma"));
    CHECK(reg.get<Mode>("mode") == Mode::C);
    CHECK(reg.modify("mode", "1"));
    CHECK(reg.get<Mode>("mode") == Mode::B);
    CHECK_FALSE(reg.modify("mode", "9"));

    CHECK(reg.modify("mono", "On"));
    CHECK(reg.get<bool>("mono"));

    CHECK(reg.modify("name", "Lead 01"));
    CHECK(reg.get<std::string>("name") == "Lead 01");

    CHECK_FALSE(reg.modify("missing", "1"));
}

TEST_CASE("durations are stored as milliseconds and read back typed", "[params][registry]")
{
    using namespace std::chrono;
    auto cm = std::make_shared<ConfigManager>();
    ParamRegistry reg(cm, "Arp0");
    reg.registerDuration("gateTime", seconds(2), milliseconds(0), seconds(10));
    CHECK(reg.get<milliseconds>("gateTime") == milliseconds(2000));
    CHECK(reg.get<seconds>("gateTime") == seconds(2));
    reg.set("gateTime", milliseconds(1500));
    CHECK(cm->get<int>("Arp0.gateTime", 0) == 1500);
    CHECK(std::get<NumericMeta>(reg.find("gateTime")->meta).unit == "ms");
}

TEST_CASE("strings are never automatable", "[params][registry]")
{
    auto cm = std::make_shared<ConfigManager>();
    ParamRegistry reg(cm, "Oscillator0");
    reg.registerString("wavetable", "Basic Shapes", "", "", ParamOpts{.automatable = true});
    CHECK_FALSE(reg.find("wavetable")->automatable);
    CHECK_FALSE(reg.handle("wavetable").valid());
}

TEST_CASE("handle reads the live value without going through the registry", "[params][registry]")
{
    auto cm = std::make_shared<ConfigManager>();
    ParamRegistry reg(cm, "Global");
    reg.registerFloat("masterVolume", 0.75f, 0.0f, 1.0f);
    const ParamHandle h = reg.handle("masterVolume");
    REQUIRE(h.valid());
    CHECK(h.load() == Approx(0.75f));
    reg.set("masterVolume", 0.25f);
    CHECK(h.load() == Approx(0.25f));
}

TEST_CASE("resetToDefaults restores every param in one batch", "[params][registry]")
{
    struct Counter : IParamListener {
        int changes = 0, batches = 0;
        void onParamChanged(const std::string&) override { ++changes; }
        void onBatchEnd() override { ++batches; }
    } counter;

    auto cm = std::make_shared<ConfigManager>();
    ParamRegistry reg(cm, "Osc");
    reg.registerFloat("level", 0.75f, 0.0f, 1.0f);
    reg.registerString("table", "Basic");
    reg.set("level", 0.1f);
    reg.set<std::string>("table", "Other");

    cm->addListener(&counter);
    reg.resetToDefaults();
    cm->removeListener(&counter);

    CHECK(reg.get<float>("level") == Approx(0.75f));
    CHECK(reg.get<std::string>("table") == "Basic");
    CHECK(counter.changes == 0);
    CHECK(counter.batches == 1);
}

TEST_CASE("validate and clampAndApply catch out-of-range values", "[params][registry]")
{
    auto cm = std::make_shared<ConfigManager>();
    ParamRegistry reg(cm, "Osc");
    reg.registerFloat("level", 0.75f, 0.0f, 1.0f);
    reg.registerFloat("tiny", 0.05f, 0.0f, 0.1f);   // 0.1 is not exact in float: must not false-positive
    reg.set("tiny", 0.1f);
    CHECK(reg.validate().empty());

    cm->set<double>("Osc.level", 3.0);   // a raw write that bypassed the registry
    const auto errors = reg.validate();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].key == "Osc.level");

    CHECK(reg.clampAndApply(*reg.find("level")));
    CHECK(reg.get<float>("level") == Approx(1.0f));
    CHECK_FALSE(reg.clampAndApply(*reg.find("level")));
}

TEST_CASE("normalization goes through the registered curve", "[params][registry]")
{
    auto cm = std::make_shared<ConfigManager>();
    ParamRegistry reg(cm, "Filter0");
    reg.registerFloat("cutoff", 425.0f, 8.18f, 22050.0f, "", "", ParamOpts{NumericMeta::Curve::Exp});
    CHECK(reg.fromNormalized("cutoff", 0.5) == Approx(424.7).margin(0.5));
    CHECK(reg.toNormalized("cutoff", 22050.0) == Approx(1.0));
}

TEST_CASE("writeParamSchema writes the INI schema", "[params][registry]")
{
    auto cm = std::make_shared<ConfigManager>();
    ParamRegistry reg(cm, "Global");
    reg.registerFloat("masterVolume", 0.75f, 0.0f, 1.0f, "Master", "Output level");
    reg.registerEnum("quality", {{0, "Good"}, {1, "High"}}, 0, "Master");

    const auto root = std::filesystem::temp_directory_path() / "winerose_schema_test";
    std::filesystem::remove_all(root);
    REQUIRE(cm->writeParamSchema(root, reg.resolvedName(), reg.getAll()));

    std::string text;
    {
        std::ifstream in(root / "Global" / "schema.ini");
        std::stringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    }
    CHECK(text.find("[masterVolume]\ntype=float\ngroup=Master\ntooltip=Output level\ndefault=0.75\n") != std::string::npos);
    CHECK(text.find("min=0\nmax=1\ncurve=linear\n") != std::string::npos);
    CHECK(text.find("[quality]\ntype=enum") != std::string::npos);
    CHECK(text.find("choices=0:Good,1:High") != std::string::npos);
    std::filesystem::remove_all(root);
}
