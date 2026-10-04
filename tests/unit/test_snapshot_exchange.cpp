#include "params/SnapshotExchange.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace winerose;

namespace {
struct Tracked {
    static inline int alive = 0;
    int id;
    explicit Tracked(int i) : id(i) { ++alive; }
    ~Tracked() { --alive; }
};
}

TEST_CASE("audio thread adopts the newest snapshot and old ones are reclaimed", "[params][snapshot]")
{
    Tracked::alive = 0;
    {
        SnapshotExchange<Tracked> ex;
        CHECK(ex.acquire() == nullptr);

        ex.publish(std::make_unique<Tracked>(1));
        REQUIRE(ex.acquire() != nullptr);
        CHECK(ex.acquire()->id == 1);

        ex.publish(std::make_unique<Tracked>(2));
        ex.publish(std::make_unique<Tracked>(3));   // 2 was never adopted: freed immediately
        CHECK(Tracked::alive == 2);                  // 1 (current) + 3 (pending)

        CHECK(ex.acquire()->id == 3);                // 1 retired to the ring
        CHECK(Tracked::alive == 2);
        ex.collectGarbage();                         // message thread frees 1
        CHECK(Tracked::alive == 1);
    }
    CHECK(Tracked::alive == 0);
}

TEST_CASE("many publish/acquire cycles never leak", "[params][snapshot]")
{
    Tracked::alive = 0;
    {
        SnapshotExchange<Tracked, 8> ex;
        for (int i = 0; i < 1000; ++i) {
            ex.publish(std::make_unique<Tracked>(i));
            if (i % 3 == 0) ex.acquire();
        }
        CHECK(Tracked::alive <= 3);
    }
    CHECK(Tracked::alive == 0);
}
