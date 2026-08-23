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

// snaporca-yww4. libslvs sizes its System with a compile-time `MAX_UNKNOWNS = 1024`, and the
// solver is handed every entity in the sketch at 2 params per point — so a sketch of about 480
// lines is the last one that fits and the next comes back TOO_MANY_UNKNOWNS. Because
// try_add_constraints rolls a failed batch back, that turned into: every auto-inferred constraint
// on a large sketch silently dropped, and from then on no dimension could ever be applied to it.
// Constraints only couple entities that share a point, so the sketch is solved component by
// component when the whole system does not fit.
TEST_CASE("slvs: a sketch past the solver's unknown limit still solves", "[slvs]")
{
    // 300 disjoint squares: 1200 lines, 4800 unknowns whole, 8 per component.
    const int N = 300;
    std::vector<SketchEntity> ents;
    std::vector<SketchEntityConstraintDef> cons;
    for (int i = 0; i < N; ++i) {
        const double x = (i % 30) * 10.0, y = (i / 30) * 10.0;
        const int b = int(ents.size());
        ents.push_back(line({x, y},         {x + 4.0, y}));
        ents.push_back(line({x + 4.0, y},   {x + 4.0, y + 4.0}));
        ents.push_back(line({x + 4.0, y + 4.0}, {x, y + 4.0}));
        ents.push_back(line({x, y + 4.0},   {x, y}));
        for (int k = 0; k < 4; ++k)
            cons.push_back(con(CT::Coincident, b + k, R::P1, b + (k + 1) % 4, R::P0));
    }
    REQUIRE(ents.size() == size_t(4 * N));

    std::vector<SketchEntity> before = ents;
    auto res = sketch_solve(ents, cons);
    REQUIRE(res.ok);
    for (size_t i = 0; i < ents.size(); ++i) {         // already satisfied: nothing may move
        CHECK(ents[i].p0.x() == Approx(before[i].p0.x()).margin(1e-9));
        CHECK(ents[i].p0.y() == Approx(before[i].p0.y()).margin(1e-9));
        CHECK(ents[i].p1.x() == Approx(before[i].p1.x()).margin(1e-9));
        CHECK(ents[i].p1.y() == Approx(before[i].p1.y()).margin(1e-9));
    }

    // And a dimension typed onto one of them lands exactly, which is what stopped working.
    cons.push_back(con(CT::Distance, 0, R::P0, 0, R::P1, 7.0));
    auto res2 = sketch_solve(ents, cons);
    REQUIRE(res2.ok);
    CHECK((ents[0].p1 - ents[0].p0).norm() == Approx(7.0).margin(1e-9));

    // A conflict inside ONE component must still be caught, not swallowed by the split.
    cons.push_back(con(CT::Distance, 0, R::P0, 0, R::P1, 99.0));
    auto res3 = sketch_solve(ents, cons);
    CHECK_FALSE(res3.ok);
}
