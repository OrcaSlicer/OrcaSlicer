#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <utility>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Emboss.hpp"
#include "libslic3r/EmbossShape.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// One square letter. Its first two points are the bottom edge, left to right.
ExPolygons square(coord_t center_x, coord_t bottom_y, coord_t size)
{
    const coord_t half = size / 2;
    Polygon contour({Point(center_x - half, bottom_y), Point(center_x + half, bottom_y),
                     Point(center_x + half, bottom_y + size), Point(center_x - half, bottom_y + size)});
    return {ExPolygon(contour)};
}

ExPolygonsWithId letter(unsigned id, ExPolygons shape = {}) { return ExPolygonsWithId{id, std::move(shape)}; }

const Points &points_of(const ExPolygonsWithId &letter) { return letter.expoly.front().contour.points; }

// Distance from the middle of the letter's bottom edge to a point.
double bottom_center_distance(const ExPolygonsWithId &letter, const Vec2d &to)
{
    const Points &pts = points_of(letter);
    const Vec2d   mid = (pts[0].cast<double>() + pts[1].cast<double>()) / 2.;
    return (mid - to).norm();
}

} // namespace

TEST_CASE("A letter under the anchor is not moved by bending", "[Emboss]")
{
    ExPolygonsWithIds shapes{letter('a', square(0, 0, 100))};
    const Points      before = points_of(shapes.front());

    Emboss::bend_to_arc(shapes, 1000., 0., 200.);

    REQUIRE(points_of(shapes.front()) == before);
}

TEST_CASE("A bent letter keeps its shape and sits on the circle", "[Emboss]")
{
    // The letter is a quarter of the circle away from the anchor, so it ends up beside the centre, turned on its side.
    const double      radius = 2000. / M_PI;
    ExPolygonsWithIds shapes{letter('a', square(1000, 0, 100))};

    Emboss::bend_to_arc(shapes, radius, 0., 200.);

    const BoundingBox bb = get_extents(shapes.front().expoly);
    CHECK_THAT(static_cast<double>(bb.min.x()), WithinAbs(637., 1.5));
    CHECK_THAT(static_cast<double>(bb.min.y()), WithinAbs(-687., 1.5));
    CHECK_THAT(static_cast<double>(bb.max.x()), WithinAbs(737., 1.5));
    CHECK_THAT(static_cast<double>(bb.max.y()), WithinAbs(-587., 1.5));
    CHECK_THAT(shapes.front().expoly.front().area(), WithinRel(10000., 0.01));
}

TEST_CASE("A negative radius bends the text the other way", "[Emboss]")
{
    const double      radius = -2000. / M_PI;
    ExPolygonsWithIds shapes{letter('a', square(1000, 0, 100))};

    Emboss::bend_to_arc(shapes, radius, 0., 200.);

    // The text end rises and the letter's top points at the centre, which is now above the text.
    const BoundingBox bb = get_extents(shapes.front().expoly);
    CHECK_THAT(static_cast<double>(bb.min.x()), WithinAbs(537., 1.5));
    CHECK_THAT(static_cast<double>(bb.min.y()), WithinAbs(587., 1.5));
    CHECK_THAT(static_cast<double>(bb.max.x()), WithinAbs(637., 1.5));
    CHECK_THAT(static_cast<double>(bb.max.y()), WithinAbs(687., 1.5));
}

TEST_CASE("Lines of bent text share one centre", "[Emboss]")
{
    const double      radius      = 1000.;
    const double      line_height = 200.;
    ExPolygonsWithIds shapes{letter('a', square(1000, 0, 100)), letter(Emboss::ENTER_UNICODE),
                             letter('b', square(1000, -200, 100))};

    Emboss::bend_to_arc(shapes, radius, 0., line_height);

    const Vec2d center(0., -radius);
    CHECK_THAT(bottom_center_distance(shapes[0], center), WithinAbs(radius, 1.5));
    CHECK_THAT(bottom_center_distance(shapes[2], center), WithinAbs(radius - line_height, 1.5));
}

TEST_CASE("A line at or past the circle's centre is left straight", "[Emboss]")
{
    ExPolygonsWithIds shapes{letter('a', square(1000, 0, 100)), letter(Emboss::ENTER_UNICODE),
                             letter('b', square(1000, -200, 100))};
    const Points first_line  = points_of(shapes[0]);
    const Points second_line = points_of(shapes[2]);

    // The second line's baseline would be 100 units past the centre.
    Emboss::bend_to_arc(shapes, 100., 0., 200.);

    CHECK(points_of(shapes[0]) != first_line);
    CHECK(points_of(shapes[2]) == second_line);
    for (const Point &p : points_of(shapes[0])) {
        CHECK(std::abs(p.x()) < 1000000);
        CHECK(std::abs(p.y()) < 1000000);
    }
}

TEST_CASE("Empty letters are skipped when bending", "[Emboss]")
{
    ExPolygonsWithIds shapes{letter('a', square(-500, 0, 100)), letter(' '), letter('\t'), letter('b', square(500, 0, 100))};

    Emboss::bend_to_arc(shapes, 1000., 0., 200.);

    REQUIRE(shapes.size() == 4);
    CHECK(shapes[1].expoly.empty());
    CHECK(shapes[2].expoly.empty());
    // The two letters are mirror images of each other about the anchor.
    const BoundingBox left  = get_extents(shapes[0].expoly);
    const BoundingBox right = get_extents(shapes[3].expoly);
    CHECK_THAT(static_cast<double>(left.min.x()), WithinAbs(-static_cast<double>(right.max.x()), 1.5));
    CHECK_THAT(static_cast<double>(left.min.y()), WithinAbs(static_cast<double>(right.min.y()), 1.5));
    CHECK(left.max.y() < 100);
}
