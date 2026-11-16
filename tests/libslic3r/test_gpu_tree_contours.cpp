#include <catch2/catch_all.hpp>

#include <cstdint>
#include <vector>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Gpu/GpuBatchSafety.hpp"
#include "libslic3r/Gpu/TreeContourCandidates.hpp"

using namespace Slic3r;

namespace {

std::vector<Gpu::Segment> whole_contour_boxes(const Polygons& contours)
{
    std::vector<Gpu::Segment> boxes;
    REQUIRE(Gpu::visit_filled_contour_boxes(true, contours,
        [&](coord_t min_x, coord_t min_y, coord_t max_x, coord_t max_y) {
            boxes.push_back({{int64_t(min_x), int64_t(min_y)}, {int64_t(max_x), int64_t(max_y)}});
        }));
    return boxes;
}

Gpu::Segment gpu_line(const Line& line)
{
    return {{int64_t(line.a.x()), int64_t(line.a.y())},
            {int64_t(line.b.x()), int64_t(line.b.y())}};
}

} // namespace

TEST_CASE("Tree broad phase retains filled containment holes and nested islands", "[GpuTreeContours]")
{
    Polygon outer({Point(0, 0), Point(100, 0), Point(100, 100), Point(0, 100)});
    Polygon hole({Point(30, 30), Point(70, 30), Point(70, 70), Point(30, 70)});
    hole.make_clockwise();
    Polygon island({Point(40, 40), Point(60, 40), Point(60, 60), Point(40, 60)});
    const Polygons contours {outer, hole, island};
    const auto boxes = whole_contour_boxes(contours);

    const Line contained(Point(10, 10), Point(20, 10));
    REQUIRE_FALSE(intersection_ln(contained, contours).empty());
    REQUIRE(Gpu::may_intersect_filled_boxes(gpu_line(contained), boxes));

    const Line inside_hole(Point(35, 35), Point(36, 35));
    REQUIRE(intersection_ln(inside_hole, contours).empty());
    // The conservative positive must retain exact CPU hole clipping.
    REQUIRE(Gpu::may_intersect_filled_boxes(gpu_line(inside_hole), boxes));

    const Line nested_island(Point(45, 45), Point(46, 45));
    REQUIRE_FALSE(intersection_ln(nested_island, contours).empty());
    REQUIRE(Gpu::may_intersect_filled_boxes(gpu_line(nested_island), boxes));

    for (const Line& line : Lines {
            contained, inside_hole, nested_island,
            Line(Point(-10, 0), Point(0, 0)),
            Line(Point(-10, 50), Point(110, 50)),
            Line(Point(-20, -20), Point(-10, -10))}) {
        const bool candidate = Gpu::may_intersect_filled_boxes(gpu_line(line), boxes);
        if (!candidate)
            REQUIRE(intersection_ln(line, contours).empty());
    }

    const Line disjoint(Point(-20, -20), Point(-10, -10));
    REQUIRE_FALSE(Gpu::may_intersect_filled_boxes(gpu_line(disjoint), boxes));
    REQUIRE(intersection_ln(disjoint, contours).empty());
}

TEST_CASE("Tree contour boxes conservatively include touching negative and degenerate bounds", "[GpuTreeContours]")
{
    Polygon negative({Point(-100, -100), Point(-50, -100),
                      Point(-50, -50), Point(-100, -50)});
    const auto boxes = whole_contour_boxes(Polygons {negative});
    const Line inside(Point(-90, -90), Point(-80, -90));
    REQUIRE_FALSE(intersection_ln(inside, Polygons {negative}).empty());
    REQUIRE(Gpu::may_intersect_filled_boxes(gpu_line(inside), boxes));
    REQUIRE(Gpu::may_intersect_filled_boxes({{-110, -100}, {-100, -100}}, boxes));
    REQUIRE(Gpu::may_intersect_filled_boxes({{-75, -75}, {-75, -75}}, boxes));
    REQUIRE(Gpu::may_intersect_filled_boxes({{5, 5}, {5, 5}}, {{{5, 5}, {5, 5}}}));
    REQUIRE_FALSE(Gpu::may_intersect_filled_boxes({{-20, -20}, {-10, -10}}, boxes));
}

TEST_CASE("Empty tree contours manufacture no filled contour box", "[GpuTreeContours]")
{
    const Polygons contours {Polygon()};
    const auto boxes = whole_contour_boxes(contours);
    REQUIRE(boxes.empty());
    const Line line(Point(0, 0), Point(10, 10));
    REQUIRE_FALSE(Gpu::may_intersect_filled_boxes(gpu_line(line), boxes));
    REQUIRE(intersection_ln(line, contours).empty());
}
