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

TEST_CASE("slvs: equal radius drives two circles to one radius", "[slvs][CadDocument]")
{
    std::vector<SketchEntity> ents = { circle({0, 0}, 5.0), circle({10, 0}, 12.0) };
    std::vector<SketchEntityConstraintDef> cons = {
        con(CT::EqualRadius, 0, R::P0, 1, R::P0),
    };
    auto res = sketch_solve(ents, cons);
    REQUIRE(res.ok);
    CHECK(ents[0].radius == Approx(ents[1].radius).margin(1e-9));
    CHECK(ents[0].radius > 1e-6);   // equal-at-zero would satisfy the line above trivially
}

TEST_CASE("slvs: equal radius plus a radius dimension pins both", "[slvs][CadDocument]")
{
    std::vector<SketchEntity> ents = { circle({0, 0}, 5.0), circle({10, 0}, 12.0) };
    std::vector<SketchEntityConstraintDef> cons = {
        con(CT::EqualRadius, 0, R::P0, 1, R::P0),
        con(CT::Radius, 0, R::P0, -1, R::P0, 8.0),
    };
    auto res = sketch_solve(ents, cons);
    REQUIRE(res.ok);
    CHECK(ents[0].radius == Approx(8.0).margin(1e-9));
    CHECK(ents[1].radius == Approx(8.0).margin(1e-9));
}

TEST_CASE("slvs: collinear makes two offset lines share one line", "[slvs][CadDocument]")
{
    std::vector<SketchEntity> ents = { line({0, 0}, {10, 0}), line({0, 4}, {10, 4}) };
    std::vector<SketchEntityConstraintDef> cons = {
        con(CT::Collinear, 0, R::P0, 1, R::P0),
    };
    auto res = sketch_solve(ents, cons);
    REQUIRE(res.ok);
    const Vec2d& a0 = ents[0].p0;
    const Vec2d  ad = ents[0].p1 - ents[0].p0;
    for (int k = 0; k <= 1; ++k) {
        const Vec2d& pk = (k == 0) ? ents[1].p0 : ents[1].p1;
        const double cross = ad.x() * (pk.y() - a0.y()) - ad.y() * (pk.x() - a0.x());
        CHECK(cross == Approx(0.0).margin(1e-9));
    }
    // A line collapsed to a point is trivially collinear with anything, so the cross
    // products above would pass on a degenerate solve. Both lines must survive intact.
    CHECK(ad.norm() == Approx(10.0).margin(1e-9));
    CHECK((ents[1].p1 - ents[1].p0).norm() == Approx(10.0).margin(1e-9));
}

TEST_CASE("slvs: collinear on already-collinear lines moves nothing", "[slvs][CadDocument]")
{
    std::vector<SketchEntity> ents = { line({0, 0}, {10, 0}), line({20, 0}, {30, 0}) };
    std::vector<SketchEntityConstraintDef> cons = {
        con(CT::Collinear, 0, R::P0, 1, R::P0),
    };
    std::vector<SketchEntity> before = ents;
    auto res = sketch_solve(ents, cons);
    REQUIRE(res.ok);
    for (size_t i = 0; i < ents.size(); ++i) {         // already satisfied: nothing may move
        CHECK(ents[i].p0.x() == Approx(before[i].p0.x()).margin(1e-9));
        CHECK(ents[i].p0.y() == Approx(before[i].p0.y()).margin(1e-9));
        CHECK(ents[i].p1.x() == Approx(before[i].p1.x()).margin(1e-9));
        CHECK(ents[i].p1.y() == Approx(before[i].p1.y()).margin(1e-9));
    }
}

TEST_CASE("slvs: distance-x drives the horizontal gap and leaves Y alone", "[slvs][CadDocument]")
{
    std::vector<SketchEntity> ents = { line({0, 0}, {3, 7}) };
    std::vector<SketchEntityConstraintDef> cons = {
        con(CT::Fix,       0, R::P0, 0, R::P0),
        con(CT::DistanceX, 0, R::P0, 0, R::P1, 10.0),
    };
    auto res = sketch_solve(ents, cons);
    REQUIRE(res.ok);
    // SIGNED, not abs. PROJ_PT_DISTANCE constrains (pB - pA).dot(unit(dir)), and a
    // LINE_SEGMENT's direction is point[0] - point[1] (slvs entity.cpp), so the reference
    // line is built head-first to mean +X. Assert on abs and a flipped reference passes
    // while every dimension lands the point on the wrong side of its anchor.
    CHECK(ents[0].p1.x() - ents[0].p0.x() == Approx(10.0).margin(1e-9));
    CHECK(ents[0].p1.y() == Approx(7.0).margin(1e-9));   // Y must not be disturbed
}

TEST_CASE("slvs: distance-y drives the vertical gap and leaves X alone", "[slvs][CadDocument]")
{
    std::vector<SketchEntity> ents = { line({0, 0}, {3, 7}) };
    std::vector<SketchEntityConstraintDef> cons = {
        con(CT::Fix,       0, R::P0, 0, R::P0),
        con(CT::DistanceY, 0, R::P0, 0, R::P1, 10.0),
    };
    auto res = sketch_solve(ents, cons);
    REQUIRE(res.ok);
    CHECK(ents[0].p1.y() - ents[0].p0.y() == Approx(10.0).margin(1e-9));   // signed: see above
    CHECK(ents[0].p1.x() == Approx(3.0).margin(1e-9));   // X must not be disturbed
}

TEST_CASE("slvs: distance-x is not the straight-line distance", "[slvs][CadDocument]")
{
    // B is at straight-line distance 10 from A; DistanceX = 6 is already satisfied, so a
    // correct projection leaves B untouched. This is the case that fails if the constraint
    // were wired to SLVS_C_PT_PT_DISTANCE, which would drag B onto the radius-6 circle.
    std::vector<SketchEntity> ents = { line({0, 0}, {6, 8}) };
    std::vector<SketchEntityConstraintDef> cons = {
        con(CT::Fix,       0, R::P0, 0, R::P0),
        con(CT::DistanceX, 0, R::P0, 0, R::P1, 6.0),
    };
    auto res = sketch_solve(ents, cons);
    REQUIRE(res.ok);
    CHECK(ents[0].p1.x() == Approx(6.0).margin(1e-9));
    CHECK(ents[0].p1.y() == Approx(8.0).margin(1e-9));
}

TEST_CASE("slvs: distance-x plus distance-y fully locates a point", "[slvs][CadDocument]")
{
    std::vector<SketchEntity> ents = { line({0, 0}, {1, 1}) };
    std::vector<SketchEntityConstraintDef> cons = {
        con(CT::Fix,       0, R::P0, 0, R::P0),
        con(CT::DistanceX, 0, R::P0, 0, R::P1, 4.0),
        con(CT::DistanceY, 0, R::P0, 0, R::P1, 3.0),
    };
    auto res = sketch_solve(ents, cons);
    REQUIRE(res.ok);
    CHECK(ents[0].p1.x() - ents[0].p0.x() == Approx(4.0).margin(1e-9));   // signed: see above
    CHECK(ents[0].p1.y() - ents[0].p0.y() == Approx(3.0).margin(1e-9));
}

// The property the GUI's ref-ordering exists to preserve: DistanceX is SIGNED, so applying
// the CURRENT projected delta as the target must not move anything. If the refs are ordered
// so the shown value is positive while the actual signed delta is negative, accepting the
// value a dimension opens with teleports the point to the other side of its anchor.
TEST_CASE("slvs: applying a point's own distance-x is a no-op", "[slvs][CadDocument]")
{
    // p1 sits to the LEFT of p0, so the signed delta p1 - p0 is negative.
    std::vector<SketchEntity> ents = { line({0, 0}, {-4, 7}) };
    std::vector<SketchEntityConstraintDef> cons = {
        con(CT::Fix,       0, R::P0, 0, R::P0),
        con(CT::DistanceX, 0, R::P0, 0, R::P1, -4.0),   // the CURRENT signed delta
    };
    auto res = sketch_solve(ents, cons);
    REQUIRE(res.ok);
    CHECK(ents[0].p1.x() == Approx(-4.0).margin(1e-9));   // stayed left, did not flip to +4
    CHECK(ents[0].p1.y() == Approx(7.0).margin(1e-9));
}
