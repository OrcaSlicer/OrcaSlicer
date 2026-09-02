#include <catch2/catch_all.hpp>   // mainline OrcaSlicer ships Catch2 v3 (v2 was catch2/catch.hpp)
#include "libslic3r/CAD/SketchEngine.hpp"
#include <cmath>
#include <TopExp_Explorer.hxx>
#include <TopAbs.hxx>

using namespace Slic3r;

using Catch::Matchers::WithinAbs;

// CONTRACT: mirror_entities hands the reflected half back REVERSED — the order of the entities
// and the direction of each — because a reflection reverses orientation and the result has to
// CONTINUE the chain it was made from. So a mirrored line's p0 is the reflection of the source's
// p1, not its p0. See [SketchProfile] "a mirrored half continues the original chain".
TEST_CASE("Mirror Line across Y axis (reversed: p0 is the reflection of the source p1)", "[SketchEdit]")
{
    SketchEntity e;
    e.type = SketchEntity::Type::Line;
    e.p0   = Vec2d(3, 2);
    e.p1   = Vec2d(5, 4);

    Vec2d a(0, -1);
    Vec2d b(0, 1);

    auto result = SketchEngine::mirror_entities({e}, a, b);
    REQUIRE(result.size() == 1);

    const auto& m = result[0];
    REQUIRE(m.type == SketchEntity::Type::Line);
    REQUIRE_THAT(m.p0.x(), WithinAbs(-5.0, 1e-9));   // reflection of the SOURCE p1
    REQUIRE_THAT(m.p0.y(), WithinAbs(4.0, 1e-9));
    REQUIRE_THAT(m.p1.x(), WithinAbs(-3.0, 1e-9));   // reflection of the SOURCE p0
    REQUIRE_THAT(m.p1.y(), WithinAbs(2.0, 1e-9));
}

TEST_CASE("Mirror Circle across Y axis", "[SketchEdit]")
{
    SketchEntity e;
    e.type   = SketchEntity::Type::Circle;
    e.center = Vec2d(5, 0);
    e.p0     = Vec2d(5, 0);
    e.radius = 3;

    Vec2d a(0, -1);
    Vec2d b(0, 1);

    auto result = SketchEngine::mirror_entities({e}, a, b);
    REQUIRE(result.size() == 1);

    const auto& m = result[0];
    REQUIRE(m.type == SketchEntity::Type::Circle);
    REQUIRE_THAT(m.center.x(), WithinAbs(-5.0, 1e-9));
    REQUIRE_THAT(m.center.y(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(m.radius, WithinAbs(3.0, 1e-9));
    REQUIRE_THAT(m.p0.x(), WithinAbs(-5.0, 1e-9));
    REQUIRE_THAT(m.p0.y(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("Mirror Arc across X axis", "[SketchEdit]")
{
    SketchEntity e;
    e.type        = SketchEntity::Type::Arc;
    e.center      = Vec2d(0, 0);
    e.radius      = 1.0;
    e.start_angle = 0.0;
    e.end_angle   = M_PI / 2.0;
    e.p0          = Vec2d(1, 0);
    e.p1          = Vec2d(0, 1);

    Vec2d a(-1, 0);
    Vec2d b(1, 0);

    auto result = SketchEngine::mirror_entities({e}, a, b);
    REQUIRE(result.size() == 1);

    const auto& m = result[0];
    REQUIRE(m.type == SketchEntity::Type::Arc);

    // Reversed with the rest of the half: the mirrored arc STARTS where the reflection of the
    // source's end is, and finishes at the reflection of its start.
    REQUIRE_THAT(m.p0.x(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(m.p0.y(), WithinAbs(-1.0, 1e-9));
    REQUIRE_THAT(m.p1.x(), WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(m.p1.y(), WithinAbs(0.0, 1e-9));

    // The reflection alone would negate the sweep; walking the arc the other way negates it
    // again, so a mirrored CCW arc is CCW once more and a mirrored CCW loop stays CCW.
    double sweep = m.end_angle - m.start_angle;
    double orig_sweep = e.end_angle - e.start_angle;
    REQUIRE(orig_sweep > 0.0);
    REQUIRE(sweep > 0.0);
}

TEST_CASE("Offset Line by positive d", "[SketchEdit]")
{
    SketchEntity e;
    e.type = SketchEntity::Type::Line;
    e.p0   = Vec2d(0, 0);
    e.p1   = Vec2d(10, 0);

    auto result = SketchEngine::offset_entities({e}, 2.0);
    REQUIRE(result.size() == 1);

    const auto& o = result[0];
    REQUIRE(o.type == SketchEntity::Type::Line);
    REQUIRE_THAT(o.p0.x(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(o.p0.y(), WithinAbs(2.0, 1e-9));
    REQUIRE_THAT(o.p1.x(), WithinAbs(10.0, 1e-9));
    REQUIRE_THAT(o.p1.y(), WithinAbs(2.0, 1e-9));
}

TEST_CASE("Offset Circle: expand and collapse", "[SketchEdit]")
{
    SketchEntity e;
    e.type   = SketchEntity::Type::Circle;
    e.center = Vec2d(0, 0);
    e.p0     = Vec2d(0, 0);
    e.radius = 5;

    auto expanded = SketchEngine::offset_entities({e}, 2.0);
    REQUIRE(expanded.size() == 1);
    REQUIRE_THAT(expanded[0].radius, WithinAbs(7.0, 1e-9));

    auto collapsed = SketchEngine::offset_entities({e}, -5.0);
    REQUIRE(collapsed.empty());
}

// CONTRACT CHANGED: +d used to mean "radius + d" for every arc regardless of its sweep, while
// for a line it meant "left of the direction of travel". The two disagreed, so a profile made
// of lines AND arcs (any slot outline) offset with its straights going one way and its caps the
// other, and could never come back closed. The arc now follows the line's rule: +d is left of
// travel, which for this CCW quarter-arc is inward -> r = 3. See [SketchProfile].
TEST_CASE("Offset Arc by positive d (left of travel: a CCW arc shrinks)", "[SketchEdit]")
{
    SketchEntity e;
    e.type        = SketchEntity::Type::Arc;
    e.center      = Vec2d(0, 0);
    e.radius      = 4.0;
    e.start_angle = 0.0;
    e.end_angle   = M_PI / 2.0;
    e.p0          = Vec2d(4, 0);
    e.p1          = Vec2d(0, 4);

    auto result = SketchEngine::offset_entities({e}, 1.0);
    REQUIRE(result.size() == 1);

    const auto& o = result[0];
    REQUIRE(o.type == SketchEntity::Type::Arc);
    REQUIRE_THAT(o.radius, WithinAbs(3.0, 1e-9));
    REQUIRE_THAT(o.p0.x(), WithinAbs(3.0, 1e-9));
    REQUIRE_THAT(o.p0.y(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(o.p1.x(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(o.p1.y(), WithinAbs(3.0, 1e-9));
}

TEST_CASE("Fillet right-angle corner", "[SketchEdit]")
{
    SketchEntity a;
    a.type = SketchEntity::Type::Line;
    a.p0   = Vec2d(0, 0);
    a.p1   = Vec2d(10, 0);

    SketchEntity b;
    b.type = SketchEntity::Type::Line;
    b.p0   = Vec2d(10, 0);
    b.p1   = Vec2d(10, 10);

    SketchEntity a_out, b_out, arc_out;
    bool ok = SketchEngine::fillet_lines(a, b, 2.0, a_out, b_out, arc_out);
    REQUIRE(ok);

    REQUIRE_THAT(a_out.p0.x(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(a_out.p0.y(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(a_out.p1.x(), WithinAbs(8.0, 1e-9));
    REQUIRE_THAT(a_out.p1.y(), WithinAbs(0.0, 1e-9));

    REQUIRE_THAT(b_out.p0.x(), WithinAbs(10.0, 1e-9));
    REQUIRE_THAT(b_out.p0.y(), WithinAbs(2.0, 1e-9));
    REQUIRE_THAT(b_out.p1.x(), WithinAbs(10.0, 1e-9));
    REQUIRE_THAT(b_out.p1.y(), WithinAbs(10.0, 1e-9));

    REQUIRE(arc_out.type == SketchEntity::Type::Arc);
    REQUIRE_THAT(arc_out.radius, WithinAbs(2.0, 1e-9));
    REQUIRE_THAT(arc_out.center.x(), WithinAbs(8.0, 1e-9));
    REQUIRE_THAT(arc_out.center.y(), WithinAbs(2.0, 1e-9));
    REQUIRE_THAT((arc_out.p0 - arc_out.center).norm(), WithinAbs(2.0, 1e-9));
    REQUIRE_THAT((arc_out.p1 - arc_out.center).norm(), WithinAbs(2.0, 1e-9));
}

TEST_CASE("Fillet parallel lines returns false", "[SketchEdit]")
{
    SketchEntity a;
    a.type = SketchEntity::Type::Line;
    a.p0   = Vec2d(0, 0);
    a.p1   = Vec2d(10, 0);

    SketchEntity b;
    b.type = SketchEntity::Type::Line;
    b.p0   = Vec2d(0, 5);
    b.p1   = Vec2d(10, 5);

    SketchEntity a_out, b_out, arc_out;
    REQUIRE_FALSE(SketchEngine::fillet_lines(a, b, 1.0, a_out, b_out, arc_out));
}

TEST_CASE("Fillet arc too big returns false", "[SketchEdit]")
{
    SketchEntity a;
    a.type = SketchEntity::Type::Line;
    a.p0   = Vec2d(0, 0);
    a.p1   = Vec2d(1, 0);

    SketchEntity b;
    b.type = SketchEntity::Type::Line;
    b.p0   = Vec2d(1, 0);
    b.p1   = Vec2d(1, 1);

    SketchEntity a_out, b_out, arc_out;
    REQUIRE_FALSE(SketchEngine::fillet_lines(a, b, 5.0, a_out, b_out, arc_out));
}

TEST_CASE("Trim right arm", "[SketchEdit]")
{
    SketchEntity e;
    e.type = SketchEntity::Type::Line;
    e.p0   = Vec2d(-5, 0);
    e.p1   = Vec2d(5, 0);

    SketchEntity vc;
    vc.type = SketchEntity::Type::Line;
    vc.p0   = Vec2d(0, -5);
    vc.p1   = Vec2d(0, 5);

    bool ok = SketchEngine::trim_entity(e, {vc}, Vec2d(3, 0));
    REQUIRE(ok);
    REQUIRE_THAT(e.p0.x(), WithinAbs(-5.0, 1e-9));
    REQUIRE_THAT(e.p0.y(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(e.p1.x(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(e.p1.y(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("Trim left arm", "[SketchEdit]")
{
    SketchEntity e;
    e.type = SketchEntity::Type::Line;
    e.p0   = Vec2d(-5, 0);
    e.p1   = Vec2d(5, 0);

    SketchEntity vc;
    vc.type = SketchEntity::Type::Line;
    vc.p0   = Vec2d(0, -5);
    vc.p1   = Vec2d(0, 5);

    bool ok = SketchEngine::trim_entity(e, {vc}, Vec2d(-3, 0));
    REQUIRE(ok);
    REQUIRE_THAT(e.p0.x(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(e.p0.y(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(e.p1.x(), WithinAbs(5.0, 1e-9));
    REQUIRE_THAT(e.p1.y(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("Trim no cut (u out of range)", "[SketchEdit]")
{
    SketchEntity e;
    e.type = SketchEntity::Type::Line;
    e.p0   = Vec2d(-5, 0);
    e.p1   = Vec2d(5, 0);

    SketchEntity other;
    other.type = SketchEntity::Type::Line;
    other.p0   = Vec2d(0, 3);
    other.p1   = Vec2d(0, 8);

    REQUIRE_FALSE(SketchEngine::trim_entity(e, {other}, Vec2d(3, 0)));
}

TEST_CASE("Extend forward to line", "[SketchEdit]")
{
    SketchEntity e;
    e.type = SketchEntity::Type::Line;
    e.p0   = Vec2d(0, 0);
    e.p1   = Vec2d(2, 0);

    SketchEntity other;
    other.type = SketchEntity::Type::Line;
    other.p0   = Vec2d(5, -5);
    other.p1   = Vec2d(5, 5);

    bool ok = SketchEngine::extend_entity(e, {other}, Vec2d(2, 0));
    REQUIRE(ok);
    REQUIRE_THAT(e.p0.x(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(e.p0.y(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(e.p1.x(), WithinAbs(5.0, 1e-9));
    REQUIRE_THAT(e.p1.y(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("Extend forward to circle", "[SketchEdit]")
{
    SketchEntity e;
    e.type = SketchEntity::Type::Line;
    e.p0   = Vec2d(0, 0);
    e.p1   = Vec2d(2, 0);

    SketchEntity other;
    other.type   = SketchEntity::Type::Circle;
    other.center = Vec2d(10, 0);
    other.p0     = Vec2d(10, 0);
    other.radius = 3;

    bool ok = SketchEngine::extend_entity(e, {other}, Vec2d(2, 0));
    REQUIRE(ok);
    REQUIRE_THAT(e.p0.x(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(e.p0.y(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(e.p1.x(), WithinAbs(7.0, 1e-9));
    REQUIRE_THAT(e.p1.y(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("Extend backward", "[SketchEdit]")
{
    SketchEntity e;
    e.type = SketchEntity::Type::Line;
    e.p0   = Vec2d(0, 0);
    e.p1   = Vec2d(2, 0);

    SketchEntity other;
    other.type = SketchEntity::Type::Line;
    other.p0   = Vec2d(-3, -5);
    other.p1   = Vec2d(-3, 5);

    bool ok = SketchEngine::extend_entity(e, {other}, Vec2d(0, 0));
    REQUIRE(ok);
    REQUIRE_THAT(e.p0.x(), WithinAbs(-3.0, 1e-9));
    REQUIRE_THAT(e.p0.y(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(e.p1.x(), WithinAbs(2.0, 1e-9));
    REQUIRE_THAT(e.p1.y(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("Extend no target", "[SketchEdit]")
{
    SketchEntity e;
    e.type = SketchEntity::Type::Line;
    e.p0   = Vec2d(0, 0);
    e.p1   = Vec2d(2, 0);

    SketchEntity other;
    other.type = SketchEntity::Type::Line;
    other.p0   = Vec2d(5, -5);
    other.p1   = Vec2d(5, -1);

    REQUIRE_FALSE(SketchEngine::extend_entity(e, {other}, Vec2d(2, 0)));
}

// --- Arc/Circle subject trim & extend (Fase 4.5 kernel) -------------------

TEST_CASE("Trim arc drops the picked (start) side", "[SketchEdit]")
{
    // Upper semicircle r=5, ccw from (5,0) to (-5,0); cutter = vertical axis.
    SketchEntity e;
    e.type        = SketchEntity::Type::Arc;
    e.center      = Vec2d(0, 0);
    e.radius      = 5;
    e.start_angle = 0.0;
    e.end_angle   = M_PI;

    SketchEntity cut;
    cut.type = SketchEntity::Type::Line;
    cut.p0   = Vec2d(0, -10);
    cut.p1   = Vec2d(0, 10);

    // Pick the right quarter (phi=pi/4) -> it is removed, left quarter kept.
    bool ok = SketchEngine::trim_entity(e, {cut}, Vec2d(5 * std::cos(M_PI/4), 5 * std::sin(M_PI/4)));
    REQUIRE(ok);
    REQUIRE(e.type == SketchEntity::Type::Arc);
    REQUIRE_THAT(e.radius, WithinAbs(5.0, 1e-9));
    REQUIRE_THAT(e.start_angle, WithinAbs(M_PI / 2.0, 1e-9));
    REQUIRE_THAT(e.end_angle,   WithinAbs(M_PI, 1e-9));
}

TEST_CASE("Trim arc drops the picked (end) side", "[SketchEdit]")
{
    SketchEntity e;
    e.type        = SketchEntity::Type::Arc;
    e.center      = Vec2d(0, 0);
    e.radius      = 5;
    e.start_angle = 0.0;
    e.end_angle   = M_PI;

    SketchEntity cut;
    cut.type = SketchEntity::Type::Line;
    cut.p0   = Vec2d(0, -10);
    cut.p1   = Vec2d(0, 10);

    // Pick the left quarter (phi=3pi/4) -> removed, right quarter kept.
    bool ok = SketchEngine::trim_entity(e, {cut}, Vec2d(5 * std::cos(3*M_PI/4), 5 * std::sin(3*M_PI/4)));
    REQUIRE(ok);
    REQUIRE(e.type == SketchEntity::Type::Arc);
    REQUIRE_THAT(e.start_angle, WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(e.end_angle,   WithinAbs(M_PI / 2.0, 1e-9));
}

TEST_CASE("Trim circle opens into an arc excluding the pick", "[SketchEdit]")
{
    // Full circle r=5; vertical axis cuts it at (0,+-5). Pick the right side
    // (5,0): the kept arc is the left half, sweeping pi and centred on (-5,0).
    SketchEntity e;
    e.type   = SketchEntity::Type::Circle;
    e.center = Vec2d(0, 0);
    e.p0     = Vec2d(5, 0);
    e.radius = 5;

    SketchEntity cut;
    cut.type = SketchEntity::Type::Line;
    cut.p0   = Vec2d(0, -10);
    cut.p1   = Vec2d(0, 10);

    bool ok = SketchEngine::trim_entity(e, {cut}, Vec2d(5, 0));
    REQUIRE(ok);
    REQUIRE(e.type == SketchEntity::Type::Arc);
    REQUIRE_THAT(e.radius, WithinAbs(5.0, 1e-9));
    REQUIRE_THAT(e.end_angle - e.start_angle, WithinAbs(M_PI, 1e-9));
    // Midpoint of the kept arc must point left (away from the pick).
    double mid = 0.5 * (e.start_angle + e.end_angle);
    REQUIRE_THAT(5 * std::cos(mid), WithinAbs(-5.0, 1e-9));
    REQUIRE_THAT(5 * std::sin(mid), WithinAbs(0.0, 1e-9));
}

TEST_CASE("Extend arc forward (end) to a crossing", "[SketchEdit]")
{
    // Quarter arc (5,0)->(0,5); cutter crosses the circle at (-5,0). Picking
    // near the end grows the sweep ccw to pi.
    SketchEntity e;
    e.type        = SketchEntity::Type::Arc;
    e.center      = Vec2d(0, 0);
    e.radius      = 5;
    e.start_angle = 0.0;
    e.end_angle   = M_PI / 2.0;

    SketchEntity cut;
    cut.type = SketchEntity::Type::Line;
    cut.p0   = Vec2d(-10, 0);
    cut.p1   = Vec2d(0, 0);

    bool ok = SketchEngine::extend_entity(e, {cut}, Vec2d(0, 5));
    REQUIRE(ok);
    REQUIRE(e.type == SketchEntity::Type::Arc);
    REQUIRE_THAT(e.start_angle, WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(e.end_angle,   WithinAbs(M_PI, 1e-9));
}

TEST_CASE("Extend arc backward (start) to a crossing", "[SketchEdit]")
{
    // Quarter arc (0,5)->(-5,0); cutter crosses at (5,0). Picking near the
    // start grows the sweep cw to start_angle 0.
    SketchEntity e;
    e.type        = SketchEntity::Type::Arc;
    e.center      = Vec2d(0, 0);
    e.radius      = 5;
    e.start_angle = M_PI / 2.0;
    e.end_angle   = M_PI;

    SketchEntity cut;
    cut.type = SketchEntity::Type::Line;
    cut.p0   = Vec2d(10, 0);
    cut.p1   = Vec2d(0, 0);

    bool ok = SketchEngine::extend_entity(e, {cut}, Vec2d(0, 5));
    REQUIRE(ok);
    REQUIRE_THAT(e.start_angle, WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(e.end_angle,   WithinAbs(M_PI, 1e-9));
}

TEST_CASE("Extend circle returns false (closed)", "[SketchEdit]")
{
    SketchEntity e;
    e.type   = SketchEntity::Type::Circle;
    e.center = Vec2d(0, 0);
    e.p0     = Vec2d(5, 0);
    e.radius = 5;

    SketchEntity cut;
    cut.type = SketchEntity::Type::Line;
    cut.p0   = Vec2d(0, -10);
    cut.p1   = Vec2d(0, 10);

    REQUIRE_FALSE(SketchEngine::extend_entity(e, {cut}, Vec2d(5, 0)));
}

TEST_CASE("Trim arc with no crossing returns false", "[SketchEdit]")
{
    SketchEntity e;
    e.type        = SketchEntity::Type::Arc;
    e.center      = Vec2d(0, 0);
    e.radius      = 5;
    e.start_angle = 0.0;
    e.end_angle   = M_PI / 2.0;

    SketchEntity cut;          // far away, never reaches the r=5 circle
    cut.type = SketchEntity::Type::Line;
    cut.p0   = Vec2d(20, -5);
    cut.p1   = Vec2d(20, 5);

    REQUIRE_FALSE(SketchEngine::trim_entity(e, {cut}, Vec2d(5 * std::cos(M_PI/4), 5 * std::sin(M_PI/4))));
}

// Regression guard: BEFORE the weld fix this test failed with 4 edges instead of 6.
// BRepLib_MakeWire::Add silently DROPS a disconnected edge (BRepLib_DisconnectedWire + NotDone)
// yet every successful Add ends with BRepLib_WireDone + Done(), so IsDone() reported only whether
// the LAST edge connected. This sketch is a real user loop (2 arcs + 4 lines) given in
// creation order, which is NOT traversal order, and its joint between the 3rd and 4th entity
// below is open by 2.28e-5 mm — larger than OCCT's default vertex tolerance.
TEST_CASE("entities_to_wires keeps every edge of a loop drawn out of order", "[SketchEngine]")
{
    std::vector<SketchEntity> ents(6);

    ents[0].type = SketchEntity::Type::Line;
    ents[0].p0   = Vec2d(-0.537697713190522, -0.0009077462579133498);
    ents[0].p1   = Vec2d(99.46230228680926, -0.0009141694814321626);

    ents[1].type        = SketchEntity::Type::Arc;
    ents[1].p0          = Vec2d(-0.537697713190522, -0.0009077462579133498);
    ents[1].p1          = Vec2d(-100.14602636660666, -0.27673132181233495);
    ents[1].center      = Vec2d(-50.313868583115394, -10.248112903179617);
    ents[1].radius      = 50.81999999999999;
    ents[1].start_angle = 0.20302922018398933;
    ents[1].end_angle   = 2.944101582158999;

    ents[2].type = SketchEntity::Type::Line;
    ents[2].p0   = Vec2d(99.46228668626469, -39.22091416947833);
    ents[2].p1   = Vec2d(-0.537864366432629, -39.22420589376945);

    ents[3].type        = SketchEntity::Type::Arc;
    ents[3].p0          = Vec2d(-100.14602636694521, -38.94673132181234);
    ents[3].p1          = Vec2d(-0.5378420354900413, -39.22421049164698);
    ents[3].center      = Vec2d(-50.31377078666179, -28.975499083790503);
    ents[3].radius      = 50.82006659345552;
    ents[3].start_angle = -2.944104841286737;
    ents[3].end_angle   = -0.20305921095748136;

    ents[4].type = SketchEntity::Type::Line;
    ents[4].p0   = Vec2d(99.46228668626469, -39.22091416947833);
    ents[4].p1   = Vec2d(99.46230228680926, -0.0009141694814321626);

    ents[5].type = SketchEntity::Type::Line;
    ents[5].p0   = Vec2d(-100.14602636694521, -38.94673132181234);
    ents[5].p1   = Vec2d(-100.14602636660666, -0.27673132181233495);

    auto wires = SketchEngine::entities_to_wires(ents, SketchPlane::XY());

    REQUIRE(wires.size() == 1);

    int edge_count = 0;
    for (TopExp_Explorer ex(wires[0], TopAbs_EDGE); ex.More(); ex.Next())
        ++edge_count;
    REQUIRE(edge_count == 6);

    REQUIRE(wires[0].Closed());
}
