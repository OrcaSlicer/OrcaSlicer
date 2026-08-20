#include <catch2/catch_all.hpp>   // mainline OrcaSlicer ships Catch2 v3 (v2 was catch2/catch.hpp)
using Catch::Approx;   // v3 scopes Approx into the Catch namespace; v2 had it at global scope

#include "libslic3r/CAD/SketchInference.hpp"

using namespace Slic3r;
using K = InferenceSnap::Kind;

static SketchEntity line(Vec2d a, Vec2d b)
{
    SketchEntity e; e.type = SketchEntity::Type::Line; e.p0 = a; e.p1 = b; return e;
}
static SketchEntity circle(Vec2d c, double r)
{
    SketchEntity e; e.type = SketchEntity::Type::Circle; e.center = c; e.p0 = c; e.radius = r; return e;
}

TEST_CASE("inference: cursor near a line endpoint snaps Coincident-able to it", "[inference]")
{
    std::vector<SketchEntity> ents = { line({0, 0}, {10, 0}) };
    auto s = infer_point_snap(ents, {10.3, 0.2}, 1.0);
    REQUIRE(s.kind == K::Endpoint);
    CHECK(s.entity == 0);
    CHECK(s.role == SketchPointRole::P1);
    CHECK((s.point - Vec2d(10, 0)).norm() == Approx(0.0).margin(1e-9));
}

TEST_CASE("inference: endpoint beats midpoint when both are in range", "[inference]")
{
    std::vector<SketchEntity> ents = { line({0, 0}, {2, 0}) };
    // Query equidistant-ish but closer to the endpoint: endpoint tier wins regardless.
    auto s = infer_point_snap(ents, {1.9, 0.0}, 5.0);
    CHECK(s.kind == K::Endpoint);
    CHECK(s.role == SketchPointRole::P1);
}

TEST_CASE("inference: midpoint of a line is detected", "[inference]")
{
    std::vector<SketchEntity> ents = { line({0, 0}, {10, 0}) };
    auto s = infer_point_snap(ents, {5.1, 0.1}, 0.5, /*include_origin=*/false);
    REQUIRE(s.kind == K::Midpoint);
    CHECK((s.point - Vec2d(5, 0)).norm() == Approx(0.0).margin(1e-9));
}

TEST_CASE("inference: circle centre and rim", "[inference]")
{
    std::vector<SketchEntity> ents = { circle({0, 0}, 5.0) };
    auto c = infer_point_snap(ents, {0.2, 0.1}, 1.0, false);
    CHECK(c.kind == K::Center);
    auto r = infer_point_snap(ents, {5.1, 0.0}, 1.0, false);
    REQUIRE(r.kind == K::OnEdge);
    CHECK((r.point - Vec2d(5, 0)).norm() == Approx(0.0).margin(1e-9));
}

TEST_CASE("inference: origin snap when nothing else is near", "[inference]")
{
    std::vector<SketchEntity> ents = { line({20, 20}, {30, 20}) };
    auto s = infer_point_snap(ents, {0.1, 0.1}, 1.0);
    REQUIRE(s.kind == K::Origin);
    CHECK(s.entity == -1);
    CHECK((s.point - Vec2d(0, 0)).norm() == Approx(0.0).margin(1e-9));
}

TEST_CASE("inference: nothing in range returns None and the raw query", "[inference]")
{
    std::vector<SketchEntity> ents = { line({0, 0}, {10, 0}) };
    auto s = infer_point_snap(ents, {50, 50}, 1.0, /*include_origin=*/false);
    CHECK(s.kind == K::None);
    CHECK((s.point - Vec2d(50, 50)).norm() == Approx(0.0).margin(1e-9));
}

TEST_CASE("inference: axis inference flags horizontal / vertical segments", "[inference]")
{
    CHECK(infer_axis_constraint({0, 0}, {10, 0.05}).value() == SketchConstraintType::Horizontal);
    CHECK(infer_axis_constraint({0, 0}, {0.05, 10}).value() == SketchConstraintType::Vertical);
    CHECK_FALSE(infer_axis_constraint({0, 0}, {10, 10}).has_value());   // 45 deg
    CHECK_FALSE(infer_axis_constraint({0, 0}, {0, 0}).has_value());     // degenerate
}
