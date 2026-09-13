#include <catch2/catch_all.hpp>   // mainline OrcaSlicer ships Catch2 v3 (v2 was catch2/catch.hpp)
#include "libslic3r/CAD/SketchConstraints.hpp"
#include "libslic3r/CAD/SketchEngine.hpp"

using namespace Slic3r;

namespace {
SketchEntity mk_line(double x0, double y0, double x1, double y1)
{
    SketchEntity e; e.type = SketchEntity::Type::Line;
    e.p0 = Vec2d(x0, y0); e.p1 = Vec2d(x1, y1); return e;
}
SketchEntity mk_point(double x, double y)
{
    SketchEntity e; e.type = SketchEntity::Type::Point; e.p0 = Vec2d(x, y); return e;
}
SketchEntity mk_circle(double cx, double cy, double r)
{
    SketchEntity e; e.type = SketchEntity::Type::Circle;
    e.center = Vec2d(cx, cy); e.radius = r; return e;
}
}

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

// ---- entity-constraint planner (kernel port of DesignPanel::apply_entity_constraint) ----

TEST_CASE("sketch_entity_ends exposes real roles only", "[SketchConstraints]")
{
    std::pair<SketchPointRole, Vec2d> out[2];
    REQUIRE(sketch_entity_ends(mk_point(3, 4), out) == 1);
    REQUIRE(out[0].first == SketchPointRole::P0);
    REQUIRE(sketch_entity_ends(mk_circle(1, 2, 5), out) == 1);
    REQUIRE(out[0].first == SketchPointRole::Center);
    REQUIRE_THAT(out[0].second.x(), Catch::Matchers::WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(out[0].second.y(), Catch::Matchers::WithinAbs(2.0, 1e-9));
    REQUIRE(sketch_entity_ends(mk_line(0, 0, 10, 0), out) == 2);
    REQUIRE(out[0].first == SketchPointRole::P0);
    REQUIRE(out[1].first == SketchPointRole::P1);
}

TEST_CASE("Coincident on two Points binds P0/P0, not phantom p1", "[SketchConstraints]")
{
    std::vector<SketchEntity> ents = { mk_point(0, 0), mk_point(5, 5) };
    ConstraintPlan p = plan_entity_constraint(ents, 0, 1, -1, SketchConstraintType::Coincident);
    REQUIRE(p.kind == ConstraintPlan::Kind::Apply);
    REQUIRE(p.defs.size() == 1);
    REQUIRE(p.defs[0].type == SketchConstraintType::Coincident);
    REQUIRE(p.defs[0].ea == 0);
    REQUIRE(p.defs[0].ra == SketchPointRole::P0);
    REQUIRE(p.defs[0].eb == 1);
    REQUIRE(p.defs[0].rb == SketchPointRole::P0);
}

TEST_CASE("DistanceX on two Points binds real roles with non-negative prefill", "[SketchConstraints]")
{
    // e0 is right of e1, so the raw projected delta is negative: the plan must swap the
    // refs so accepting the shown (positive) value is a no-op, not a sign flip.
    std::vector<SketchEntity> ents = { mk_point(5, 1), mk_point(2, 3) };
    ConstraintPlan p = plan_entity_constraint(ents, 0, 1, -1, SketchConstraintType::DistanceX);
    REQUIRE(p.kind == ConstraintPlan::Kind::AskValue);
    REQUIRE(p.defs.size() == 1);
    REQUIRE(p.defs[0].type == SketchConstraintType::DistanceX);
    REQUIRE(p.defs[0].ra == SketchPointRole::P0);
    REQUIRE(p.defs[0].rb == SketchPointRole::P0);
    REQUIRE(p.prefill >= 0.0);
    REQUIRE(p.defs[0].ea == 1);
    REQUIRE(p.defs[0].eb == 0);
}

TEST_CASE("Horizontal on a Point rejects with NeedALine", "[SketchConstraints]")
{
    std::vector<SketchEntity> ents = { mk_point(1, 2) };
    ConstraintPlan p = plan_entity_constraint(ents, 0, -1, -1, SketchConstraintType::Horizontal);
    REQUIRE(p.kind == ConstraintPlan::Kind::Reject);
    REQUIRE(p.reason == ConstraintReject::NeedALine);
}

TEST_CASE("Angle on two Circles rejects with NeedTwoLines", "[SketchConstraints]")
{
    std::vector<SketchEntity> ents = { mk_circle(0, 0, 1), mk_circle(5, 0, 1) };
    ConstraintPlan p = plan_entity_constraint(ents, 0, 1, -1, SketchConstraintType::Angle);
    REQUIRE(p.kind == ConstraintPlan::Kind::Reject);
    REQUIRE(p.reason == ConstraintReject::NeedTwoLines);
}

TEST_CASE("Parallel on a Line + Circle rejects with NeedTwoLines (new guard)", "[SketchConstraints]")
{
    std::vector<SketchEntity> ents = { mk_line(0, 0, 1, 0), mk_circle(5, 0, 1) };
    ConstraintPlan p = plan_entity_constraint(ents, 0, 1, -1, SketchConstraintType::Parallel);
    REQUIRE(p.kind == ConstraintPlan::Kind::Reject);
    REQUIRE(p.reason == ConstraintReject::NeedTwoLines);
}

TEST_CASE("Equal on two Circles promotes to EqualRadius", "[SketchConstraints]")
{
    std::vector<SketchEntity> ents = { mk_circle(0, 0, 1), mk_circle(5, 0, 2) };
    ConstraintPlan p = plan_entity_constraint(ents, 0, 1, -1, SketchConstraintType::EqualLength);
    REQUIRE(p.kind == ConstraintPlan::Kind::Apply);
    REQUIRE(p.defs.size() == 1);
    REQUIRE(p.defs[0].type == SketchConstraintType::EqualRadius);
    REQUIRE(p.defs[0].ea == 0);
    REQUIRE(p.defs[0].eb == 1);
}

TEST_CASE("Symmetric on two Lines returns two defs with ec set to the axis", "[SketchConstraints]")
{
    std::vector<SketchEntity> ents = { mk_line(0, 1, 5, 1), mk_line(0, -1, 5, -1), mk_line(0, 0, 0, 1) };
    ConstraintPlan p = plan_entity_constraint(ents, 0, 1, 2, SketchConstraintType::Symmetric);
    REQUIRE(p.kind == ConstraintPlan::Kind::Apply);
    REQUIRE(p.defs.size() == 2);
    for (const auto& d : p.defs) {
        REQUIRE(d.type == SketchConstraintType::Symmetric);
        REQUIRE(d.ea == 0);
        REQUIRE(d.eb == 1);
        REQUIRE(d.ec == 2);
    }
    REQUIRE(p.defs[0].ra == SketchPointRole::P0);
    REQUIRE(p.defs[0].rb == SketchPointRole::P0);
    REQUIRE(p.defs[1].ra == SketchPointRole::P1);
    REQUIRE(p.defs[1].rb == SketchPointRole::P1);
}

TEST_CASE("Symmetric with no axis rejects with NeedAxisLine", "[SketchConstraints]")
{
    std::vector<SketchEntity> ents = { mk_point(0, 0), mk_point(5, 0) };
    ConstraintPlan p = plan_entity_constraint(ents, 0, 1, -1, SketchConstraintType::Symmetric);
    REQUIRE(p.kind == ConstraintPlan::Kind::Reject);
    REQUIRE(p.reason == ConstraintReject::NeedAxisLine);
}

TEST_CASE("SymmetricAboutY on two Points returns one def with ec == kSketchRefAxisY", "[SketchConstraints]")
{
    std::vector<SketchEntity> ents = { mk_point(1, 0), mk_point(-2, 0) };
    ConstraintPlan p = plan_entity_constraint(ents, 0, 1, -1, SketchConstraintType::SymmetricAboutY);
    REQUIRE(p.kind == ConstraintPlan::Kind::Apply);
    REQUIRE(p.defs.size() == 1);
    REQUIRE(p.defs[0].type == SketchConstraintType::SymmetricAboutY);
    REQUIRE(p.defs[0].ec == kSketchRefAxisY);
    REQUIRE(p.defs[0].ea == 0);
    REQUIRE(p.defs[0].eb == 1);
}

TEST_CASE("constraint planner apply/askvalue matrix", "[SketchConstraints]")
{
    struct C {
        const char* name; SketchConstraintType type; std::vector<SketchEntity> ents;
        int e0, e1, e2; ConstraintPlan::Kind kind;
    };
    const std::vector<C> cases = {
        { "Fix",              SketchConstraintType::Fix,              { mk_point(1, 2) },                      0, -1, -1, ConstraintPlan::Kind::Apply },
        { "Coincident",       SketchConstraintType::Coincident,       { mk_point(0, 0), mk_point(5, 5) },      0,  1, -1, ConstraintPlan::Kind::Apply },
        { "Horizontal",       SketchConstraintType::Horizontal,       { mk_line(0, 0, 5, 0) },                 0, -1, -1, ConstraintPlan::Kind::Apply },
        { "Vertical",         SketchConstraintType::Vertical,         { mk_line(0, 0, 0, 5) },                 0, -1, -1, ConstraintPlan::Kind::Apply },
        { "Parallel",         SketchConstraintType::Parallel,         { mk_line(0, 0, 1, 0), mk_line(0, 1, 1, 1) }, 0, 1, -1, ConstraintPlan::Kind::Apply },
        { "Perpendicular",    SketchConstraintType::Perpendicular,    { mk_line(0, 0, 1, 0), mk_line(0, 0, 0, 1) }, 0, 1, -1, ConstraintPlan::Kind::Apply },
        { "EqualLength",      SketchConstraintType::EqualLength,      { mk_line(0, 0, 1, 0), mk_line(0, 1, 2, 1) }, 0, 1, -1, ConstraintPlan::Kind::Apply },
        { "Concentric",       SketchConstraintType::Concentric,       { mk_circle(0, 0, 1), mk_circle(5, 0, 1) }, 0, 1, -1, ConstraintPlan::Kind::Apply },
        { "Tangent",          SketchConstraintType::Tangent,          { mk_line(0, 0, 1, 0), mk_circle(0, 1, 1) }, 0, 1, -1, ConstraintPlan::Kind::Apply },
        { "Midpoint",         SketchConstraintType::Midpoint,         { mk_point(2, 0), mk_line(0, 0, 5, 0) },   0, 1, -1, ConstraintPlan::Kind::Apply },
        { "Symmetric",        SketchConstraintType::Symmetric,        { mk_point(0, 0), mk_point(5, 0), mk_line(0, -1, 0, 1) }, 0, 1, 2, ConstraintPlan::Kind::Apply },
        { "SymmetricAboutY",  SketchConstraintType::SymmetricAboutY,  { mk_point(1, 0), mk_point(-2, 0) },       0, 1, -1, ConstraintPlan::Kind::Apply },
        { "SymmetricAboutX",  SketchConstraintType::SymmetricAboutX,  { mk_point(0, 1), mk_point(0, -2) },       0, 1, -1, ConstraintPlan::Kind::Apply },
        { "EqualRadius",      SketchConstraintType::EqualRadius,      { mk_circle(0, 0, 1), mk_circle(5, 0, 2) }, 0, 1, -1, ConstraintPlan::Kind::Apply },
        { "Collinear",        SketchConstraintType::Collinear,        { mk_line(0, 0, 1, 0), mk_line(2, 0, 3, 0) }, 0, 1, -1, ConstraintPlan::Kind::Apply },
        { "Angle",            SketchConstraintType::Angle,            { mk_line(0, 0, 1, 0), mk_line(0, 0, 0, 1) }, 0, 1, -1, ConstraintPlan::Kind::AskValue },
        { "Radius",           SketchConstraintType::Radius,           { mk_circle(0, 0, 2.5) },                  0, -1, -1, ConstraintPlan::Kind::AskValue },
        { "Diameter",         SketchConstraintType::Diameter,         { mk_circle(0, 0, 2.5) },                  0, -1, -1, ConstraintPlan::Kind::AskValue },
        { "DistanceX",        SketchConstraintType::DistanceX,        { mk_point(0, 0), mk_point(5, 3) },        0,  1, -1, ConstraintPlan::Kind::AskValue },
        { "DistanceY",        SketchConstraintType::DistanceY,        { mk_point(0, 0), mk_point(5, 3) },        0,  1, -1, ConstraintPlan::Kind::AskValue },
    };
    for (const C& c : cases) {
        DYNAMIC_SECTION("apply " << c.name) {
            ConstraintPlan p = plan_entity_constraint(c.ents, c.e0, c.e1, c.e2, c.type);
            REQUIRE(p.kind == c.kind);
            REQUIRE(p.defs.size() >= 1);
            for (const auto& d : p.defs) REQUIRE(d.type == c.type);
        }
    }
}

TEST_CASE("constraint planner reject matrix", "[SketchConstraints]")
{
    struct C {
        const char* name; SketchConstraintType type; std::vector<SketchEntity> ents;
        int e0, e1, e2; ConstraintReject reason;
    };
    const std::vector<C> cases = {
        { "Fix",              SketchConstraintType::Fix,              {},                                 0, -1, -1, ConstraintReject::NeedOneEntity },
        { "Coincident",       SketchConstraintType::Coincident,       { mk_point(0, 0) },                 0, -1, -1, ConstraintReject::NeedTwoEntities },
        { "Horizontal",       SketchConstraintType::Horizontal,       { mk_point(1, 2) },                 0, -1, -1, ConstraintReject::NeedALine },
        { "Vertical",         SketchConstraintType::Vertical,         { mk_circle(0, 0, 1) },             0, -1, -1, ConstraintReject::NeedALine },
        { "Parallel",         SketchConstraintType::Parallel,         { mk_line(0, 0, 1, 0), mk_circle(5, 0, 1) }, 0, 1, -1, ConstraintReject::NeedTwoLines },
        { "Perpendicular",    SketchConstraintType::Perpendicular,    { mk_circle(0, 0, 1), mk_line(0, 0, 1, 0) }, 0, 1, -1, ConstraintReject::NeedTwoLines },
        { "EqualLength",      SketchConstraintType::EqualLength,      { mk_line(0, 0, 1, 0), mk_circle(5, 0, 1) }, 0, 1, -1, ConstraintReject::NeedTwoLines },
        { "Concentric",       SketchConstraintType::Concentric,       { mk_line(0, 0, 1, 0), mk_circle(5, 0, 1) }, 0, 1, -1, ConstraintReject::NeedTwoRounds },
        { "Tangent",          SketchConstraintType::Tangent,          { mk_line(0, 0, 1, 0), mk_line(0, 1, 1, 1) }, 0, 1, -1, ConstraintReject::NeedTangentPair },
        { "Midpoint",         SketchConstraintType::Midpoint,         { mk_line(0, 0, 1, 0), mk_line(0, 1, 1, 1) }, 0, 1, -1, ConstraintReject::NeedPointAndLine },
        { "Symmetric",        SketchConstraintType::Symmetric,        { mk_line(0, 0, 1, 0), mk_point(1, 1), mk_line(0, -1, 0, 1) }, 0, 1, 2, ConstraintReject::NeedTwoPointsOrLines },
        { "SymmetricAboutY",  SketchConstraintType::SymmetricAboutY,  { mk_line(0, 0, 1, 0), mk_point(1, 1) },     0, 1, -1, ConstraintReject::NeedTwoPointsOrLines },
        { "SymmetricAboutX",  SketchConstraintType::SymmetricAboutX,  { mk_point(1, 1), mk_line(0, 0, 1, 0) },     0, 1, -1, ConstraintReject::NeedTwoPointsOrLines },
        { "EqualRadius",      SketchConstraintType::EqualRadius,      { mk_line(0, 0, 1, 0), mk_circle(5, 0, 1) }, 0, 1, -1, ConstraintReject::NeedTwoRounds },
        { "Collinear",        SketchConstraintType::Collinear,        { mk_line(0, 0, 1, 0), mk_circle(5, 0, 1) }, 0, 1, -1, ConstraintReject::NeedTwoLines },
        { "Angle",            SketchConstraintType::Angle,            { mk_circle(0, 0, 1), mk_circle(5, 0, 1) }, 0, 1, -1, ConstraintReject::NeedTwoLines },
        { "Radius",           SketchConstraintType::Radius,           { mk_line(0, 0, 1, 0) },             0, -1, -1, ConstraintReject::NeedRound },
        { "Diameter",         SketchConstraintType::Diameter,         { mk_point(1, 2) },                 0, -1, -1, ConstraintReject::NeedRound },
        { "DistanceX",        SketchConstraintType::DistanceX,        { mk_point(0, 0) },                 0, -1, -1, ConstraintReject::NeedTwoEntities },
        { "DistanceY",        SketchConstraintType::DistanceY,        { mk_point(0, 0) },                 0, -1, -1, ConstraintReject::NeedTwoEntities },
    };
    for (const C& c : cases) {
        DYNAMIC_SECTION("reject " << c.name) {
            ConstraintPlan p = plan_entity_constraint(c.ents, c.e0, c.e1, c.e2, c.type);
            REQUIRE(p.kind == ConstraintPlan::Kind::Reject);
            REQUIRE(p.reason == c.reason);
        }
    }
}

TEST_CASE("constraint planner rejects types with no entity binding", "[SketchConstraints]")
{
    const SketchConstraintType unsupported[] = {
        SketchConstraintType::Distance, SketchConstraintType::LockX, SketchConstraintType::LockY,
        SketchConstraintType::PointOnLine, SketchConstraintType::PointOnObject,
    };
    std::vector<SketchEntity> ents = { mk_point(0, 0), mk_point(1, 1) };
    for (SketchConstraintType t : unsupported) {
        DYNAMIC_SECTION("unsupported " << int(t)) {
            ConstraintPlan p = plan_entity_constraint(ents, 0, 1, -1, t);
            REQUIRE(p.kind == ConstraintPlan::Kind::Reject);
            REQUIRE(p.reason == ConstraintReject::Unsupported);
        }
    }
}
