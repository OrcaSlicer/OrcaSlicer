#include <algorithm>
#include <catch2/catch_all.hpp>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <vector>

#include "libslic3r/Brim.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;

namespace {

// Ear points come back in no particular order, so compare them as a set.
bool same_point_set(const Points &a, const Points &b, coord_t tolerance)
{
    if (a.size() != b.size())
        return false;
    std::vector<bool> matched(b.size(), false);
    for (const Point &p : a) {
        bool found = false;
        for (size_t i = 0; i < b.size() && ! found; ++ i) {
            if (! matched[i] && std::abs(p.x() - b[i].x()) <= tolerance && std::abs(p.y() - b[i].y()) <= tolerance) {
                matched[i] = true;
                found      = true;
            }
        }
        if (! found)
            return false;
    }
    return true;
}

bool contains_point(const Points &points, const Point &expected, coord_t tolerance)
{
    return std::any_of(points.begin(), points.end(), [&expected, tolerance](const Point &p) {
        return std::abs(p.x() - expected.x()) <= tolerance && std::abs(p.y() - expected.y()) <= tolerance;
    });
}

bool is_finite(const Point &p)
{
    return std::isfinite(unscale<double>(p.x())) && std::isfinite(unscale<double>(p.y()));
}

// Arc that replaces `corner` between `prev` and `next`, as `segments` chords of the given radius, in mm.
std::vector<Vec2d> fillet_arc(const Vec2d &prev, const Vec2d &corner, const Vec2d &next, double radius, int segments)
{
    const Vec2d  to_prev = (prev - corner).normalized();
    const Vec2d  to_next = (next - corner).normalized();
    const double half    = std::acos(std::clamp(to_prev.dot(to_next), -1., 1.)) / 2.;
    const double tangent = radius / std::tan(half);
    const Vec2d  start   = corner + to_prev * tangent;
    const Vec2d  end     = corner + to_next * tangent;
    const Vec2d  centre  = corner + (to_prev + to_next).normalized() * (radius / std::sin(half));
    const double from    = std::atan2(start.y() - centre.y(), start.x() - centre.x());
    const double to      = std::atan2(end.y() - centre.y(), end.x() - centre.x());
    double       sweep   = to - from;
    while (sweep > PI)
        sweep -= 2. * PI;
    while (sweep < -PI)
        sweep += 2. * PI;
    std::vector<Vec2d> arc;
    for (int i = 0; i <= segments; ++ i) {
        const double angle = from + sweep * i / segments;
        arc.emplace_back(centre.x() + radius * std::cos(angle), centre.y() + radius * std::sin(angle));
    }
    return arc;
}

// 40 mm wide triangle with a 70 degree apex rounded by a 1.5 mm fillet in six chords, counter-clockwise.
// The 60 / 50 degree base corners are deliberately unequal: a mirror symmetric outline ties the fillet's
// two sharpest vertices exactly, and the tie is then broken by vertex index.
std::vector<Vec2d> filleted_triangle_outline()
{
    const Vec2d  a(0., 0.);
    const Vec2d  b(40., 0.);
    const double to_apex = 40. * std::sin(50. * PI / 180.) / std::sin(70. * PI / 180.);
    const Vec2d  apex    = a + Vec2d(std::cos(60. * PI / 180.), std::sin(60. * PI / 180.)) * to_apex;
    const std::vector<Vec2d> arc = fillet_arc(a, apex, b, 1.5, 6);
    std::vector<Vec2d> outline{ a, b };
    outline.insert(outline.end(), arc.rbegin(), arc.rend());
    return outline;
}

Points scaled_filleted_triangle()
{
    return Polygon::new_scale(filleted_triangle_outline()).points;
}

// A quarter turn about the origin is exact in scaled integer coordinates.
Point rotated_quarter_turns(const Point &p, int quarter)
{
    Point rotated = p;
    for (int i = 0; i < quarter; ++ i)
        rotated = Point(-rotated.y(), rotated.x());
    return rotated;
}

} // namespace

// The detection must read the corner, not the contour's start vertex: a decimation anchored on the first
// vertex used to place an ear on a rounded corner for some vertex orders and not for others.
TEST_CASE("Brim ear detection is independent of the contour start vertex", "[Brim][Regression]")
{
    const Points  contour   = scaled_filleted_triangle();
    const coord_t detection = coord_t(scale_(1.0));
    const Points  found     = make_brim_ear_points(Polygon(contour), detection, 125., true);
    REQUIRE(! found.empty());

    for (size_t shift = 1; shift < contour.size(); ++ shift) {
        Points shifted;
        shifted.reserve(contour.size());
        std::rotate_copy(contour.begin(), contour.begin() + shift, contour.end(), std::back_inserter(shifted));
        DYNAMIC_SECTION("start vertex " << shift) {
            const Points ears = make_brim_ear_points(Polygon(shifted), detection, 125., true);
            CHECK(ears.size() == found.size());
            CHECK(same_point_set(ears, found, 2));
        }
    }
}

TEST_CASE("Brim ear detection rotates with the contour", "[Brim]")
{
    const Points  contour   = scaled_filleted_triangle();
    const coord_t detection = coord_t(scale_(1.0));
    const Points  base      = make_brim_ear_points(Polygon(contour), detection, 125., true);
    REQUIRE(! base.empty());

    for (int quarter : { 1, 2, 3 }) {
        DYNAMIC_SECTION("quarter turns " << quarter) {
            Points rotated_contour;
            rotated_contour.reserve(contour.size());
            for (const Point &p : contour)
                rotated_contour.push_back(rotated_quarter_turns(p, quarter));
            Points expected;
            expected.reserve(base.size());
            for (const Point &p : base)
                expected.push_back(rotated_quarter_turns(p, quarter));

            const Points ears = make_brim_ear_points(Polygon(rotated_contour), detection, 125., true);
            CHECK(ears.size() == base.size());
            CHECK(same_point_set(ears, expected, 2));
        }
    }
}

TEST_CASE("A rounded apex carries a single brim ear", "[Brim]")
{
    const coord_t detection = coord_t(scale_(1.0));
    const Points  ears      = make_brim_ear_points(Polygon(scaled_filleted_triangle()), detection, 125., true);

    // Two sharp base corners plus the rounded apex: one ear per corner that turns past the threshold.
    CHECK(ears.size() == 3);
    // A fillet is one corner, so it must not leave a row of adjacent ears along its chords.
    for (size_t i = 0; i < ears.size(); ++ i)
        for (size_t j = i + 1; j < ears.size(); ++ j)
            CHECK((ears[i] - ears[j]).cast<double>().norm() > 2. * detection);
}

TEST_CASE("Zero detection length still finds the sharp corners", "[Brim]")
{
    const Points contour = scaled_filleted_triangle();
    const Points ears    = make_brim_ear_points(Polygon(contour), 0, 125., true);

    // With immediate neighbours only, the two base corners turn far past the threshold.
    CHECK(contains_point(ears, contour[0], 2));
    CHECK(contains_point(ears, contour[1], 2));
    // The fillet is not asserted on: its chords turn below the threshold at per-vertex spacing.
    for (const Point &ear : ears)
        CHECK(is_finite(ear));
}

TEST_CASE("A concave notch takes brim ears on an inner contour", "[Brim]")
{
    // 40 x 40 mm plate with a 10 x 10 mm notch in its top edge, given in the winding a hole is stored in.
    const std::vector<Vec2d> stored_outline{
        Vec2d(0., 40.), Vec2d(20., 40.), Vec2d(20., 30.), Vec2d(30., 30.),
        Vec2d(30., 40.), Vec2d(40., 40.), Vec2d(40., 0.), Vec2d(0., 0.),
    };
    Polygons holes{ Polygon::new_scale(stored_outline) };
    // outer_inner_brim_area reverses hole contours before its concave pass, so the test does the same.
    polygons_reverse(holes);
    const Polygon &contour   = holes.front();
    const coord_t  detection = coord_t(scale_(1.0));

    const auto on_notch_floor = [](const Point &ear) {
        const double x = unscale<double>(ear.x());
        const double y = unscale<double>(ear.y());
        return std::abs(y - 30.) < 1. && x > 19. && x < 31.;
    };

    const Points concave_ears = make_brim_ear_points(contour, detection, 125., false);
    REQUIRE(! concave_ears.empty());
    CHECK(std::all_of(concave_ears.begin(), concave_ears.end(), on_notch_floor));

    const Points convex_ears = make_brim_ear_points(contour, detection, 125., true);
    CHECK(! convex_ears.empty());
    CHECK(std::none_of(convex_ears.begin(), convex_ears.end(), on_notch_floor));
}

TEST_CASE("A clockwise contour keeps brim ear detection well defined", "[Brim]")
{
    Points contour = scaled_filleted_triangle();
    std::reverse(contour.begin(), contour.end());

    Points ears;
    REQUIRE_NOTHROW(ears = make_brim_ear_points(Polygon(contour), coord_t(scale_(1.0)), 125., true));
    for (const Point &ear : ears)
        CHECK(is_finite(ear));
    // No ear count is asserted: the convexity predicate is winding sensitive by design.
}

TEST_CASE("Brim ear max angle excludes a corner at exactly the bound", "[Brim]")
{
    // A rectangle's corners turn by exactly 90 degrees.
    const std::vector<Vec2d> rectangle{ Vec2d(0., 0.), Vec2d(40., 0.), Vec2d(40., 20.), Vec2d(0., 20.) };
    const Polygon            contour   = Polygon::new_scale(rectangle);
    const coord_t            detection = coord_t(scale_(1.0));

    CHECK(make_brim_ear_points(contour, detection, 91., true).size() == 4);
    CHECK(make_brim_ear_points(contour, detection, 90., true).empty());
    CHECK(make_brim_ear_points(contour, detection, 89., true).empty());
}
