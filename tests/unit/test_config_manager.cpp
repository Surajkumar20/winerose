#include "params/ParamRegistry.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace winerose;
using Catch::Approx;

namespace {
struct Recorder : IParamListener {
    std::vector<std::string> keys;
    int batches = 0;
    void onParamChanged(const std::string& k) override { keys.push_back(k); }
    void onBatchEnd() override { ++batches; }
};
}

TEST_CASE("typed get/set with defaults and cross-type fallback", "[params][config]")
{
    ConfigManager cm;
    CHECK(cm.get<int>("missing", 7) == 7);
    cm.set("a", 0.5);
    CHECK(cm.get<double>("a", 0.0) == Approx(0.5));
    CHECK(cm.get<std::string>("a", "") == "0.5");
    cm.set("s", "hello");
    CHECK(cm.get<std::string>("s", "") == "hello");
    CHECK(cm.get<double>("s", -1.0) == -1.0);   // non-numeric text → default
    cm.set("n", std::string("42"));
    CHECK(cm.get<int>("n", 0) == 42);           // numeric text parses
    cm.set("b", true);
    CHECK(cm.get<bool>("b", false));
}

TEST_CASE("listeners hear per-key changes, batches coalesce, and batches nest", "[params][config]")
{
    ConfigManager cm;
    Recorder r;
    cm.addListener(&r);

    cm.set("x", 1.0);
    CHECK(r.keys == std::vector<std::string>{"x"});

    cm.beginBatch();
    cm.set("y", 1.0);
    cm.beginBatch();
    cm.set("z", 1.0);
    cm.endBatch();
    CHECK(r.batches == 0);   // inner end doesn't fire
    cm.endBatch();
    CHECK(r.batches == 1);
    CHECK(r.keys.size() == 1);

    cm.beginBatch();
    cm.endBatch();
    CHECK(r.batches == 1);   // empty batch doesn't fire

    cm.removeListener(&r);
    cm.set("x", 2.0);
    CHECK(r.keys.size() == 1);
}

TEST_CASE("serialize/restore round-trips and preserves unknown keys", "[params][config]")
{
    auto cm = std::make_shared<ConfigManager>();
    {
        ParamRegistry reg(cm, "Osc");
        reg.registerFloat("level", 0.75f, 0.0f, 1.0f);
        reg.registerString("table", "Basic");
        reg.set("level", 0.3f);
        cm->set<double>("Future.thing", 9.0);   // a key from a newer build
        const std::string blob = cm->serialize();

        auto other = std::make_shared<ConfigManager>();
        REQUIRE(other->restore(blob));
        ParamRegistry reg2(other, "Osc");
        reg2.registerFloat("level", 0.75f, 0.0f, 1.0f);   // hydrates from the restored value
        reg2.registerString("table", "Basic");
        CHECK(reg2.get<float>("level") == Approx(0.3f));
        CHECK(reg2.get<std::string>("table") == "Basic");
        CHECK(other->get<double>("Future.thing", 0.0) == Approx(9.0));
        CHECK(other->serialize() == blob);   // byte-identical
    }
}

TEST_CASE("restore rejects garbage without touching state", "[params][config]")
{
    ConfigManager cm;
    cm.set("a", 1.0);
    Recorder r;
    cm.addListener(&r);
    CHECK_FALSE(cm.restore("not json"));
    CHECK_FALSE(cm.restore(R"({"format":"something.else","values":{}})"));
    CHECK(cm.get<double>("a", 0.0) == 1.0);
    CHECK(r.batches == 0);
    CHECK(ConfigManager::isValidState(cm.serialize()));
}

TEST_CASE("restore fires exactly one batch notification", "[params][config]")
{
    ConfigManager cm;
    cm.set("a", 1.0);
    const auto blob = cm.serialize();
    Recorder r;
    cm.addListener(&r);
    REQUIRE(cm.restore(blob));
    CHECK(r.batches == 1);
    CHECK(r.keys.empty());
}

TEST_CASE("slots keep stable addresses as more keys are added", "[params][config]")
{
    ConfigManager cm;
    cm.set("first", 1.0);
    const auto* slot = cm.numericSlot("first");
    for (int i = 0; i < 5000; ++i) cm.set("k" + std::to_string(i), static_cast<double>(i));
    CHECK(cm.numericSlot("first") == slot);
    CHECK(slot->load() == 1.0f);
}

TEST_CASE("host writes go straight to the slot and notify later", "[params][config]")
{
    ConfigManager cm;
    cm.set("p", 0.0);
    Recorder r;
    cm.addListener(&r);
    cm.hostWritableSlot("p")->store(0.5f);
    CHECK(r.keys.empty());
    CHECK(cm.get<double>("p", 0.0) == Approx(0.5));
    cm.notifyChanged("p");
    CHECK(r.keys == std::vector<std::string>{"p"});
}
