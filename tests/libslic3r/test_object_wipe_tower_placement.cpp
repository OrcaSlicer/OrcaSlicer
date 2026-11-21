#include <catch2/catch_all.hpp>
#include <cstddef>
#include <vector>

#include "catch2/catch_test_macros.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "catch2/matchers/catch_matchers_floating_point.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/ObjectWipeTowerPlacement.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// A counter-clockwise rectangle given in millimetres.
Polygon rectangle(double x0, double y0, double x1, double y1)
{
    return Polygon{Point(scale_(x0), scale_(y0)), Point(scale_(x1), scale_(y0)), Point(scale_(x1), scale_(y1)), Point(scale_(x0), scale_(y1))};
}

constexpr double position_tolerance = 1e-3;

ObjectWipeTowerPlacementParameters parameters()
{
    ObjectWipeTowerPlacementParameters p{};
    p.bed = {rectangle(0., 0., 350., 320.)};
    p.origin = Point(0, 0);
    p.clearance = 8.; // Millimetres; the remaining clearance dimensions are scaled.
    p.object_distance = scaled<float>(4.);
    p.rod_adjustment = scaled(2.);
    p.lid_height = scaled(100.);
    p.rod_height = scaled(40.);
    p.printable_height = scaled(200.);
    return p;
}

ObjectWipeTowerPlacementInstance instance(double x = 100., double y = 100.)
{
    ObjectWipeTowerPlacementInstance i{};
    i.model_hull = rectangle(0., 0., 20., 20.);
    i.shift = Point(scale_(x), scale_(y));
    i.footprint = i.model_hull;
    i.footprint.translate(i.shift);
    i.top = scaled(10.);
    i.tower_hull = rectangle(0., 0., 10., 10.);
    i.tower_height = 10.;
    // No compacted descent, so compacted_config is not needed.
    return i;
}

ObjectWipeTowerPlacementResult place(const std::vector<ObjectWipeTowerPlacementInstance> &instances,
                                     const ObjectWipeTowerPlacementParameters &p)
{
    return place_object_wipe_towers(instances, p, [] {});
}

void require_legal(const ObjectWipeTowerPlacement &placement, const ObjectWipeTowerPlacementInstance &i,
                   const ObjectWipeTowerPlacementParameters &p)
{
    REQUIRE(diff(Polygons{placement.footprint}, p.bed).empty());
    REQUIRE(intersection(Polygons{placement.footprint}, p.excluded).empty());
    REQUIRE(intersection(Polygons{placement.footprint}, Polygons{i.footprint}).empty());
    REQUIRE_THAT(placement.height, WithinAbs(i.tower_height, position_tolerance));
}

} // namespace

TEST_CASE("Tower search prefers the centred right side when free", "[ObjectWipeTowerPlacement]")
{
    const auto i = instance();
    const auto p = parameters();
    const auto result = place({i}, p);
    REQUIRE(result.unplaced.empty());
    REQUIRE(result.placed.size() == 1);
    REQUIRE(result.placed.front().first == 0);
    const auto &tower = result.placed.front().second;
    REQUIRE_THAT(double(tower.position.x()), WithinAbs(122., position_tolerance));
    REQUIRE_THAT(double(tower.position.y()), WithinAbs(105., position_tolerance));
    require_legal(tower, i, p);
}

TEST_CASE("Tower search avoids an excluded preferred side", "[ObjectWipeTowerPlacement]")
{
    const auto i = instance();
    auto p = parameters();
    p.excluded = {rectangle(121., 80., 160., 140.)};
    const auto result = place({i}, p);
    REQUIRE(result.placed.size() == 1);
    REQUIRE(result.unplaced.empty());
    const auto &tower = result.placed.front().second;
    REQUIRE_THAT(double(tower.position.x()), WithinAbs(88., position_tolerance));
    REQUIRE_THAT(double(tower.position.y()), WithinAbs(105., position_tolerance));
    require_legal(tower, i, p);
}

TEST_CASE("Tower search slides to a corner when centred candidates cannot fit", "[ObjectWipeTowerPlacement]")
{
    const auto i = instance();
    auto p = parameters();
    // Leave 0.001 mm around the diagonal endpoint: separately truncated scaled
    // coordinates differ by one grid unit from the sum of object edge + gap.
    // This is far below the 2 mm slide step, so only the endpoint still fits.
    p.bed = {i.footprint, rectangle(121.999, 121.999, 132.001, 132.001)};
    const auto result = place({i}, p);
    REQUIRE(result.placed.size() == 1);
    REQUIRE(result.unplaced.empty());
    const auto &tower = result.placed.front().second;
    REQUIRE_THAT(double(tower.position.x()), WithinAbs(122., position_tolerance));
    REQUIRE_THAT(double(tower.position.y()), WithinAbs(122., position_tolerance));
    require_legal(tower, i, p);
}

TEST_CASE("Tower search tries an outer ring when the entire inner ring is blocked", "[ObjectWipeTowerPlacement]")
{
    const auto i = instance();
    auto p = parameters();
    p.bed = {i.footprint, rectangle(127., 105., 137., 115.)};
    const auto result = place({i}, p);
    REQUIRE(result.placed.size() == 1);
    REQUIRE(result.unplaced.empty());
    const auto &tower = result.placed.front().second;
    REQUIRE_THAT(double(tower.position.x()), WithinAbs(127., position_tolerance));
    REQUIRE_THAT(double(tower.position.y()), WithinAbs(105., position_tolerance));
    require_legal(tower, i, p);
}

TEST_CASE("Tower search reports only instances needing an unplaceable tower", "[ObjectWipeTowerPlacement]")
{
    const auto i = instance();
    auto mono = instance(220., 220.);
    mono.tower_hull = Polygon{};
    auto p = parameters();
    p.bed = {i.footprint, mono.footprint};
    const auto result = place({i, mono}, p);
    REQUIRE(result.placed.empty());
    REQUIRE(result.unplaced == std::vector<size_t>{0});
}

TEST_CASE("Placed towers keep the configured clearance from one another", "[ObjectWipeTowerPlacement]")
{
    auto a = instance();
    auto b = instance(100., 160.);
    // Both preferred RIGHT positions are statically legal but their tall footprints touch.
    a.tower_hull = b.tower_hull = rectangle(0., 0., 10., 60.);
    const auto p = parameters();
    const auto result = place({a, b}, p);
    REQUIRE(result.placed.size() == 2);
    REQUIRE(result.unplaced.empty());
    REQUIRE(result.placed[0].first == 0);
    REQUIRE(result.placed[1].first == 1);
    // Switching to the left side keeps the same Y centre and resolves the
    // conflict. Check the full preferred positions, not just their Y values.
    const auto &first = result.placed[0].second.position;
    const auto &second = result.placed[1].second.position;
    REQUIRE_THAT(double(first.x()), WithinAbs(122., position_tolerance));
    REQUIRE_THAT(double(first.y()), WithinAbs(80., position_tolerance));
    REQUIRE_THAT(double(second.x()), WithinAbs(88., position_tolerance));
    REQUIRE_THAT(double(second.y()), WithinAbs(140., position_tolerance));
    require_legal(result.placed[0].second, a, p);
    require_legal(result.placed[1].second, b, p);
    const Polygons swept = offset(result.placed[0].second.footprint, float(scaled(p.clearance)));
    REQUIRE(intersection(swept, Polygons{result.placed[1].second.footprint}).empty());
}

TEST_CASE("Tower search is deterministic for identical input and print order", "[ObjectWipeTowerPlacement]")
{
    auto p = parameters();
    p.excluded = {rectangle(121., 80., 160., 140.)};
    const std::vector<ObjectWipeTowerPlacementInstance> inputs{instance(), instance(220., 220.)};
    const auto first = place(inputs, p);
    REQUIRE(first.placed.size() == 2);
    for (int repeat = 0; repeat < 3; ++repeat) {
        const auto again = place(inputs, p);
        REQUIRE(again.unplaced == first.unplaced);
        REQUIRE(again.placed.size() == first.placed.size());
        for (size_t j = 0; j < first.placed.size(); ++j) {
            REQUIRE(again.placed[j].first == first.placed[j].first);
            REQUIRE(again.placed[j].second.position.x() == first.placed[j].second.position.x());
            REQUIRE(again.placed[j].second.position.y() == first.placed[j].second.position.y());
            REQUIRE(again.placed[j].second.footprint.points == first.placed[j].second.footprint.points);
            REQUIRE(again.placed[j].second.height == first.placed[j].second.height);
        }
    }
}
