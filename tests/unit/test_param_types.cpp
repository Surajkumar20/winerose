#include "params/ParamTypes.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace winerose;
using Catch::Approx;

namespace {
ParamMeta numeric(double mn, double mx, NumericMeta::Curve c = NumericMeta::Curve::Linear, double p = 1.0,
                  NumericMeta::SubType st = NumericMeta::SubType::FLOAT)
{
    return NumericMeta{st, mn, mx, c, p, {}};
}
}

TEST_CASE("linear normalization round-trips and clamps", "[params][types]")
{
    const auto m = numeric(-12.0, 12.0);
    CHECK(toNormalized(m, 0.0) == Approx(0.5));
    CHECK(fromNormalized(m, 0.25) == Approx(-6.0));
    CHECK(toNormalized(m, 100.0) == Approx(1.0));
    CHECK(fromNormalized(m, -1.0) == Approx(-12.0));
}

TEST_CASE("exp normalization matches the SPEC's inferred filter cutoff curve", "[params][types]")
{
    // SPEC §1, finding 6: Hz = 8.18·(22050/8.18)^x; x = 0.5 shows about 425 Hz.
    const auto m = numeric(8.18, 22050.0, NumericMeta::Curve::Exp);
    CHECK(fromNormalized(m, 0.5) == Approx(424.7).margin(0.5));
    CHECK(toNormalized(m, fromNormalized(m, 0.3)) == Approx(0.3));
}

TEST_CASE("exp curve with a non-positive minimum falls back to linear", "[params][types]")
{
    const auto m = numeric(0.0, 10.0, NumericMeta::Curve::Exp);
    CHECK(fromNormalized(m, 0.5) == Approx(5.0));
}

TEST_CASE("power normalization round-trips", "[params][types]")
{
    const auto m = numeric(0.0, 32.0, NumericMeta::Curve::Power, 3.0);   // SPEC §5.5 env time hypothesis
    CHECK(fromNormalized(m, 0.5) == Approx(4.0));
    CHECK(toNormalized(m, 4.0) == Approx(0.5));
}

TEST_CASE("int params round and enums snap to choices", "[params][types]")
{
    const auto i = numeric(0, 16, NumericMeta::Curve::Linear, 1.0, NumericMeta::SubType::INT);
    CHECK(clampPlain(i, 3.6) == 4.0);
    CHECK(fromNormalized(i, 0.5) == 8.0);

    const ParamMeta e = EnumMeta{{{0, "Off"}, {5, "Mid"}, {10, "Max"}}, false};
    CHECK(clampPlain(e, 6.0) == 5.0);
    CHECK(toNormalized(e, 10.0) == Approx(1.0));
    CHECK(toNormalized(e, 5.0) == Approx(0.5));
    CHECK(fromNormalized(e, 0.6) == 5.0);
}

TEST_CASE("parsePlain interprets text per type", "[params][types]")
{
    const auto i = numeric(0, 100, NumericMeta::Curve::Linear, 1.0, NumericMeta::SubType::INT);
    CHECK(parsePlain(i, "50") == 50.0);
    CHECK(parsePlain(i, " 50.0 ") == 50.0);
    CHECK(parsePlain(i, "500") == 100.0);   // clamped
    CHECK_FALSE(parsePlain(i, "fifty").has_value());
    CHECK_FALSE(parsePlain(i, "").has_value());

    const ParamMeta e = EnumMeta{{{0, "Good"}, {1, "High"}, {2, "Ultra"}}, false};
    CHECK(parsePlain(e, "2") == 2.0);
    CHECK(parsePlain(e, "ultra") == 2.0);
    CHECK_FALSE(parsePlain(e, "7").has_value());
    CHECK_FALSE(parsePlain(e, "Extreme").has_value());

    const ParamMeta b = EnumMeta{{{0, "Off"}, {1, "On"}}, true};
    CHECK(parsePlain(b, "true") == 1.0);
    CHECK(parsePlain(b, "Off") == 0.0);

    CHECK_FALSE(parsePlain(StringMeta{}, "anything").has_value());
}

TEST_CASE("formatPlain and typeName", "[params][types]")
{
    const ParamMeta e = EnumMeta{{{0, "Good"}, {1, "High"}}, false};
    CHECK(formatPlain(e, 1.0) == "High");
    CHECK(formatPlain(numeric(0, 10, NumericMeta::Curve::Linear, 1, NumericMeta::SubType::INT), 3.0) == "3");
    CHECK(formatNumber(0.75) == "0.75");
    CHECK(std::string(typeName(e)) == "enum");
    CHECK(std::string(typeName(EnumMeta{{{0, "Off"}, {1, "On"}}, true})) == "bool");
    CHECK(std::string(typeName(StringMeta{})) == "string");
}
