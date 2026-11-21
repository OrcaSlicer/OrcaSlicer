#include <algorithm>
#include <catch2/catch_all.hpp>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <vector>

#include "catch2/catch_test_macros.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "catch2/matchers/catch_matchers_floating_point.hpp"
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

// The arc that fillet_arc puts at a corner: its middle point in mm, and the angle it sweeps in radians.
struct Fillet {
    Vec2d  midpoint;
    double sweep;
};

Fillet fillet_geometry(const Vec2d &prev, const Vec2d &corner, const Vec2d &next, double radius)
{
    const Vec2d  to_prev  = (prev - corner).normalized();
    const Vec2d  to_next  = (next - corner).normalized();
    const double half     = std::acos(std::clamp(to_prev.dot(to_next), -1., 1.)) / 2.;
    const Vec2d  midpoint = corner + (to_prev + to_next).normalized() * (radius / std::sin(half) - radius);
    return Fillet{ midpoint, PI - 2. * half };
}

// A closed outline plus the geometry of the arc that replaced each of its corners.
struct FilletedOutline {
    std::vector<Vec2d>  outline;
    std::vector<Fillet> corners;
};

// Replace every corner of the closed `corners` polygon with an arc of `radius` in `chords` chords.
FilletedOutline filleted_outline(const std::vector<Vec2d> &corners, double radius, int chords)
{
    FilletedOutline filleted;
    filleted.outline.reserve(corners.size() * size_t(chords + 1));
    filleted.corners.reserve(corners.size());
    for (size_t i = 0; i < corners.size(); ++ i) {
        const Vec2d &prev = corners[(i + corners.size() - 1) % corners.size()];
        const Vec2d &next = corners[(i + 1) % corners.size()];
        const std::vector<Vec2d> arc = fillet_arc(prev, corners[i], next, radius, chords);
        filleted.outline.insert(filleted.outline.end(), arc.begin(), arc.end());
        filleted.corners.push_back(fillet_geometry(prev, corners[i], next, radius));
    }
    return filleted;
}

// 40 mm wide triangle with a 70 degree apex rounded by a fillet of `radius` in `chords` chords, counter-clockwise.
// The 60 / 50 degree base corners are deliberately unequal: a mirror symmetric outline ties the fillet's
// two sharpest vertices exactly, and the tie is then broken by vertex index.
struct FilletedTriangle {
    std::vector<Vec2d> outline;
    Fillet             apex;
};

FilletedTriangle filleted_triangle(double radius, int chords)
{
    const Vec2d  a(0., 0.);
    const Vec2d  b(40., 0.);
    const double to_apex = 40. * std::sin(50. * PI / 180.) / std::sin(70. * PI / 180.);
    const Vec2d  apex    = a + Vec2d(std::cos(60. * PI / 180.), std::sin(60. * PI / 180.)) * to_apex;
    const std::vector<Vec2d> arc = fillet_arc(a, apex, b, radius, chords);
    std::vector<Vec2d>       outline{ a, b };
    outline.insert(outline.end(), arc.rbegin(), arc.rend());
    return { outline, fillet_geometry(a, apex, b, radius) };
}

std::vector<Vec2d> filleted_triangle_outline(double radius, int chords)
{
    return filleted_triangle(radius, chords).outline;
}

// Axis-aligned CCW square (0, 0)..(side, side) with every corner filleted.
FilletedOutline filleted_square_outline(double side, double radius, int chords)
{
    return filleted_outline({ Vec2d(0., 0.), Vec2d(side, 0.), Vec2d(side, side), Vec2d(0., side) }, radius, chords);
}

// Regular polygon with `sides` vertices on a circle of the given radius, counter-clockwise.
std::vector<Vec2d> regular_polygon_outline(int sides, double radius)
{
    std::vector<Vec2d> outline;
    outline.reserve(size_t(sides));
    for (int i = 0; i < sides; ++ i) {
        const double angle = 2. * PI * i / sides;
        outline.emplace_back(radius * std::cos(angle), radius * std::sin(angle));
    }
    return outline;
}

// Regular `chords`-gon approximating a circle of the given radius.
std::vector<Vec2d> circle_outline(double radius, int chords)
{
    return regular_polygon_outline(chords, radius);
}

// Half disc of `radius`: a flat side and a `chords`-chord semicircular wall meeting it at two right angles.
std::vector<Vec2d> half_disc_outline(double radius, int chords)
{
    std::vector<Vec2d> outline;
    outline.reserve(size_t(chords) + 1);
    outline.emplace_back(-radius, 0.);
    for (int i = 1; i <= chords; ++ i) {
        const double angle = PI + PI * i / chords;
        outline.emplace_back(radius * std::cos(angle), radius * std::sin(angle));
    }
    return outline;
}

// Ellipse with semi-axes `a` and `b` in `chords` chords.
std::vector<Vec2d> ellipse_outline(double a, double b, int chords)
{
    std::vector<Vec2d> outline;
    outline.reserve(size_t(chords));
    for (int i = 0; i < chords; ++ i) {
        const double angle = 2. * PI * i / chords;
        outline.emplace_back(a * std::cos(angle), b * std::sin(angle));
    }
    return outline;
}

// Mean of the outline's vertices, in mm.
Vec2d outline_centroid(const std::vector<Vec2d> &points)
{
    Vec2d sum = Vec2d::Zero();
    for (const Vec2d &p : points)
        sum += p;
    return sum / double(points.size());
}

std::vector<Vec2d> to_mm(const Points &points)
{
    std::vector<Vec2d> out;
    out.reserve(points.size());
    for (const Point &p : points)
        out.emplace_back(unscale<double>(p.x()), unscale<double>(p.y()));
    return out;
}

// Sorted distances between every pair of points, in mm: unchanged by a rigid motion of the whole set.
std::vector<double> pairwise_distances(const std::vector<Vec2d> &points)
{
    std::vector<double> distances;
    distances.reserve(points.size() * (points.size() - 1) / 2);
    for (size_t i = 0; i < points.size(); ++ i)
        for (size_t j = i + 1; j < points.size(); ++ j)
            distances.push_back((points[i] - points[j]).norm());
    std::sort(distances.begin(), distances.end());
    return distances;
}

// Ears sitting on one rounded corner: within half a chord of the arc's midpoint, which is the closest any
// point of the fillet gets to an ear placed on it.
size_t ears_on_corner(const Points &ears, const Fillet &corner, double radius, int chords)
{
    const Point   midpoint  = Point::new_scale(corner.midpoint);
    const coord_t tolerance = coord_t(scale_(radius + 0.5 * radius * corner.sweep / chords));
    return size_t(std::count_if(ears.begin(), ears.end(), [&midpoint, tolerance](const Point &ear) {
        return (ear - midpoint).cast<double>().norm() <= double(tolerance);
    }));
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
    struct Case { double radius; int chords; };

    const coord_t detection = coord_t(scale_(1.0));
    for (const Case &c : { Case{ 1.5, 6 }, Case{ 2.0, 12 }, Case{ 5.0, 12 } }) {
        DYNAMIC_SECTION("fillet " << c.radius << " mm in " << c.chords << " chords") {
            const Points contour = Polygon::new_scale(filleted_triangle_outline(c.radius, c.chords)).points;
            const Points found   = make_brim_ear_points(Polygon(contour), detection, 125., true);
            // Two sharp base corners plus the rounded apex.
            REQUIRE(found.size() == 3);

            for (size_t shift = 1; shift < contour.size(); ++ shift) {
                Points shifted;
                shifted.reserve(contour.size());
                std::rotate_copy(contour.begin(), contour.begin() + shift, contour.end(), std::back_inserter(shifted));
                const Points ears = make_brim_ear_points(Polygon(shifted), detection, 125., true);
                CHECK(ears.size() == found.size());
                CHECK(same_point_set(ears, found, 2));
            }
        }
    }
}

TEST_CASE("Brim ear detection rotates with the contour", "[Brim]")
{
    const std::vector<Vec2d> outline   = filleted_triangle_outline(1.5, 6);
    const Points             contour   = Polygon::new_scale(outline).points;
    const coord_t            detection = coord_t(scale_(1.0));
    const Points             base      = make_brim_ear_points(Polygon(contour), detection, 125., true);
    REQUIRE(base.size() == 3);

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

    SECTION("37 degrees") {
        // 37 degrees is not exact in scaled integer coordinates, so compare ear distances instead of positions.
        const Vec2d  centre = outline_centroid(outline);
        const double angle  = 37. * PI / 180.;
        const auto   rotate = [&centre, angle](const Vec2d &p) -> Vec2d {
            const Vec2d d(p.x() - centre.x(), p.y() - centre.y());
            return Vec2d(centre.x() + d.x() * std::cos(angle) - d.y() * std::sin(angle),
                         centre.y() + d.x() * std::sin(angle) + d.y() * std::cos(angle));
        };

        Points rotated_contour;
        rotated_contour.reserve(outline.size());
        for (const Vec2d &p : outline)
            rotated_contour.push_back(Point::new_scale(rotate(p)));

        std::vector<Vec2d> expected = to_mm(base);
        for (Vec2d &p : expected)
            p = rotate(p);

        const Points ears = make_brim_ear_points(Polygon(rotated_contour), detection, 125., true);
        const std::vector<double> found_distances    = pairwise_distances(to_mm(ears));
        const std::vector<double> expected_distances = pairwise_distances(expected);
        REQUIRE(found_distances.size() == expected_distances.size());
        for (size_t i = 0; i < found_distances.size(); ++ i)
            CHECK_THAT(found_distances[i], Catch::Matchers::WithinAbs(expected_distances[i], 0.01));
    }
}

TEST_CASE("A rounded apex carries a single brim ear", "[Brim]")
{
    const coord_t detection = coord_t(scale_(1.0));

    for (double radius : { 0.5, 1.5, 3.0, 5.0 }) {
        for (int chords : { 3, 6, 12, 24 }) {
            DYNAMIC_SECTION("fillet " << radius << " mm in " << chords << " chords") {
                const FilletedTriangle triangle = filleted_triangle(radius, chords);
                const Points ears = make_brim_ear_points(Polygon::new_scale(triangle.outline), detection, 125., true);
                // Two sharp base corners plus the rounded apex.
                REQUIRE(ears.size() == 3);

                // A fillet is one corner, so exactly one ear sits on it, never a row along its chords.
                CHECK(ears_on_corner(ears, triangle.apex, radius, chords) == 1);
            }
        }
    }
}

TEST_CASE("A square keeps one brim ear per rounded corner", "[Brim][Regression]")
{
    const coord_t         detection = coord_t(scale_(1.0));
    const double          radius    = 5.;
    const int             chords    = 12;
    const FilletedOutline square    = filleted_square_outline(40., radius, chords);
    const Points          ears      = make_brim_ear_points(Polygon::new_scale(square.outline), detection, 125., true);

    // Four 90 degree corners, each rounded by one fillet: four ears, not one per chord.
    REQUIRE(ears.size() == 4);

    for (const Fillet &corner : square.corners)
        CHECK(ears_on_corner(ears, corner, radius, chords) == 1);
}

// A part whose fillets take up most of the perimeter: the ladder stops at its cap of perimeter/8, so the
// saturation lookahead already reaches the neighbouring corner, and twice the window reaches the
// neighbouring ear. Detection used to return two ears at some orientations and none at others.
TEST_CASE("A small part keeps one brim ear per rounded corner", "[Brim][Regression]")
{
    const coord_t detection = coord_t(scale_(1.0));

    SECTION("12 mm square with 4 mm fillets") {
        const double          radius = 4.;
        const int             chords = 12;
        const FilletedOutline square = filleted_square_outline(12., radius, chords);
        const Points          ears   = make_brim_ear_points(Polygon::new_scale(square.outline), detection, 125., true);

        REQUIRE(ears.size() == 4);
        for (const Fillet &corner : square.corners)
            CHECK(ears_on_corner(ears, corner, radius, chords) == 1);
    }

    SECTION("10 mm hexagon with 2 mm fillets") {
        const double          radius  = 2.;
        const int             chords  = 6;
        const FilletedOutline hexagon = filleted_outline(regular_polygon_outline(6, 10.), radius, chords);
        const Points          ears    = make_brim_ear_points(Polygon::new_scale(hexagon.outline), detection, 125., true);

        REQUIRE(ears.size() == 6);
        for (const Fillet &corner : hexagon.corners)
            CHECK(ears_on_corner(ears, corner, radius, chords) == 1);
    }
}

TEST_CASE("A smooth contour has no brim ears", "[Brim]")
{
    const coord_t detection = coord_t(scale_(1.0));

    // A circle's turn is spread evenly, so no vertex turns past the threshold at any measurement scale.
    CHECK(make_brim_ear_points(Polygon::new_scale(circle_outline(5., 48)), detection, 125., true).empty());
    // A 24-gon is as smooth as the detector can tell at the default 1 mm detection length.
    CHECK(make_brim_ear_points(Polygon::new_scale(regular_polygon_outline(24, 10.)), detection, 125., true).empty());
}

// A corner that meets a curved wall is one corner. A point on the curve accepts only once its window reaches
// that corner, so its candidate is the same corner seen from further away; keeping it puts a second ear 2 to
// 7 mm from the first.
TEST_CASE("A corner next to a curve carries one brim ear", "[Brim][Regression]")
{
    const coord_t detection = coord_t(scale_(1.0));

    for (double radius : { 5., 10. }) {
        DYNAMIC_SECTION("half disc of " << radius << " mm") {
            const Points ears = make_brim_ear_points(Polygon::new_scale(half_disc_outline(radius, 64)),
                                                     detection, 125., true);
            // Two right angles, one at each end of the flat side, and nothing on the semicircle between them.
            REQUIRE(ears.size() == 2);
            for (const Point &ear : ears) {
                CHECK_THAT(std::abs(unscale<double>(ear.x())), Catch::Matchers::WithinAbs(radius, 1.));
                CHECK_THAT(unscale<double>(ear.y()), Catch::Matchers::WithinAbs(0., 1.));
            }
        }
    }

    for (int chords : { 64, 128 }) {
        DYNAMIC_SECTION("60 x 10 mm ellipse in " << chords << " chords") {
            const Points ears = make_brim_ear_points(Polygon::new_scale(ellipse_outline(30., 5., chords)),
                                                     detection, 125., true);
            // One ear at each end of the long axis, none on the flanks.
            REQUIRE(ears.size() == 2);
            for (const Point &ear : ears)
                CHECK(std::abs(unscale<double>(ear.x())) > 25.);
        }
    }
}

TEST_CASE("A pentagon keeps one brim ear per rounded corner", "[Brim][Regression]")
{
    // Side 20 mm: circumradius 20 / (2 sin 36 degrees), every corner rounded by 1 mm in six chords.
    const std::vector<Vec2d> corners = regular_polygon_outline(5, 20. / (2. * std::sin(PI / 5.)));
    const FilletedOutline    filleted = filleted_outline(corners, 1., 6);
    const Points             ears     = make_brim_ear_points(Polygon::new_scale(filleted.outline),
                                                              coord_t(scale_(1.0)), 125., true);

    // Five 72 degree corners: a decimation that collapsed the outline to a triangle would report three.
    REQUIRE(ears.size() == 5);
}

TEST_CASE("Zero detection length still finds the sharp corners", "[Brim]")
{
    const std::vector<Vec2d> outline = filleted_triangle_outline(1.5, 6);
    const Points             contour = Polygon::new_scale(outline).points;
    const Points             ears    = make_brim_ear_points(Polygon(contour), 0, 125., true);

    // With immediate neighbours only, the two base corners turn far past the threshold and the fillet does not.
    REQUIRE(ears.size() == 2);
    CHECK(contains_point(ears, contour[0], 2));
    CHECK(contains_point(ears, contour[1], 2));
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
    Points contour = Polygon::new_scale(filleted_triangle_outline(1.5, 6)).points;
    std::reverse(contour.begin(), contour.end());

    Points ears;
    REQUIRE_NOTHROW(ears = make_brim_ear_points(Polygon(contour), coord_t(scale_(1.0)), 125., true));
    // The convexity predicate is winding sensitive, so a reversed outer contour has no convex corner left.
    CHECK(ears.empty());
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
