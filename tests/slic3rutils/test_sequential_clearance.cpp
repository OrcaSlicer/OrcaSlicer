#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/SequentialPrintClearance.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

namespace {

SequentialClearanceInstance instance(coord_t x, coord_t y, double height)
{
    return {height, BoundingBox(Point(x, y), Point(x + 5, y + 10)),
        Polygon{Point(x, y), Point(x + 5, y), Point(x + 5, y + 10), Point(x, y + 10)}};
}

} // namespace

TEST_CASE("Sequential clearance preserves model print order when objects move", "[SequentialClearance][Regression]")
{
    const coord_t last_y = GENERATE(-20, 20);
    std::vector<SequentialClearanceInstance> instances{instance(20, 0, 50.), instance(0, last_y, 50.)};
    const auto warnings = sequential_clearance_height_polygons(instances, 200., 30., 10.);

    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().first == instance(20, 0, 50.).hull_polygon);
    CHECK_THAT(warnings.front().second, WithinAbs(30., 1e-6));
}

TEST_CASE("Sequential clearance handles chains of overlapping Y ranges", "[SequentialClearance][Regression]")
{
    // A overlaps B, B overlaps C, but A does not overlap C. Combining X order
    // for overlaps with Y order otherwise formed the cycle B < A < C < B.
    std::vector<SequentialClearanceInstance> instances{
        instance(20, 0, 50.), instance(10, 8, 50.), instance(0, 16, 50.)};
    const auto warnings = sequential_clearance_height_polygons(instances, 200., 30., 10.);

    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0].first == instance(20, 0, 50.).hull_polygon);
    CHECK(warnings[1].first == instance(10, 8, 50.).hull_polygon);
    CHECK_THAT(warnings[0].second, WithinAbs(10., 1e-6));
    CHECK_THAT(warnings[1].second, WithinAbs(10., 1e-6));
}

TEST_CASE("Sequential clearance applies rod height only to positive Y overlap", "[SequentialClearance]")
{
    const coord_t next_y = GENERATE(9, 10, 11);
    std::vector<SequentialClearanceInstance> instances{instance(0, 0, 50.), instance(20, next_y, 50.)};
    const auto warnings = sequential_clearance_height_polygons(instances, 200., 30., 10.);

    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().first == instances.front().hull_polygon);
    CHECK_THAT(warnings.front().second, WithinAbs(next_y < 10 ? 10. : 30., 1e-6));
}

TEST_CASE("Sequential clearance allows the last object up to printable height", "[SequentialClearance]")
{
    std::vector<SequentialClearanceInstance> instances;
    CHECK(sequential_clearance_height_polygons(instances, 200., 30., 10.).empty());

    instances.push_back(instance(0, 0, 200.));
    CHECK(sequential_clearance_height_polygons(instances, 200., 30., 10.).empty());

    instances.front().instance_height = 201.;
    const auto warnings = sequential_clearance_height_polygons(instances, 200., 30., 10.);
    REQUIRE(warnings.size() == 1);
    CHECK_THAT(warnings.front().second, WithinAbs(200., 1e-6));
}
