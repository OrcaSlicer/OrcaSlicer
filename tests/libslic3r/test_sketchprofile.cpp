// Closed-profile harness for the 2D sketch layer.
//
// The existing [SketchEdit] cases check one entity at a time — offset ONE line, mirror ONE
// arc — and every one of them passes while the feature they belong to is unusable. What a
// user actually does is combine 2D features into a CLOSED PROFILE and extrude it, and the
// property that makes that work is topological, not per-entity: after the operation, do the
// pieces still form a single closed loop?
//
// So these cases assert the loop, not the coordinates. That is the invariant every sketch
// operation has to preserve and the only one that predicts whether the GUI can build a solid
// out of the result.
#include <catch2/catch_all.hpp>   // mainline OrcaSlicer ships Catch2 v3 (v2 was catch2/catch.hpp)
#include "libslic3r/CAD/SketchEngine.hpp"
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <cmath>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

SketchPlane xy_plane() { return SketchPlane::XY(); }

SketchEntity line(const Vec2d& a, const Vec2d& b)
{
    SketchEntity e;
    e.type = SketchEntity::Type::Line;
    e.p0 = a; e.p1 = b;
    return e;
}

// A CCW rectangle as four Line entities sharing endpoints exactly.
std::vector<SketchEntity> rect(double w, double h)
{
    return { line({0, 0}, {w, 0}), line({w, 0}, {w, h}),
             line({w, h}, {0, h}), line({0, h}, {0, 0}) };
}

// How many of the wires the sketch resolves to are CLOSED.
int closed_wires(const std::vector<SketchEntity>& ents)
{
    const auto ws = SketchEngine::entities_to_wires(ents, xy_plane());
    int n = 0;
    for (const auto& w : ws)
        if (!w.IsNull() && w.Closed()) ++n;
    return n;
}

// Enclosed area of the single closed loop the sketch resolves to. -1 when it is not one
// closed loop — the failure the whole file exists to catch.
double profile_area(const std::vector<SketchEntity>& ents)
{
    const auto ws = SketchEngine::entities_to_wires(ents, xy_plane());
    if (ws.size() != 1 || ws[0].IsNull() || !ws[0].Closed()) return -1.0;
    const TopoDS_Face f = SketchEngine::wires_to_face(ws, xy_plane());
    GProp_GProps props;
    BRepGProp::SurfaceProperties(f, props);
    return props.Mass();
}

SketchEntity arc(const Vec2d& c, double r, double a0, double a1)
{
    SketchEntity e;
    e.type        = SketchEntity::Type::Arc;
    e.center      = c;
    e.radius      = r;
    e.start_angle = a0;
    e.end_angle   = a1;
    e.p0 = c + r * Vec2d(std::cos(a0), std::sin(a0));
    e.p1 = c + r * Vec2d(std::cos(a1), std::sin(a1));
    return e;
}

} // namespace

TEST_CASE("profile baseline: a hand-built rectangle is one closed loop", "[SketchProfile]")
{
    REQUIRE(closed_wires(rect(40, 20)) == 1);
}

TEST_CASE("profile: mirroring a closed rectangle keeps it closed", "[SketchProfile]")
{
    const auto m = SketchEngine::mirror_entities(rect(40, 20), Vec2d(-10, 0), Vec2d(-10, 1));
    REQUIRE(m.size() == 4);
    REQUIRE(closed_wires(m) == 1);
}

TEST_CASE("profile: mirroring an open half-profile closes it against the axis", "[SketchProfile]")
{
    // Half a rectangle, open along x=0 — the classic "draw half, mirror it" gesture.
    const std::vector<SketchEntity> half = {
        line({0, 0}, {20, 0}), line({20, 0}, {20, 10}), line({20, 10}, {0, 10}) };
    auto all = half;
    for (const auto& e : SketchEngine::mirror_entities(half, Vec2d(0, 0), Vec2d(0, 1)))
        all.push_back(e);
    REQUIRE(all.size() == 6);
    REQUIRE(closed_wires(all) == 1);
}

TEST_CASE("profile: offsetting a closed rectangle keeps it closed", "[SketchProfile]")
{
    const auto out = SketchEngine::offset_entities(rect(40, 20), 5.0);
    REQUIRE(out.size() == 4);
    REQUIRE(closed_wires(out) == 1);
}

TEST_CASE("profile: offset outward grows the enclosed area by the right amount", "[SketchProfile]")
{
    // A rectangle offset outward by d is (w+2d) x (h+2d) with the corners rounded at r=d,
    // so its area is w*h + 2d(w+h) + pi*d^2 whichever way the corners are healed... except
    // for a sharp-corner offset, which is exactly (w+2d)*(h+2d). Either healing is defensible;
    // a set of four disconnected segments is not, and that is what this measures.
    const double w = 40, h = 20, d = 5;
    const auto out = SketchEngine::offset_entities(rect(w, h), d);
    const auto ws  = SketchEngine::entities_to_wires(out, xy_plane());
    REQUIRE(ws.size() == 1);
    REQUIRE(ws[0].Closed());
}

TEST_CASE("profile: offset sign is left-of-travel, so +d shrinks a CCW rectangle", "[SketchProfile]")
{
    // The convention has to be pinned by a test, because it is the one thing a caller cannot
    // read off the geometry: +d = left of the direction of travel = inward for a CCW loop.
    // Miter join on a rectangle keeps the corners sharp, so the result is exact.
    const double w = 40, h = 20, d = 5;
    REQUIRE_THAT(profile_area(SketchEngine::offset_entities(rect(w, h), d)),
                 WithinAbs((w - 2 * d) * (h - 2 * d), 1e-6));
    REQUIRE_THAT(profile_area(SketchEngine::offset_entities(rect(w, h), -d)),
                 WithinAbs((w + 2 * d) * (h + 2 * d), 1e-6));
}

TEST_CASE("profile: offsetting a stadium (two lines + two arcs) stays closed", "[SketchProfile]")
{
    // A slot outline: straight top and bottom joined by half-circle caps. This is the case the
    // per-entity offset could never repair, because both seams are line-to-arc.
    const double L = 30, r = 8, d = 3;
    const std::vector<SketchEntity> slot = {
        line({0, -r}, {L, -r}),
        arc({L, 0}, r, -M_PI / 2, M_PI / 2),
        line({L, r}, {0, r}),
        arc({0, 0}, r, M_PI / 2, 3 * M_PI / 2),
    };
    REQUIRE(closed_wires(slot) == 1);
    const auto out = SketchEngine::offset_entities(slot, -d);   // -d = outward for this CCW loop
    REQUIRE(closed_wires(out) == 1);
    // Offsetting a stadium outward by d gives the stadium with radius r+d: L*2(r+d) + pi(r+d)^2.
    // Lines and caps must move the SAME way — that is the assertion this case exists for.
    const double rr = r + d;
    REQUIRE_THAT(profile_area(out), WithinAbs(L * 2 * rr + M_PI * rr * rr, 1e-6));
}

TEST_CASE("profile: an open chain offsets without being forced closed", "[SketchProfile]")
{
    // A sweep path is legitimately open; the repair must join its interior seams and leave
    // the two free ends alone.
    const std::vector<SketchEntity> open_chain = {
        line({0, 0}, {20, 0}), line({20, 0}, {20, 10}) };
    const auto out = SketchEngine::offset_entities(open_chain, 4.0);
    REQUIRE(out.size() == 2);
    REQUIRE(closed_wires(out) == 0);
    // The interior seam is repaired: the two offset segments still meet.
    REQUIRE_THAT((out[0].p1 - out[1].p0).norm(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("profile: a mirrored half offsets as one loop, not two", "[SketchProfile]")
{
    // The classic "draw half, mirror it" gesture on a stadium. mirror_entities emits a half
    // that travels the opposite way round, so the concatenation must still chain as ONE closed
    // loop and its mirrored cap must offset outward like the original, not inward.
    const double L = 30, R = 15, d = 4;
    const std::vector<SketchEntity> half = {
        line({0, -R}, {L, -R}),
        arc({L, 0}, R, -M_PI / 2, M_PI / 2),
        line({L, R}, {0, R}),
    };
    std::vector<SketchEntity> all = half;
    for (const auto& e : SketchEngine::mirror_entities(half, Vec2d(0, 0), Vec2d(0, 1)))
        all.push_back(e);
    REQUIRE(closed_wires(all) == 1);

    const auto out = SketchEngine::offset_entities(all, -d);   // -d = outward for this loop
    REQUIRE(closed_wires(out) == 1);
    for (const auto& o : out)
        if (o.type == SketchEntity::Type::Arc)
            REQUIRE_THAT(o.radius, WithinAbs(R + d, 1e-9));
}
