#include <catch2/catch_all.hpp>   // mainline OrcaSlicer ships Catch2 v3 (v2 was catch2/catch.hpp)
#include "libslic3r/CAD/SketchConstraints.hpp"

using namespace Slic3r;

TEST_CASE("Coincident with anchor", "[SketchConstraints]")
{
    SketchConstraints sc;
    int a = sc.add_point(0, 0);
    int b = sc.add_point(5, 5);
    sc.fix_point(a);
    sc.coincident(a, b);
    REQUIRE(sc.solve());
    Vec2d pb = sc.get_point(b);
    REQUIRE_THAT(pb.x(), Catch::Matchers::WithinAbs(0.0, 1e-4));
    REQUIRE_THAT(pb.y(), Catch::Matchers::WithinAbs(0.0, 1e-4));
}

TEST_CASE("Horizontal + distance", "[SketchConstraints]")
{
    SketchConstraints sc;
    int a = sc.add_point(0, 0);
    int b = sc.add_point(5, 3);
    sc.fix_point(a);
    sc.horizontal(a, b);
    sc.distance(a, b, 10);
    REQUIRE(sc.solve());
    Vec2d pb = sc.get_point(b);
    REQUIRE_THAT(pb.y(), Catch::Matchers::WithinAbs(0.0, 1e-3));
    REQUIRE_THAT(std::abs(pb.x()), Catch::Matchers::WithinAbs(10.0, 1e-3));
}

TEST_CASE("Rectangle", "[SketchConstraints]")
{
    SketchConstraints sc;
    int p0 = sc.add_point(0, 0);
    int p1 = sc.add_point(8, 1);
    int p2 = sc.add_point(9, 5);
    int p3 = sc.add_point(-1, 4);
    sc.fix_point(p0);
    sc.lock_x(p0, 0);
    sc.lock_y(p0, 0);
    sc.horizontal(p0, p1);
    sc.vertical(p1, p2);
    sc.horizontal(p2, p3);
    sc.vertical(p3, p0);
    sc.distance(p0, p1, 10);
    sc.distance(p1, p2, 6);
    REQUIRE(sc.solve());
    Vec2d pp1 = sc.get_point(p1);
    Vec2d pp2 = sc.get_point(p2);
    Vec2d pp3 = sc.get_point(p3);
    REQUIRE_THAT(pp1.x(), Catch::Matchers::WithinAbs(10.0, 1e-3));
    REQUIRE_THAT(pp1.y(), Catch::Matchers::WithinAbs(0.0, 1e-3));
    REQUIRE_THAT(pp2.x(), Catch::Matchers::WithinAbs(10.0, 1e-3));
    REQUIRE_THAT(pp2.y(), Catch::Matchers::WithinAbs(6.0, 1e-3));
    REQUIRE_THAT(pp3.x(), Catch::Matchers::WithinAbs(0.0, 1e-3));
    REQUIRE_THAT(pp3.y(), Catch::Matchers::WithinAbs(6.0, 1e-3));
}

TEST_CASE("residual_norm after each solve", "[SketchConstraints]")
{
    SECTION("coincident case")
    {
        SketchConstraints sc;
        int a = sc.add_point(0, 0);
        int b = sc.add_point(5, 5);
        sc.fix_point(a);
        sc.coincident(a, b);
        REQUIRE(sc.solve());
        REQUIRE(sc.residual_norm() < 1e-5);
    }
    SECTION("horizontal+distance case")
    {
        SketchConstraints sc;
        int a = sc.add_point(0, 0);
        int b = sc.add_point(5, 3);
        sc.fix_point(a);
        sc.horizontal(a, b);
        sc.distance(a, b, 10);
        REQUIRE(sc.solve());
        REQUIRE(sc.residual_norm() < 1e-5);
    }
    SECTION("rectangle case")
    {
        SketchConstraints sc;
        int p0 = sc.add_point(0, 0);
        int p1 = sc.add_point(8, 1);
        int p2 = sc.add_point(9, 5);
        int p3 = sc.add_point(-1, 4);
        sc.fix_point(p0);
        sc.lock_x(p0, 0);
        sc.lock_y(p0, 0);
        sc.horizontal(p0, p1);
        sc.vertical(p1, p2);
        sc.horizontal(p2, p3);
        sc.vertical(p3, p0);
        sc.distance(p0, p1, 10);
        sc.distance(p1, p2, 6);
        REQUIRE(sc.solve());
        REQUIRE(sc.residual_norm() < 1e-5);
    }
}

TEST_CASE("midpoint", "[SketchConstraints]")
{
    SketchConstraints sc;
    int a = sc.add_point(0, 0);
    int b = sc.add_point(10, 0);
    int m = sc.add_point(3, 7);
    sc.fix_point(a);
    sc.fix_point(b);
    sc.midpoint(m, a, b);
    REQUIRE(sc.solve());
    Vec2d pm = sc.get_point(m);
    REQUIRE_THAT(pm.x(), Catch::Matchers::WithinAbs(5.0, 1e-3));
    REQUIRE_THAT(pm.y(), Catch::Matchers::WithinAbs(0.0, 1e-3));
}

TEST_CASE("symmetric across Y axis", "[SketchConstraints]")
{
    SketchConstraints sc;
    int a = sc.add_point(2, 3);
    int b = sc.add_point(-1, 1);
    int c = sc.add_point(0, 0);
    int d = sc.add_point(0, 1);
    sc.fix_point(a);
    sc.fix_point(c);
    sc.fix_point(d);
    sc.symmetric(a, b, c, d);
    REQUIRE(sc.solve());
    Vec2d pb = sc.get_point(b);
    REQUIRE_THAT(pb.x(), Catch::Matchers::WithinAbs(-2.0, 1e-3));
    REQUIRE_THAT(pb.y(), Catch::Matchers::WithinAbs(3.0, 1e-3));
}

TEST_CASE("angle 90 degrees", "[SketchConstraints]")
{
    SketchConstraints sc;
    int a = sc.add_point(0, 0);
    int b = sc.add_point(1, 0);
    int c = sc.add_point(0, 0);
    int d = sc.add_point(1, 1);
    sc.fix_point(a);
    sc.fix_point(b);
    sc.fix_point(c);
    sc.angle(a, b, c, d, M_PI / 2);
    REQUIRE(sc.solve());
    Vec2d pd = sc.get_point(d);
    Vec2d pc = sc.get_point(c);
    REQUIRE_THAT(pd.x() - pc.x(), Catch::Matchers::WithinAbs(0.0, 1e-3));
    REQUIRE(pd.y() > pc.y());
}

TEST_CASE("point-line distance", "[SketchConstraints]")
{
    SketchConstraints sc;
    int a = sc.add_point(0, 0);
    int b = sc.add_point(10, 0);
    int p = sc.add_point(3, 1);
    sc.fix_point(a);
    sc.fix_point(b);
    sc.lock_x(p, 3.0);
    sc.point_line_distance(p, a, b, 5.0);
    REQUIRE(sc.solve());
    Vec2d pp = sc.get_point(p);
    REQUIRE_THAT(std::abs(pp.y()), Catch::Matchers::WithinAbs(5.0, 1e-3));
    REQUIRE_THAT(pp.x(), Catch::Matchers::WithinAbs(3.0, 1e-3));
}
