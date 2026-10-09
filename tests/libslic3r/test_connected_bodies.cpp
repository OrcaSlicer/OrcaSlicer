#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/ConnectedBodies.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include <cstddef>
#include <utility>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

ExPolygon rectangle(double x, double width) { return ExPolygon(Polygon::new_scale({ { x, 0. }, { x + width, 0. }, { x + width, 10. }, { x, 10. } })); }

} // namespace

TEST_CASE("Islands overlapping their neighbors' make one body", "[ConnectedBodies]")
{
    const ExPolygons apart  = { rectangle(0., 10.), rectangle(20., 10.) };
    const ExPolygons bridge = { rectangle(0., 30.) };
    const ExPolygons left   = { rectangle(0., 10.) };
    const ExPolygons right  = { rectangle(20., 10.) };
    size_t           count  = 0;

    const std::vector<std::vector<size_t>> stacked = connected_bodies({ &apart, &apart }, count);
    CHECK(count == 2);
    CHECK(stacked[1][0] == stacked[0][0]);
    CHECK(stacked[1][1] == stacked[0][1]);

    connected_bodies({ &apart, &bridge, &apart }, count);
    CHECK(count == 1);

    // Neighbors that do not overlap stay apart even with one island a layer.
    connected_bodies({ &left, &right }, count);
    CHECK(count == 2);
}

TEST_CASE("The island locator tests the outlines only where boxes overlap", "[ConnectedBodies]")
{
    const auto square = [](double from, double to) {
        return Polygon::new_scale({ { from, from }, { to, from }, { to, to }, { from, to } });
    };
    Polygon hole = square(5., 25.);
    hole.make_clockwise();
    ExPolygon ring(square(0., 30.));
    ring.holes.emplace_back(hole);
    ExPolygon alone(square(40., 50.));
    const ExPolygons    islands = { ring, ExPolygon(square(10., 20.)), alone };
    const IslandLocator locator(islands, scaled<coord_t>(1.));
    const auto          at = [](double x, double y) { return Point::new_scale(x, y); };

    CHECK(locator.find(at(2., 2.)).first == 0);
    CHECK(locator.find(at(15., 15.)).first == 1);
    // In the ring's hole, outside the island within it, the nearest outline counts.
    CHECK(locator.find(at(7., 15.)).first == 0);
    CHECK(locator.find(at(9.5, 15.)).first == 1);
    // An island no other box reaches takes the margin past its outline.
    CHECK(locator.find(at(50.5, 45.)).first == 2);
    // Unless strict, as for an island whose neighbor is another instance's: then it is only the nearest, 0.5 mm away.
    const auto [nearest, distance] = locator.find(at(50.5, 45.), true);
    CHECK(nearest == 2);
    CHECK_THAT(distance, WithinRel(sqr(scaled<double>(0.5)), 1e-6));
    CHECK_FALSE(locator.holds(2, at(50.5, 45.), true));
    CHECK(locator.holds(2, at(50.5, 45.)));
    CHECK_FALSE(locator.holds(1, at(7., 15.)));
    CHECK(locator.find(at(35., 45.)).first == -1);
}

TEST_CASE("Separate solids are separate bodies", "[ConnectedBodies]")
{
    const indexed_triangle_set cube   = its_make_cube(10., 10., 10.);
    const auto                 bodies = solid_bodies({ { &cube, Transform3d::Identity() }, { &cube, Geometry::translation_transform({ 20., 0., 0. }) } }, { 1., 1. }, {}, 100);

    REQUIRE(bodies.size() == 2);
    CHECK_THAT(bodies[0].mass, WithinRel(1000., 1e-4));
    CHECK_THAT((bodies[0].center - Vec3d(5., 5., 5.)).norm(), WithinAbs(0., 1e-4));
    CHECK_THAT(bodies[1].mass, WithinRel(1000., 1e-4));
    CHECK_THAT((bodies[1].center - Vec3d(25., 5., 5.)).norm(), WithinAbs(0., 1e-4));
}

TEST_CASE("Overlapping solids are one body that counts the overlap once", "[ConnectedBodies]")
{
    const indexed_triangle_set cube   = its_make_cube(10., 10., 10.);
    const auto                 bodies = solid_bodies({ { &cube, Transform3d::Identity() }, { &cube, Geometry::translation_transform({ 5., 0., 0. }) } }, { 1., 1. }, {}, 100);

    // Their union is a 15 x 10 x 10 box, which spreads a^2 / 12 along each side a.
    REQUIRE(bodies.size() == 1);
    const SolidBody &body = bodies.front();
    CHECK_THAT(body.mass, WithinRel(1500., 1e-4));
    CHECK_THAT(body.volume, WithinRel(1500., 1e-4));
    CHECK_THAT((body.center - Vec3d(7.5, 5., 5.)).norm(), WithinAbs(0., 1e-4));
    const Matrix3d spread = Vec3d(225., 100., 100.).asDiagonal() * (1. / 12.);
    CHECK_THAT((body.spread - spread).norm(), WithinAbs(0., 1e-4));

    // Turned a quarter about z, the box spans what was its y in -x.
    const BoundingBoxf3 box = body.bounding_box(Geometry::rotation_transform({ 0., 0., 0.5 * PI }));
    CHECK_THAT((box.min - Vec3d(-10., 0., 0.)).norm(), WithinAbs(0., 1e-4));
    CHECK_THAT((box.max - Vec3d(0., 15., 10.)).norm(), WithinAbs(0., 1e-4));
}

TEST_CASE("A negative solid is cut away from the body", "[ConnectedBodies]")
{
    const indexed_triangle_set cube     = its_make_cube(10., 10., 10.);
    const indexed_triangle_set notch    = its_make_cube(4., 4., 4.);
    const auto                 bodies   = solid_bodies({ { &cube, Transform3d::Identity() } }, { 1. }, { { &notch, Transform3d::Identity() } }, 100);
    // A 10 mm cube centered at 5 less a 4 mm cube centered at 2, in each axis alike.
    const double               expected = (1000. * 5. - 64. * 2.) / (1000. - 64.);

    REQUIRE(bodies.size() == 1);
    CHECK_THAT(bodies[0].mass, WithinRel(1000. - 64., 1e-4));
    CHECK_THAT((bodies[0].center - Vec3d(expected, expected, expected)).norm(), WithinAbs(0., 1e-4));
}

TEST_CASE("Each solid weighs its density, the later of two overlapping ones the overlap", "[ConnectedBodies]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    const MeshInPlace          left{ &cube, Transform3d::Identity() };
    const MeshInPlace          right{ &cube, Geometry::translation_transform({ 5., 0., 0. }) };

    // The right cube, three times as dense, prints the overlap from x 5 to 10.
    auto bodies = solid_bodies({ left, right }, { 1., 3. }, {}, 100);
    REQUIRE(bodies.size() == 1);
    CHECK_THAT(bodies[0].mass, WithinRel(500. + 3. * 1000., 1e-4));
    CHECK_THAT(bodies[0].volume, WithinRel(1500., 1e-4));
    CHECK_THAT(bodies[0].center.x(), WithinAbs((500. * 2.5 + 3000. * 10.) / 3500., 1e-4));

    // Listed the other way round, the left cube prints it.
    bodies = solid_bodies({ right, left }, { 3., 1. }, {}, 100);
    REQUIRE(bodies.size() == 1);
    CHECK_THAT(bodies[0].mass, WithinRel(1000. + 3. * 500., 1e-4));
    CHECK_THAT(bodies[0].center.x(), WithinAbs((1000. * 5. + 1500. * 12.5) / 2500., 1e-4));
}

TEST_CASE("Separate solids weigh their own densities", "[ConnectedBodies]")
{
    const indexed_triangle_set cube   = its_make_cube(10., 10., 10.);
    const auto                 bodies = solid_bodies({ { &cube, Transform3d::Identity() }, { &cube, Geometry::translation_transform({ 20., 0., 0. }) } },
                                                     { 1.24, 2. }, {}, 100);

    REQUIRE(bodies.size() == 2);
    CHECK_THAT(bodies[0].mass, WithinRel(1240., 1e-4));
    CHECK_THAT(bodies[1].mass, WithinRel(2000., 1e-4));
}
