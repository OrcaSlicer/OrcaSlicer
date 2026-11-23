#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/libslic3r.h"

#include <initializer_list>
#include <utility>

using namespace Slic3r;

namespace {

Polygon square(double x0, double y0, double size)
{
    return Polygon{ Point::new_scale(x0, y0), Point::new_scale(x0 + size, y0), Point::new_scale(x0 + size, y0 + size),
                    Point::new_scale(x0, y0 + size) };
}

Polyline polyline(std::initializer_list<Vec2d> points)
{
    Polyline out;
    for (const Vec2d &p : points)
        out.points.emplace_back(Point::new_scale(p.x(), p.y()));
    return out;
}

// A 10 mm square with a 2 mm square hole at its center.
ExPolygon square_with_hole()
{
    ExPolygon expoly(square(0., 0., 10.));
    Polygon   hole = square(4., 4., 2.);
    hole.reverse();
    expoly.holes.emplace_back(std::move(hole));
    return expoly;
}

} // namespace

TEST_CASE("A polyline running inside an expolygon is contained in it", "[ExPolygon]")
{
    const ExPolygon expoly = square_with_hole();
    CHECK(expoly.contains(polyline({ { 1., 1. }, { 9., 1. }, { 9., 9. } })));
}

TEST_CASE("A polyline leaving an expolygon is not contained in it", "[ExPolygon]")
{
    const ExPolygon expoly = square_with_hole();
    // Out through the contour, far beyond the bounding box and just past it.
    CHECK_FALSE(expoly.contains(polyline({ { 1., 1. }, { 20., 1. } })));
    CHECK_FALSE(expoly.contains(polyline({ { 1., 1. }, { 10.01, 1. } })));
    // Across the hole.
    CHECK_FALSE(expoly.contains(polyline({ { 1., 5. }, { 9., 5. } })));
}

TEST_CASE("A polyline within the bounding box but outside of a concave expolygon is not contained in it", "[ExPolygon]")
{
    // An L shape: the 10 mm square without its top right 5 mm quarter.
    const ExPolygon l_shape(Polygon{ Point::new_scale(0., 0.), Point::new_scale(10., 0.), Point::new_scale(10., 5.),
                                     Point::new_scale(5., 5.), Point::new_scale(5., 10.), Point::new_scale(0., 10.) });
    CHECK_FALSE(l_shape.contains(polyline({ { 1., 1. }, { 9., 1. }, { 9., 9. } })));
    CHECK(l_shape.contains(polyline({ { 1., 1. }, { 9., 1. }, { 4., 4. }, { 1., 9. } })));
}
