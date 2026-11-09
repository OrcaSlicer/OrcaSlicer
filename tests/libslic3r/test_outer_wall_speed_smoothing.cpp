#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/GCode/OuterWallSpeedSmoothing.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

TEST_CASE("Outer wall speeds that already change slowly are kept", "[OuterWallSpeedSmoothing]")
{
    const std::vector<float> speeds = { 100.f, 98.f, 95.f, 93.f, 90.f };
    const std::vector<float> out    = smooth_outer_wall_speeds(speeds);
    REQUIRE(out.size() == speeds.size());
    for (size_t i = 0; i < speeds.size(); ++ i)
        CHECK_THAT(out[i], WithinAbs(speeds[i], 1e-3));
}

TEST_CASE("Outer wall speeds are only reduced and adjacent layers end up close", "[OuterWallSpeedSmoothing]")
{
    // A fast layer is surrounded by slow ones, the speeds of adjacent layers differ by far more than the limit.
    std::vector<float> speeds(21, 40.f);
    for (size_t i = 8; i <= 12; ++ i)
        speeds[i] = 200.f;
    const std::vector<float> out = smooth_outer_wall_speeds(speeds);
    REQUIRE(out.size() == speeds.size());
    for (size_t i = 0; i < speeds.size(); ++ i) {
        CHECK(out[i] <= speeds[i]);
        CHECK(out[i] > 0.f);
    }
    for (size_t i = 0; i + 1 < out.size(); ++ i)
        CHECK(std::abs(out[i + 1] - out[i]) <= 5.f + 1e-3f);
}

TEST_CASE("Layers without an outer wall are skipped when smoothing the outer wall speeds", "[OuterWallSpeedSmoothing]")
{
    const std::vector<float> speeds = { 150.f, 0.f, 0.f, 150.f };
    const std::vector<float> out    = smooth_outer_wall_speeds(speeds);
    REQUIRE(out.size() == speeds.size());
    CHECK_THAT(out[1], WithinAbs(0., 1e-6));
    CHECK_THAT(out[2], WithinAbs(0., 1e-6));
    CHECK_THAT(out[0], WithinAbs(150., 1e-3));
    CHECK_THAT(out[3], WithinAbs(150., 1e-3));
}

TEST_CASE("Smoothing the outer wall speeds of an empty chain returns an empty chain", "[OuterWallSpeedSmoothing]")
{
    CHECK(smooth_outer_wall_speeds({}).empty());
}
