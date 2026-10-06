#include "control/Controller.h"
#include "control/Json.h"
#include "engine/Engine.h"
#include "params/ConfigManager.h"
#include "params/ParamRegistry.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using namespace winerose;
using namespace winerose::control;
using Catch::Approx;

namespace {

struct Fixture {
    std::shared_ptr<ConfigManager> cm = std::make_shared<ConfigManager>();
    Engine                         engine { cm };
    Controller                     ctl { engine, cm };
};

struct GestureLog : IGestureSink {
    std::vector<std::string> events;
    void beginGesture(const std::string& k) override { events.push_back("begin " + k); }
    void endGesture(const std::string& k) override { events.push_back("end " + k); }
};

} // namespace

TEST_CASE("schema describes every registered parameter", "[control]")
{
    Fixture f;
    const auto schema = f.ctl.schema();
    const auto it = std::find_if(schema.begin(), schema.end(),
                                 [](const ParamSchema& s) { return s.nsKey == "Global.masterVolume"; });
    REQUIRE(it != schema.end());
    CHECK(it->module == "Global");
    CHECK(it->key == "masterVolume");
    CHECK(it->type == "float");
    CHECK(it->defaultValue.number() == Approx(0.75));

    const auto q = std::find_if(schema.begin(), schema.end(),
                                [](const ParamSchema& s) { return s.nsKey == "Global.quality"; });
    REQUIRE(q != schema.end());
    CHECK(q->choices.size() == 3);
    CHECK(q->choices[2].second == "Ultra");
}

TEST_CASE("schema and changes serialize to JSON for a web UI", "[control]")
{
    Fixture f;
    nlohmann::json j = f.ctl.schema();
    REQUIRE(j.is_array());
    const auto first = j.at(0).get<ParamSchema>();
    CHECK_FALSE(first.nsKey.empty());

    nlohmann::json c = ParamChange{"Global.masterVolume", 0.5, false};
    const auto back = c.get<ParamChange>();
    CHECK(back.nsKey == "Global.masterVolume");
    CHECK(back.value.number() == 0.5);
}

TEST_CASE("set/get/modify through the controller", "[control]")
{
    Fixture f;
    CHECK(f.ctl.set("Global.masterVolume", 0.5));
    CHECK(f.ctl.get("Global.masterVolume").number() == Approx(0.5));
    CHECK(f.ctl.set("Global.masterVolume", 4.0));   // clamped
    CHECK(f.ctl.get("Global.masterVolume").number() == Approx(1.0));
    CHECK(f.ctl.modify("Global.quality", "High"));
    CHECK(f.ctl.get("Global.quality").number() == 1.0);
    CHECK(f.ctl.format("Global.quality", 2.0) == "Ultra");
    CHECK_FALSE(f.ctl.set("Global.nope", 1.0));
    CHECK_FALSE(f.ctl.set("NoModule.x", 1.0));
    CHECK_FALSE(f.ctl.modify("Global.quality", "Extreme"));
}

TEST_CASE("onChange delivers per-key and batch notifications; subscriptions unsubscribe", "[control]")
{
    Fixture f;
    std::vector<ParamChange> seen;
    {
        auto sub = f.ctl.onChange([&](const ParamChange& c) { seen.push_back(c); });
        f.ctl.set("Global.masterVolume", 0.2);
        REQUIRE(seen.size() == 1);
        CHECK(seen[0].nsKey == "Global.masterVolume");
        CHECK(seen[0].value.number() == Approx(0.2));

        REQUIRE(f.ctl.loadState(f.ctl.saveState()).ok);
        REQUIRE(seen.size() == 2);
        CHECK(seen[1].everything);
    }
    f.ctl.set("Global.masterVolume", 0.3);
    CHECK(seen.size() == 2);
}

TEST_CASE("a gesture is one undo step and is forwarded to the host sink", "[control]")
{
    Fixture f;
    GestureLog log;
    f.ctl.setGestureSink(&log);

    f.ctl.beginGesture("Global.masterVolume");
    f.ctl.set("Global.masterVolume", 0.6);
    f.ctl.set("Global.masterVolume", 0.5);
    f.ctl.set("Global.masterVolume", 0.4);
    f.ctl.endGesture("Global.masterVolume");
    CHECK(log.events == std::vector<std::string>{"begin Global.masterVolume", "end Global.masterVolume"});

    f.ctl.modify("Global.quality", "Ultra");

    REQUIRE(f.ctl.canUndo());
    CHECK(f.ctl.undo());
    CHECK(f.ctl.get("Global.quality").number() == 0.0);
    CHECK(f.ctl.undo());
    CHECK(f.ctl.get("Global.masterVolume").number() == Approx(0.75));
    CHECK_FALSE(f.ctl.undo());

    CHECK(f.ctl.redo());
    CHECK(f.ctl.get("Global.masterVolume").number() == Approx(0.4));
    CHECK(f.ctl.redo());
    CHECK(f.ctl.get("Global.quality").number() == 2.0);
    CHECK_FALSE(f.ctl.canRedo());
}

TEST_CASE("loadState resets keys the state doesn't mention and clears history", "[control]")
{
    Fixture f;
    const std::string initState = f.ctl.saveState();
    f.ctl.set("Global.masterVolume", 0.1);
    f.ctl.modify("Global.quality", "High");

    // A state from an older build that only knew masterVolume.
    const std::string oldState = R"({"format":"winerose.state","version":1,"values":{"Global.masterVolume":0.3}})";
    REQUIRE(f.ctl.loadState(oldState).ok);
    CHECK(f.ctl.get("Global.masterVolume").number() == Approx(0.3));
    CHECK(f.ctl.get("Global.quality").number() == 0.0);   // back to default, not "High"
    CHECK_FALSE(f.ctl.canUndo());

    REQUIRE(f.ctl.loadState(initState).ok);
    CHECK(f.ctl.saveState() == initState);
    CHECK_FALSE(f.ctl.loadState("garbage").ok);
}

TEST_CASE("loadState clamps out-of-range stored values", "[control]")
{
    Fixture f;
    REQUIRE(f.ctl.loadState(R"({"format":"winerose.state","version":1,"values":{"Global.masterVolume":7}})").ok);
    CHECK(f.ctl.get("Global.masterVolume").number() == Approx(1.0));
}

TEST_CASE("loadPreset routes by container format", "[control]")
{
    Fixture f;
    f.ctl.set("Global.masterVolume", 0.2);
    const std::string state = f.ctl.saveState();
    f.ctl.set("Global.masterVolume", 0.9);
    const std::vector<std::uint8_t> own(state.begin(), state.end());
    CHECK(f.ctl.loadPreset(own).ok);
    CHECK(f.ctl.get("Global.masterVolume").number() == Approx(0.2));

    std::vector<std::uint8_t> serum = {'X','f','e','r','J','s','o','n','\0'};
    serum.resize(64, 0);
    const auto r = f.ctl.loadPreset(serum);   // magic but no real payload: decoding fails, patch unchanged
    CHECK_FALSE(r.ok);
    CHECK(r.error.find(".SerumPreset") != std::string::npos);
    CHECK(f.ctl.get("Global.masterVolume").number() == Approx(0.2));
}

TEST_CASE("schema defaults decode enum labels, not just numbers", "[control]")
{
    Fixture f;
    ParamRegistry extra(f.cm, "Test");
    extra.registerEnum("mode", {{0, "Alpha"}, {1, "Beta"}, {2, "Gamma"}}, 2);
    const auto schema = f.ctl.schema();
    const auto it = std::find_if(schema.begin(), schema.end(),
                                 [](const ParamSchema& s) { return s.nsKey == "Test.mode"; });
    REQUIRE(it != schema.end());
    CHECK(it->defaultValue.number() == 2.0);
}

TEST_CASE("FX slot knobs are labelled and formatted for the slot's effect", "[control][fx]")
{
    Fixture f;
    CHECK(f.ctl.label("FXRack0Slot0.p0") == "p0 (unused)");          // type None
    CHECK(f.ctl.set("FXRack0Slot0.type", 9.0));                       // EQ
    CHECK(f.ctl.label("FXRack0Slot0.p0") == "Low Type");
    CHECK(f.ctl.label("FXRack0Slot0.p4") == "High Type");
    CHECK(f.ctl.format("FXRack0Slot0.p0", 0.0) == "Low Shelf");
    CHECK(f.ctl.format("FXRack0Slot0.p0", 0.99) == "High-pass");
    CHECK(f.ctl.format("FXRack0Slot0.p4", 0.99) == "Low-pass");
    CHECK(f.ctl.format("FXRack0Slot0.p2", 0.5) == "+0.0 dB");
    CHECK(f.ctl.format("FXRack0Slot0.p1", 1.0) == "2.00 kHz");
    CHECK(f.ctl.label("Filter0.cutoff") == "cutoff");                 // ordinary params keep their key
}

TEST_CASE("choosing an FX type applies its defaults as one undo step; loading state does not", "[control][fx]")
{
    Fixture f;
    f.ctl.set("FXRack0Slot0.p1", 0.123);
    CHECK(f.ctl.set("FXRack0Slot0.type", 8.0));   // Reverb
    CHECK(f.ctl.get("FXRack0Slot0.p1").number() == Approx(0.45));     // reverb's default decay knob
    CHECK(f.ctl.format("FXRack0Slot0.p0", 0.0) == "Plate");
    CHECK(f.ctl.undo());                           // type and knobs revert together
    CHECK(f.ctl.get("FXRack0Slot0.type").number() == 0.0);
    CHECK(f.ctl.get("FXRack0Slot0.p1").number() == Approx(0.123));

    // A state that sets a type AND custom knobs keeps the custom knobs.
    const std::string state = R"({"format":"winerose.state","version":1,"values":{"FXRack0Slot0.type":8,"FXRack0Slot0.p1":0.9}})";
    REQUIRE(f.ctl.loadState(state).ok);
    CHECK(f.ctl.get("FXRack0Slot0.p1").number() == Approx(0.9));
}

TEST_CASE("display formatting: three significant digits, ms and kHz; typed units are understood", "[control]")
{
    Fixture f;
    CHECK(f.ctl.format("Env0.attack", 0.0005) == "0.5 ms");
    CHECK(f.ctl.format("Env0.release", 0.0149999996) == "15 ms");
    CHECK(f.ctl.format("Env0.decay", 1.0) == "1 s");
    CHECK(f.ctl.format("Filter0.cutoff", 425.0) == "425 Hz");
    CHECK(f.ctl.format("Filter0.cutoff", 4567.0) == "4.57 kHz");
    CHECK(f.ctl.format("Filter0.resonance", 0.100000001) == "0.1");
    CHECK(f.ctl.format("Oscillator0.level", 0.0) == "0");
    CHECK(f.ctl.modify("Env0.attack", "250 ms"));
    CHECK(f.ctl.get("Env0.attack").number() == Approx(0.25));
    CHECK(f.ctl.modify("Filter0.cutoff", "2.5 kHz"));
    CHECK(f.ctl.get("Filter0.cutoff").number() == Approx(2500.0));
    CHECK(f.ctl.modify("Filter0.cutoff", "800"));
    CHECK(f.ctl.get("Filter0.cutoff").number() == Approx(800.0));
}

TEST_CASE("schema marks modulation targets", "[control]")
{
    Fixture f;
    bool cutoff = false, masterVolume = true;
    for (const auto& s : f.ctl.schema()) {
        if (s.nsKey == "Filter0.cutoff") cutoff = s.modulatable;
        if (s.nsKey == "Global.masterVolume") masterVolume = s.modulatable;
    }
    CHECK(cutoff);
    CHECK_FALSE(masterVolume);
}
