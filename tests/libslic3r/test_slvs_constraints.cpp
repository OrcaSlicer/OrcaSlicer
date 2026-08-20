#include <catch2/catch_all.hpp>   // mainline OrcaSlicer ships Catch2 v3 (v2 was catch2/catch.hpp)
using Catch::Approx;   // v3 scopes Approx into the Catch namespace; v2 had it at global scope

#include "libslic3r/CAD/SketchSolver.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"

using namespace Slic3r;
using CT = SketchConstraintType;
using R  = SketchPointRole;

static SketchEntity line(Vec2d a, Vec2d b)
{
    SketchEntity e; e.type = SketchEntity::Type::Line; e.p0 = a; e.p1 = b; return e;
}
static SketchEntity circle(Vec2d c, double r)
{
    SketchEntity e; e.type = SketchEntity::Type::Circle; e.center = c; e.p0 = c; e.radius = r; return e;
}
static SketchEntityConstraintDef con(CT t, int ea, R ra, int eb, R rb, double v = 0.0)
{
    SketchEntityConstraintDef c; c.type = t; c.ea = ea; c.ra = ra; c.eb = eb; c.rb = rb; c.value = v; return c;
}

TEST_CASE("slvs: distance + horizontal + fix solves a line length", "[slvs]")
{
    std::vector<SketchEntity> ents = { line({0, 0}, {5, 1}) };
    std::vector<SketchEntityConstraintDef> cons = {
        con(CT::Fix,        0, R::P0, 0, R::P0),
        con(CT::Horizontal, 0, R::P0, 0, R::P1),
        con(CT::Distance,   0, R::P0, 0, R::P1, 10.0),
    };
    auto res = sketch_solve(ents, cons);
    REQUIRE(res.ok);
    CHECK((ents[0].p1 - ents[0].p0).norm() == Approx(10.0).margin(1e-6));
    CHECK(ents[0].p0.x() == Approx(0.0).margin(1e-6));
    CHECK(ents[0].p0.y() == Approx(0.0).margin(1e-6));
    CHECK(ents[0].p1.y() == Approx(0.0).margin(1e-6));   // horizontal
}

TEST_CASE("slvs: coincident joins two line endpoints (loop closes)", "[slvs]")
{
    std::vector<SketchEntity> ents = { line({0, 0}, {10, 0}), line({10.3, 0.2}, {10, 10}) };
    std::vector<SketchEntityConstraintDef> cons = {
        con(CT::Coincident, 0, R::P1, 1, R::P0),
    };
    auto res = sketch_solve(ents, cons);
    REQUIRE(res.ok);
    CHECK((ents[0].p1 - ents[1].p0).norm() == Approx(0.0).margin(1e-6));
}

TEST_CASE("slvs: parallel + perpendicular on lines", "[slvs]")
{
    std::vector<SketchEntity> ents = { line({0, 0}, {10, 1}), line({0, 5}, {10, 5.5}), line({0, 0}, {0.5, 10}) };
    std::vector<SketchEntityConstraintDef> cons = {
        con(CT::Fix,           0, R::P0, 0, R::P0),
        con(CT::Horizontal,    0, R::P0, 0, R::P1),
        con(CT::Parallel,      0, R::P0, 1, R::P0),     // line1 parallel to line0
        con(CT::Perpendicular, 0, R::P0, 2, R::P0),     // line2 perpendicular to line0
    };
    auto res = sketch_solve(ents, cons);
    REQUIRE(res.ok);
    CHECK(ents[1].p1.y() - ents[1].p0.y() == Approx(0.0).margin(1e-6));            // line1 horizontal
    CHECK(ents[2].p1.x() - ents[2].p0.x() == Approx(0.0).margin(1e-6));            // line2 vertical
}

TEST_CASE("slvs: circle radius constraint", "[slvs]")
{
    std::vector<SketchEntity> ents = { circle({2, 2}, 3.0) };
    std::vector<SketchEntityConstraintDef> cons = { con(CT::Radius, 0, R::P0, -1, R::P0, 7.0) };
    auto res = sketch_solve(ents, cons);
    REQUIRE(res.ok);
    CHECK(ents[0].radius == Approx(7.0).margin(1e-6));
}

TEST_CASE("slvs: degrees of freedom reported", "[slvs]")
{
    // One free line with only a Fix on the start: 4 DoF total minus 2 (fix) = 2 remaining.
    std::vector<SketchEntity> ents = { line({0, 0}, {3, 4}) };
    std::vector<SketchEntityConstraintDef> cons = { con(CT::Fix, 0, R::P0, 0, R::P0) };
    auto res = sketch_solve(ents, cons);
    REQUIRE(res.ok);
    CHECK(res.dof == 2);
}

TEST_CASE("slvs: drag pulls a point while constraints hold", "[slvs]")
{
    // A vertical line of fixed length 10, P0 pinned at the origin. Dragging P1 toward
    // (10,0) must keep the length (Distance constraint) but rotate the line so the end
    // follows the cursor into positive x — the dragged param wins the under-constrained DoF.
    std::vector<SketchEntity> ents = { line({0, 0}, {0, 10}) };
    std::vector<SketchEntityConstraintDef> cons = {
        con(CT::Fix,      0, R::P0, 0, R::P0),
        con(CT::Distance, 0, R::P0, 0, R::P1, 10.0),
    };
    ents[0].p1 = Vec2d(10, 0);   // user dropped the endpoint here
    auto res = sketch_solve_drag(ents, cons, 0, R::P1);
    REQUIRE(res.ok);
    CHECK((ents[0].p1 - ents[0].p0).norm() == Approx(10.0).margin(1e-6));  // length held
    CHECK(ents[0].p0.x() == Approx(0.0).margin(1e-6));                     // P0 still pinned
    CHECK(ents[0].p0.y() == Approx(0.0).margin(1e-6));
    CHECK(ents[0].p1.x() > 1.0);   // end followed the drag toward +x (not stuck vertical)
}

TEST_CASE("slvs: over-constrained / inconsistent is detected", "[slvs]")
{
    std::vector<SketchEntity> ents = { line({0, 0}, {5, 0}) };
    std::vector<SketchEntityConstraintDef> cons = {
        con(CT::Fix,      0, R::P0, 0, R::P0),
        con(CT::Fix,      0, R::P1, 0, R::P1),
        con(CT::Distance, 0, R::P0, 0, R::P1, 99.0),   // contradicts the pinned endpoints
    };
    auto res = sketch_solve(ents, cons);
    CHECK_FALSE(res.ok);   // SLVS_RESULT_INCONSISTENT
}
