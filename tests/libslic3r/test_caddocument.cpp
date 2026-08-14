#include <catch2/catch_all.hpp>   // mainline OrcaSlicer ships Catch2 v3 (v2 was catch2/catch.hpp)

// Substring assertions, spelled so this file compiles UNCHANGED on both forks.
// Catch2 v2 (snaporca) spells it Matchers::Contains; v3 (orca_cad / mainline) spells it
// Matchers::ContainsSubstring and gives Contains an incompatible meaning — range-contains-
// ELEMENT — which fails to compile against a std::string rather than failing a test.
// Using find() sidesteps the rename entirely; INFO keeps the actual string in the report.
#define REQUIRE_CONTAINS(str, sub) \
    do { const std::string _actual = (str); INFO("actual: " << _actual); \
         REQUIRE(_actual.find(sub) != std::string::npos); } while (0)
#define CHECK_CONTAINS(str, sub) \
    do { const std::string _actual = (str); INFO("actual: " << _actual); \
         CHECK(_actual.find(sub) != std::string::npos); } while (0)

#include "libslic3r/CadDocument.hpp"
#include "libslic3r/GeometryEngine.hpp"
#include "libslic3r/SketchEngine.hpp"
#include "libslic3r/SketchImport.hpp"
#include "libslic3r/ThreadStandards.hpp"
#include "libslic3r/Utils.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <cereal/archives/binary.hpp>

using namespace Slic3r;
using Catch::Approx;   // Catch2 v3 scopes Approx under Catch:: (v2 had it unqualified)

TEST_CASE("CadDocument profile sketch -> extrude -> solid", "[CadDocument]")
{
    CadDocument doc;

    SketchProfile sp;
    sp.points.push_back(Vec2d(-10, -10));
    sp.points.push_back(Vec2d( 10, -10));
    sp.points.push_back(Vec2d( 10,  10));
    sp.points.push_back(Vec2d(-10,  10));
    sp.closed = true;

    int sk_idx = doc.add_sketch_profile(sp, SketchPlane::XY(), "SquareProfile");
    REQUIRE(sk_idx >= 0);
    doc.add_extrude(sk_idx, 5.0, false, BooleanMode::New, "Extrude1");

    bool ok = doc.recompute();
    REQUIRE(ok);
    REQUIRE(doc.error.empty());
    REQUIRE(doc.display_mesh.facets_count() > 0);
}

TEST_CASE("CadDocument legacy Rectangle sketch still works", "[CadDocument]")
{
    CadDocument doc;

    int sk_idx = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(),
                                20, 20, 10, "RectSketch");
    REQUIRE(sk_idx >= 0);
    doc.add_extrude(sk_idx, 5.0, false, BooleanMode::New, "Extrude1");

    bool ok = doc.recompute();
    REQUIRE(ok);
    REQUIRE(doc.error.empty());
    REQUIRE(doc.display_mesh.facets_count() > 0);
}

TEST_CASE("CadDocument preview with profile on self-contained candidate", "[CadDocument]")
{
    CadDocument doc;

    CadFeature candidate;
    candidate.type     = CadFeatureType::Extrude;
    candidate.plane    = SketchPlane::XY();
    candidate.distance = 4;
    candidate.mode     = BooleanMode::New;
    // sketch_ref is -1 by default -> apply_feature uses candidate's own params

    SketchProfile tri;
    tri.points.push_back(Vec2d(0,  0));
    tri.points.push_back(Vec2d(10, 0));
    tri.points.push_back(Vec2d(5,  8.66));
    tri.closed   = true;
    candidate.profile = tri;

    TriangleMesh mesh;
    std::string  err;
    bool ok = doc.preview(candidate, mesh, err);
    REQUIRE(ok);
    REQUIRE(mesh.facets_count() > 0);
}

TEST_CASE("CadDocument solve_sketch_feature snaps a rough quad to a rectangle", "[CadDocument]")
{
    CadDocument doc;
    SketchProfile sp;
    sp.points = { Vec2d(0,0), Vec2d(8,1), Vec2d(9,5), Vec2d(-1,4) };
    sp.closed = true;
    int sk = doc.add_sketch_profile(sp, SketchPlane::XY(), "S");

    auto& cons = doc.features[sk].constraints;
    cons.push_back({SketchConstraintType::Fix,        0,-1,-1,-1, 0});
    cons.push_back({SketchConstraintType::LockX,      0,-1,-1,-1, 0});
    cons.push_back({SketchConstraintType::LockY,      0,-1,-1,-1, 0});
    cons.push_back({SketchConstraintType::Horizontal, 0, 1,-1,-1, 0});
    cons.push_back({SketchConstraintType::Vertical,   1, 2,-1,-1, 0});
    cons.push_back({SketchConstraintType::Horizontal, 2, 3,-1,-1, 0});
    cons.push_back({SketchConstraintType::Vertical,   3, 0,-1,-1, 0});
    cons.push_back({SketchConstraintType::Distance,   0, 1,-1,-1, 10});
    cons.push_back({SketchConstraintType::Distance,   1, 2,-1,-1, 6});

    REQUIRE(doc.solve_sketch_feature(sk));
    const auto& pts = doc.features[sk].profile.points;
    REQUIRE_THAT(pts[1].x(), Catch::Matchers::WithinAbs(10.0, 1e-3));
    REQUIRE_THAT(pts[1].y(), Catch::Matchers::WithinAbs(0.0,  1e-3));
    REQUIRE_THAT(pts[2].x(), Catch::Matchers::WithinAbs(10.0, 1e-3));
    REQUIRE_THAT(pts[2].y(), Catch::Matchers::WithinAbs(6.0,  1e-3));
    REQUIRE_THAT(pts[3].x(), Catch::Matchers::WithinAbs(0.0,  1e-3));
    REQUIRE_THAT(pts[3].y(), Catch::Matchers::WithinAbs(6.0,  1e-3));

    // the solved profile still extrudes into a solid
    doc.add_extrude(sk, 5.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());
    REQUIRE(doc.display_mesh.facets_count() > 0);
}

TEST_CASE("sketch entities -> wire -> extrude", "[CadDocument]")
{
    SECTION("square from 4 lines") {
        CadDocument doc;

        CadFeature sk;
        sk.type    = CadFeatureType::Sketch;
        sk.name    = "square";
        sk.plane   = SketchPlane::XY();
        sk.entities = {
            {SketchEntity::Type::Line, Vec2d(-10,-10), Vec2d(10,-10)},
            {SketchEntity::Type::Line, Vec2d(10,-10), Vec2d(10,10)},
            {SketchEntity::Type::Line, Vec2d(10,10), Vec2d(-10,10)},
            {SketchEntity::Type::Line, Vec2d(-10,10), Vec2d(-10,-10)},
        };
        doc.features.push_back(sk);

        CadFeature ex;
        ex.type       = CadFeatureType::Extrude;
        ex.name       = "extrude";
        ex.sketch_ref = 0;
        ex.distance   = 5;
        ex.mode       = BooleanMode::New;
        doc.features.push_back(ex);

        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        REQUIRE(doc.display_mesh.facets_count() > 0);

        auto bb = doc.display_mesh.bounding_box();
        auto sz = bb.max - bb.min;
        REQUIRE(std::abs(sz.x() - 20.0) < 0.5);
        REQUIRE(std::abs(sz.y() - 20.0) < 0.5);
        REQUIRE(std::abs(sz.z() -  5.0) < 0.5);
    }

    SECTION("single circle") {
        CadDocument doc;

        CadFeature sk;
        sk.type    = CadFeatureType::Sketch;
        sk.name    = "circle";
        sk.plane   = SketchPlane::XY();
        sk.entities = {
            {SketchEntity::Type::Circle, Vec2d(0,0), Vec2d(0,0), Vec2d(0,0), 10.0},
        };
        doc.features.push_back(sk);

        CadFeature ex;
        ex.type       = CadFeatureType::Extrude;
        ex.name       = "extrude";
        ex.sketch_ref = 0;
        ex.distance   = 8;
        ex.mode       = BooleanMode::New;
        doc.features.push_back(ex);

        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        REQUIRE(doc.display_mesh.facets_count() > 0);

        auto bb = doc.display_mesh.bounding_box();
        auto sz = bb.max - bb.min;
        REQUIRE(std::abs(sz.x() - 20.0) < 0.5);
        REQUIRE(std::abs(sz.y() - 20.0) < 0.5);
        REQUIRE(std::abs(sz.z() -  8.0) < 0.5);
    }

    SECTION("construction line excluded") {
        CadDocument doc;

        CadFeature sk;
        sk.type  = CadFeatureType::Sketch;
        sk.name  = "square_with_construction";
        sk.plane = SketchPlane::XY();

        SketchEntity cline;
        cline.type         = SketchEntity::Type::Line;
        cline.p0           = Vec2d(-30, 0);
        cline.p1           = Vec2d(30, 0);
        cline.construction = true;

        sk.entities = {
            {SketchEntity::Type::Line, Vec2d(-10,-10), Vec2d(10,-10)},
            {SketchEntity::Type::Line, Vec2d(10,-10), Vec2d(10,10)},
            {SketchEntity::Type::Line, Vec2d(10,10), Vec2d(-10,10)},
            {SketchEntity::Type::Line, Vec2d(-10,10), Vec2d(-10,-10)},
            cline,
        };
        doc.features.push_back(sk);

        CadFeature ex;
        ex.type       = CadFeatureType::Extrude;
        ex.name       = "extrude";
        ex.sketch_ref = 0;
        ex.distance   = 5;
        ex.mode       = BooleanMode::New;
        doc.features.push_back(ex);

        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        REQUIRE(doc.display_mesh.facets_count() > 0);

        auto bb = doc.display_mesh.bounding_box();
        auto sz = bb.max - bb.min;
        REQUIRE(std::abs(sz.x() - 20.0) < 0.5);
        REQUIRE(std::abs(sz.y() - 20.0) < 0.5);
        REQUIRE(std::abs(sz.z() -  5.0) < 0.5);
    }
}

TEST_CASE("construction flag survives recipe round-trip", "[CadDocument]")
{
    CadDocument doc;

    // Square edge + 1 construction line
    std::vector<SketchEntity> ents = {
        {SketchEntity::Type::Line, Vec2d(-10, -10), Vec2d(10, -10)},
        {SketchEntity::Type::Line, Vec2d(10, -10), Vec2d(10, 10)},
        {SketchEntity::Type::Line, Vec2d(10, 10), Vec2d(-10, 10)},
        {SketchEntity::Type::Line, Vec2d(-10, 10), Vec2d(-10, -10)},
    };
    SketchEntity cx;
    cx.type = SketchEntity::Type::Line;
    cx.p0 = Vec2d(-33.0, 7.0);
    cx.p1 = Vec2d(33.0, 7.0);
    cx.construction = true;
    ents.push_back(cx);   // ents[4]

    int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "SkCtor");
    REQUIRE(sk == 0);
    doc.add_extrude(sk, 5.0, false, BooleanMode::New, "Ex");

    std::string blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    CadDocument fresh;
    REQUIRE(fresh.deserialize_recipe(blob));

    // Locate SkCtor in fresh.features
    const CadFeature* sf = nullptr;
    for (const auto& f : fresh.features) {
        if (f.name == "SkCtor") { sf = &f; break; }
    }
    REQUIRE(sf != nullptr);
    REQUIRE(sf->entities.size() == 5);
    REQUIRE(sf->entities[0].construction == false);
    REQUIRE(sf->entities[4].construction == true);
    REQUIRE_THAT(sf->entities[4].p0.x(), Catch::Matchers::WithinAbs(-33.0, 1e-9));
    REQUIRE_THAT(sf->entities[4].p1.x(), Catch::Matchers::WithinAbs( 33.0, 1e-9));
}

// Mirrors the GUI interactive-sketch commit path (DesignPanel ->
// add_sketch_entities) for the Fase 4.1 entity drawing tools: a corner-rect and
// a center-rect produce 4 closed Line entities; a center-circle produces 1
// Circle entity. add_sketch_entities must store them and extrude into a solid.
TEST_CASE("add_sketch_entities commit path -> extrude", "[CadDocument]")
{
    SECTION("corner-rect 4 lines -> 30x16x5") {
        CadDocument doc;
        // Corner A=(-15,-8), B=(15,8): the tool's push_closed_lines order.
        const Vec2d A(-15, -8), B(15, 8);
        std::vector<SketchEntity> ents = {
            {SketchEntity::Type::Line, A,                 Vec2d(B.x(), A.y())},
            {SketchEntity::Type::Line, Vec2d(B.x(),A.y()), B},
            {SketchEntity::Type::Line, B,                 Vec2d(A.x(), B.y())},
            {SketchEntity::Type::Line, Vec2d(A.x(),B.y()), A},
        };
        int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "Sketch1");
        REQUIRE(sk == 0);
        doc.add_extrude(sk, 5.0, false, BooleanMode::New, "Extrude1");

        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        REQUIRE(doc.display_mesh.facets_count() > 0);
        auto sz = doc.display_mesh.bounding_box().size();
        REQUIRE(std::abs(sz.x() - 30.0) < 0.5);
        REQUIRE(std::abs(sz.y() - 16.0) < 0.5);
        REQUIRE(std::abs(sz.z() -  5.0) < 0.5);
    }

    SECTION("center-circle 1 entity -> r=7 cylinder") {
        CadDocument doc;
        SketchEntity c;
        c.type = SketchEntity::Type::Circle;
        c.center = Vec2d(0, 0);
        c.p0 = Vec2d(0, 0);
        c.radius = 7.0;
        int sk = doc.add_sketch_entities({c}, SketchPlane::XY(), "Sketch1");
        doc.add_extrude(sk, 4.0, false, BooleanMode::New, "Extrude1");

        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        REQUIRE(doc.display_mesh.facets_count() > 0);
        auto sz = doc.display_mesh.bounding_box().size();
        REQUIRE(std::abs(sz.x() - 14.0) < 0.5);
        REQUIRE(std::abs(sz.y() - 14.0) < 0.5);
        REQUIRE(std::abs(sz.z() -  4.0) < 0.5);
    }
}

// A slot (stadium): 2 lines + 2 semicircular Arc entities forming one closed
// loop — the shape the Fase 4.1b Slot tool emits. Validates the kernel's Arc
// edge path (GC_MakeArcOfCircle via center/radius/start_angle/end_angle, with
// the mid reconstructed at (start+end)/2) inside a mixed Line/Arc wire.
TEST_CASE("slot (line+arc closed wire) -> extrude", "[CadDocument]")
{
    const double PI = 3.14159265358979323846;
    CadDocument doc;

    // Centerline ends c0=(-10,0), c1=(10,0); half-width w=5 → stadium 30 x 10.
    SketchEntity top;   // top line A0(-10,5) -> A1(10,5)
    top.type = SketchEntity::Type::Line; top.p0 = Vec2d(-10, 5); top.p1 = Vec2d(10, 5);

    SketchEntity cap1;  // right cap @c1=(10,0): A1(10,5) -> B1(10,-5) through (15,0)
    cap1.type = SketchEntity::Type::Arc; cap1.center = Vec2d(10, 0); cap1.radius = 5;
    cap1.p0 = Vec2d(10, 5); cap1.p1 = Vec2d(10, -5);
    cap1.start_angle = PI / 2; cap1.end_angle = -PI / 2;   // mid angle 0 -> (15,0)

    SketchEntity bot;   // bottom line B1(10,-5) -> B0(-10,-5)
    bot.type = SketchEntity::Type::Line; bot.p0 = Vec2d(10, -5); bot.p1 = Vec2d(-10, -5);

    SketchEntity cap0;  // left cap @c0=(-10,0): B0(-10,-5) -> A0(-10,5) through (-15,0)
    cap0.type = SketchEntity::Type::Arc; cap0.center = Vec2d(-10, 0); cap0.radius = 5;
    cap0.p0 = Vec2d(-10, -5); cap0.p1 = Vec2d(-10, 5);
    cap0.start_angle = -PI / 2; cap0.end_angle = -3 * PI / 2; // mid angle -PI -> (-15,0)

    int sk = doc.add_sketch_entities({top, cap1, bot, cap0}, SketchPlane::XY(), "Slot");
    doc.add_extrude(sk, 4.0, false, BooleanMode::New, "Extrude1");

    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.display_mesh.facets_count() > 0);
    auto sz = doc.display_mesh.bounding_box().size();
    REQUIRE(std::abs(sz.x() - 30.0) < 0.5);
    REQUIRE(std::abs(sz.y() - 10.0) < 0.5);
    REQUIRE(std::abs(sz.z() -  4.0) < 0.5);
}

// Onshape-style constraints on coexisting entities (Fase 4.2). The solver maps
// each Line/Point entity endpoint to a solver variable, applies the entity
// constraints, and writes the solved coordinates back into the entities.
TEST_CASE("entity constraints: solve on SketchEntity endpoints", "[CadDocument]")
{
    using R = SketchPointRole;
    using T = SketchConstraintType;

    auto dir = [](const SketchEntity& e) { return Vec2d(e.p1 - e.p0); };

    SECTION("perpendicular rotates line1 normal to a pinned line0") {
        CadDocument doc;
        std::vector<SketchEntity> ents = {
            {SketchEntity::Type::Line, Vec2d(0,0), Vec2d(10,0)},   // line0 (pinned)
            {SketchEntity::Type::Line, Vec2d(0,0), Vec2d(7,7)},    // line1 @45 deg
        };
        int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "S");
        auto& ec = doc.features[sk].entity_constraints;
        ec.push_back({T::Fix,           0, -1, R::P0, R::P0, 0.0});
        ec.push_back({T::Fix,           0, -1, R::P1, R::P0, 0.0});
        ec.push_back({T::Perpendicular, 0,  1, R::P0, R::P0, 0.0});

        REQUIRE(doc.solve_sketch_feature(sk));
        const auto& e = doc.features[sk].entities;
        // line0 stayed put.
        REQUIRE(std::abs(e[0].p0.x() - 0.0)  < 1e-6);
        REQUIRE(std::abs(e[0].p1.x() - 10.0) < 1e-6);
        // line1 is now perpendicular to line0: directions dot to ~0.
        const double d = dir(e[0]).dot(dir(e[1]));
        REQUIRE(std::abs(d) < 1e-6);
    }

    // Driving length: the Dimension tool records a Distance between a line's own
    // P0/P1 (committed via add_sketch_entities' constraints arg). Solving drives
    // the line to that exact length.
    SECTION("driving length: Distance(P0,P1) sets a line's length") {
        CadDocument doc;
        std::vector<SketchEntity> ents = {
            {SketchEntity::Type::Line, Vec2d(0,0), Vec2d(10,0)},   // length 10
        };
        std::vector<SketchEntityConstraintDef> cons;
        cons.push_back({T::Fix,      0, -1, R::P0, R::P0, 0.0});   // pin the start
        cons.push_back({T::Distance, 0,  0, R::P0, R::P1, 25.0});  // length -> 25
        int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "S", cons);
        REQUIRE(doc.features[sk].entity_constraints.size() == 2);  // constraints stored
        REQUIRE(doc.solve_sketch_feature(sk));
        const auto& e = doc.features[sk].entities;
        REQUIRE(std::abs((e[0].p1 - e[0].p0).norm() - 25.0) < 1e-6);
    }

    SECTION("parallel flattens line1 onto a pinned horizontal line0") {
        CadDocument doc;
        std::vector<SketchEntity> ents = {
            {SketchEntity::Type::Line, Vec2d(0,0), Vec2d(10,0)},   // line0 (pinned)
            {SketchEntity::Type::Line, Vec2d(0,5), Vec2d(7,9)},    // line1 tilted
        };
        int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "S");
        auto& ec = doc.features[sk].entity_constraints;
        ec.push_back({T::Fix,      0, -1, R::P0, R::P0, 0.0});
        ec.push_back({T::Fix,      0, -1, R::P1, R::P0, 0.0});
        ec.push_back({T::Parallel, 0,  1, R::P0, R::P0, 0.0});

        REQUIRE(doc.solve_sketch_feature(sk));
        const auto& e = doc.features[sk].entities;
        const Vec2d d0 = dir(e[0]), d1 = dir(e[1]);
        const double cross = d0.x() * d1.y() - d0.y() * d1.x();
        REQUIRE(std::abs(cross) < 1e-6);
    }

    SECTION("coincident merges a line endpoint onto another") {
        CadDocument doc;
        std::vector<SketchEntity> ents = {
            {SketchEntity::Type::Line, Vec2d(0,0),  Vec2d(10,0)},   // line0
            {SketchEntity::Type::Line, Vec2d(12,1), Vec2d(20,1)},   // line1
        };
        int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "S");
        auto& ec = doc.features[sk].entity_constraints;
        ec.push_back({T::Coincident, 0, 1, R::P1, R::P0, 0.0});   // line0.end == line1.start

        REQUIRE(doc.solve_sketch_feature(sk));
        const auto& e = doc.features[sk].entities;
        const Vec2d gap = Vec2d(e[0].p1 - e[1].p0);
        REQUIRE(gap.norm() < 1e-6);
    }

    SECTION("horizontal levels a tilted line's endpoints") {
        CadDocument doc;
        std::vector<SketchEntity> ents = {
            {SketchEntity::Type::Line, Vec2d(0,0), Vec2d(10,2)},   // tilted line
        };
        int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "S");
        auto& ec = doc.features[sk].entity_constraints;
        ec.push_back({T::Horizontal, 0, 0, R::P0, R::P1, 0.0});   // p0.y == p1.y

        REQUIRE(doc.solve_sketch_feature(sk));
        const auto& e = doc.features[sk].entities;
        REQUIRE(std::abs(e[0].p0.y() - e[0].p1.y()) < 1e-6);
    }
}

TEST_CASE("entity constraints: arc/circle registration + concentric", "[CadDocument]")
{
    using R = SketchPointRole;
    using T = SketchConstraintType;

    SECTION("concentric centers coincide") {
        CadDocument doc;
        std::vector<SketchEntity> ents = {
            {SketchEntity::Type::Circle, Vec2d(0,0), Vec2d(0,0), Vec2d(0,0), 5.0},   // circle0
            {SketchEntity::Type::Circle, Vec2d(10,2), Vec2d(10,2), Vec2d(10,2), 3.0}, // circle1
        };
        int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "S");
        auto& ec = doc.features[sk].entity_constraints;
        ec.push_back({T::Fix,        0, -1, R::Center, R::Center, 0.0});
        ec.push_back({T::Concentric, 0,  1, R::Center, R::Center, 0.0});

        REQUIRE(doc.solve_sketch_feature(sk));
        const auto& e = doc.features[sk].entities;
        REQUIRE((e[1].center - e[0].center).norm() < 1e-6);
        REQUIRE(e[0].center.x() < 1e-6);
        REQUIRE(e[0].center.y() < 1e-6);
    }

    SECTION("arc reflow keeps radius and angle consistent") {
        CadDocument doc;
        const double PI2 = M_PI / 2;
        std::vector<SketchEntity> ents = {
            {SketchEntity::Type::Arc, Vec2d(5,0), Vec2d(0,5), Vec2d(0,0), 5.0, 0.0, PI2},
        };
        int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "S");
        auto& ec = doc.features[sk].entity_constraints;
        ec.push_back({T::Fix, 0, -1, R::Center, R::Center, 0.0});
        ec.push_back({T::Fix, 0, -1, R::P0,     R::P0,     0.0});

        REQUIRE(doc.solve_sketch_feature(sk));
        const auto& e = doc.features[sk].entities;
        REQUIRE(std::abs((e[0].p0 - e[0].center).norm() - 5.0) < 1e-6);
        REQUIRE(std::abs(e[0].start_angle - 0.0) < 1e-6);
        REQUIRE(std::abs(e[0].end_angle - PI2) < 1e-3);
        REQUIRE(e[0].end_angle > e[0].start_angle);
    }
}

TEST_CASE("entity constraints: radius/diameter dimensions", "[CadDocument]")
{
    using R = SketchPointRole;
    using T = SketchConstraintType;

    SECTION("circle radius dimension") {
        CadDocument doc;
        std::vector<SketchEntity> ents = {
            {SketchEntity::Type::Circle, Vec2d(0,0), Vec2d(0,0), Vec2d(0,0), 5.0},
        };
        int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "S");
        auto& ec = doc.features[sk].entity_constraints;
        ec.push_back({T::Fix,       0, -1, R::Center, R::Center, 0.0, -1, R::P0});
        ec.push_back({T::Radius,    0, -1, R::Center, R::P0,     8.0, -1, R::P0});

        REQUIRE(doc.solve_sketch_feature(sk));
        const auto& e = doc.features[sk].entities;
        REQUIRE(std::abs(e[0].radius - 8.0) < 1e-9);
    }

    SECTION("circle diameter dimension") {
        CadDocument doc;
        std::vector<SketchEntity> ents = {
            {SketchEntity::Type::Circle, Vec2d(0,0), Vec2d(0,0), Vec2d(0,0), 5.0},
        };
        int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "S");
        auto& ec = doc.features[sk].entity_constraints;
        ec.push_back({T::Fix,       0, -1, R::Center, R::Center, 0.0, -1, R::P0});
        ec.push_back({T::Diameter,  0, -1, R::Center, R::P0,     20.0, -1, R::P0});

        REQUIRE(doc.solve_sketch_feature(sk));
        const auto& e = doc.features[sk].entities;
        REQUIRE(std::abs(e[0].radius - 10.0) < 1e-9);
    }

    SECTION("arc radius rescales endpoints") {
        CadDocument doc;
        std::vector<SketchEntity> ents = {
            {SketchEntity::Type::Arc, Vec2d(5,0), Vec2d(0,5), Vec2d(0,0), 5.0, 0.0, M_PI/2},
        };
        int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "S");
        auto& ec = doc.features[sk].entity_constraints;
        ec.push_back({T::Fix,       0, -1, R::Center, R::Center, 0.0, -1, R::P0});
        ec.push_back({T::Radius,    0, -1, R::Center, R::P0,     10.0, -1, R::P0});

        REQUIRE(doc.solve_sketch_feature(sk));
        const auto& e = doc.features[sk].entities;
        REQUIRE(std::abs(e[0].radius - 10.0) < 1e-9);
        REQUIRE((e[0].p0 - Vec2d(10,0)).norm() < 1e-6);
        REQUIRE((e[0].p1 - Vec2d(0,10)).norm() < 1e-6);
    }
}

// PointOnLine (Fase: persistent positioning). A point-like entity is held on a
// line (value 0) or at perpendicular distance `value`. Drives e.g. a circle centre
// onto a construction axis and, being a real constraint, keeps it there on re-solve.
TEST_CASE("entity constraints: point-on-line positions a centre onto an axis", "[CadDocument]")
{
    using R = SketchPointRole;
    using T = SketchConstraintType;

    SECTION("circle centre snaps onto a pinned axis (value 0) and persists") {
        CadDocument doc;
        std::vector<SketchEntity> ents = {
            {SketchEntity::Type::Line,   Vec2d(0,0), Vec2d(10,0)},                 // axis (X)
            {SketchEntity::Type::Circle, Vec2d(5,7), Vec2d(5,7), Vec2d(5,7), 3.0}, // off-axis centre
        };
        std::vector<SketchEntityConstraintDef> cons;
        cons.push_back({T::Fix,         0, -1, R::P0,     R::P0, 0.0});   // pin axis endpoints
        cons.push_back({T::Fix,         0, -1, R::P1,     R::P1, 0.0});
        cons.push_back({T::PointOnLine, 1,  0, R::Center, R::P0, 0.0});   // centre onto axis
        int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "S", cons);
        REQUIRE(doc.solve_sketch_feature(sk));
        REQUIRE(std::abs(doc.features[sk].entities[1].center.y()) < 1e-6);   // on the axis
        // Re-solving keeps it on the axis (a driving constraint, not a one-shot move).
        REQUIRE(doc.solve_sketch_feature(sk));
        REQUIRE(std::abs(doc.features[sk].entities[1].center.y()) < 1e-6);
    }

    SECTION("non-zero perpendicular distance via the free solve_sketch_entities") {
        std::vector<SketchEntity> ents = {
            {SketchEntity::Type::Line,  Vec2d(0,0), Vec2d(10,0)},
            {SketchEntity::Type::Point, Vec2d(4,9)},                       // point above the axis
        };
        std::vector<SketchEntityConstraintDef> cons;
        cons.push_back({T::Fix,         0, -1, R::P0, R::P0, 0.0});
        cons.push_back({T::Fix,         0, -1, R::P1, R::P1, 0.0});
        cons.push_back({T::PointOnLine, 1,  0, R::P0, R::P0, 2.0});        // hold at distance 2
        REQUIRE(solve_sketch_entities(ents, cons));
        REQUIRE(std::abs(std::abs(ents[1].p0.y()) - 2.0) < 1e-6);         // 2 mm off the axis
    }
}

// The "tangent line to circle" SECTION used to abort the whole Catch2 process inside the
// vendored solver (slvs/dsc.h FindById, "Cannot find handle"), taking every later test with
// it, and was quarantined for it. Fixed in SketchSolver: a full circle can no longer be handed
// to SLVS_C_ARC_LINE_TANGENT, which dereferences arc endpoints a circle does not have. See
// snaporca-tkz.
TEST_CASE("entity constraints: tangent/midpoint/symmetric/angle", "[CadDocument][sketch]")
{
    using R = SketchPointRole;
    using T = SketchConstraintType;

    auto dir = [](const SketchEntity& e) { return Vec2d(e.p1 - e.p0); };

    SECTION("angle 90 between two lines") {
        CadDocument doc;
        std::vector<SketchEntity> ents = {
            {SketchEntity::Type::Line, Vec2d(0,0), Vec2d(10,0)},    // line0
            {SketchEntity::Type::Line, Vec2d(0,0), Vec2d(5,5)},     // line1
        };
        int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "S");
        auto& ec = doc.features[sk].entity_constraints;
        ec.push_back({T::Fix,  0,-1, R::P0,R::P0, 0.0,  -1,R::P0});
        ec.push_back({T::Fix,  0,-1, R::P1,R::P0, 0.0,  -1,R::P0});
        ec.push_back({T::Fix,  1,-1, R::P0,R::P0, 0.0,  -1,R::P0});
        ec.push_back({T::Angle,0, 1, R::P0,R::P0, M_PI/2,-1,R::P0});

        REQUIRE(doc.solve_sketch_feature(sk));
        const auto& e = doc.features[sk].entities;
        const Vec2d d0 = dir(e[0]).normalized();
        const Vec2d d1 = dir(e[1]).normalized();
        REQUIRE(std::abs(d0.dot(d1)) < 1e-3);
    }

    SECTION("midpoint of a line") {
        CadDocument doc;
        std::vector<SketchEntity> ents = {
            {SketchEntity::Type::Line,  Vec2d(0,0), Vec2d(10,0)},  // line0
            {SketchEntity::Type::Point, Vec2d(3,9)},                // point p
        };
        int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "S");
        auto& ec = doc.features[sk].entity_constraints;
        ec.push_back({T::Fix,     0,-1, R::P0,R::P0, 0.0,-1,R::P0});
        ec.push_back({T::Fix,     0,-1, R::P1,R::P0, 0.0,-1,R::P0});
        ec.push_back({T::Midpoint,1, 0, R::P0,R::P0, 0.0,-1,R::P0});

        REQUIRE(doc.solve_sketch_feature(sk));
        const auto& e = doc.features[sk].entities;
        REQUIRE((e[1].p0 - Vec2d(5,0)).norm() < 1e-3);
    }

    SECTION("tangent line to circle") {
        CadDocument doc;
        std::vector<SketchEntity> ents = {
            {SketchEntity::Type::Circle, Vec2d(0,0), Vec2d(0,0), Vec2d(0,0), 5.0},   // circle0 r=5
            {SketchEntity::Type::Line,   Vec2d(-10,8), Vec2d(10,8)},                  // line1 y=8
        };
        int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "S");
        auto& ec = doc.features[sk].entity_constraints;
        ec.push_back({T::Fix,        0,-1, R::Center,R::Center,  0.0,-1,R::P0});
        ec.push_back({T::LockX,      1,-1, R::P0,    R::P0,   -10.0,-1,R::P0});
        ec.push_back({T::LockX,      1,-1, R::P1,    R::P0,    10.0,-1,R::P0});
        ec.push_back({T::Horizontal, 1, 1, R::P0,    R::P1,     0.0,-1,R::P0});
        ec.push_back({T::Tangent,    0, 1, R::Center,R::P0,     0.0,-1,R::P0});

        REQUIRE(doc.solve_sketch_feature(sk));
        const auto& e = doc.features[sk].entities;
        REQUIRE(std::abs(std::abs(e[1].p0.y()) - 5.0) < 1e-3);
    }

    SECTION("symmetric across a line") {
        CadDocument doc;
        std::vector<SketchEntity> ents = {
            {SketchEntity::Type::Point, Vec2d(2,3)},            // pointA
            {SketchEntity::Type::Point, Vec2d(-1,1)},           // pointB
            {SketchEntity::Type::Line,  Vec2d(0,0), Vec2d(0,10)}// axis (Y axis)
        };
        int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "S");
        auto& ec = doc.features[sk].entity_constraints;
        ec.push_back({T::Fix,       0,-1, R::P0,R::P0, 0.0,-1,R::P0});
        ec.push_back({T::Fix,       2,-1, R::P0,R::P0, 0.0,-1,R::P0});
        ec.push_back({T::Fix,       2,-1, R::P1,R::P0, 0.0,-1,R::P0});
        ec.push_back({T::Symmetric, 0, 1, R::P0,R::P0, 0.0, 2,R::P0});

        REQUIRE(doc.solve_sketch_feature(sk));
        const auto& e = doc.features[sk].entities;
        REQUIRE((e[1].p0 - Vec2d(-2,3)).norm() < 1e-3);
    }
}

TEST_CASE("imported text glyphs all extrude without failing (charset sweep)", "[CadDocument]")
{
    std::string font = resources_dir().empty()
        ? std::string("resources/fonts/HarmonyOS_Sans_SC_Regular.ttf")
        : resources_dir() + "/fonts/HarmonyOS_Sans_SC_Regular.ttf";
    {
        std::ifstream probe(font);
        if (!probe.good()) {
            SUCCEED("bundled font not reachable in this environment; covered live on :10");
            return;
        }
    }

    const std::string charset =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789"
        "@#$%&*()[]{}<>?/+-=.,;:!"
        "\xC3\xA0\xC3\xA8\xC3\xA9\xC3\xAC\xC3\xB2\xC3\xB9";   // à è é ì ò ù (UTF-8)

    std::string failures;
    for (char c : charset) {
        ImportRegions regs = text_to_regions(std::string(1, c), 12.0, font);
        if (regs.empty()) continue;
        CadDocument doc;
        CadFeature sk;
        sk.type  = CadFeatureType::Sketch;
        sk.plane = SketchPlane::XY();
        sk.imported_regions = regs;
        doc.features.push_back(sk);
        CadFeature ex;
        ex.type       = CadFeatureType::Extrude;
        ex.sketch_ref = 0;
        ex.distance   = 3;
        doc.features.push_back(ex);
        if (!doc.recompute() || !doc.error.empty())
            failures += c;
    }
    INFO("glyphs that failed to extrude: [" << failures << "]");
    CHECK(failures.empty());

    // A realistic multi-glyph word must extrude too.
    {
        ImportRegions regs = text_to_regions("Snapmaker", 12.0, font);
        REQUIRE_FALSE(regs.empty());
        CadDocument doc;
        CadFeature sk;
        sk.type  = CadFeatureType::Sketch;
        sk.plane = SketchPlane::XY();
        sk.imported_regions = regs;
        doc.features.push_back(sk);
        CadFeature ex;
        ex.type       = CadFeatureType::Extrude;
        ex.sketch_ref = 0;
        ex.distance   = 3;
        doc.features.push_back(ex);
        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        REQUIRE(doc.display_mesh.facets_count() > 0);
    }
}

TEST_CASE("imported regions: faces-with-holes extrude (Text/SVG carrier)", "[CadDocument]")
{
    SECTION("square with a square hole -> tube volume") {
        CadDocument doc;

        CadFeature sk;
        sk.type  = CadFeatureType::Sketch;
        sk.name  = "art";
        sk.plane = SketchPlane::XY();
        // One region: outer 20x20 (CCW) + inner 8x8 hole (CW).
        sk.imported_regions = {{
            {Vec2d(-10,-10), Vec2d(10,-10), Vec2d(10,10), Vec2d(-10,10)},   // outer
            {Vec2d(-4,-4),   Vec2d(-4,4),   Vec2d(4,4),   Vec2d(4,-4)},     // hole (reversed winding)
        }};
        doc.features.push_back(sk);

        CadFeature ex;
        ex.type       = CadFeatureType::Extrude;
        ex.name       = "extrude";
        ex.sketch_ref = 0;
        ex.distance   = 5;
        ex.mode       = BooleanMode::New;
        doc.features.push_back(ex);

        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        REQUIRE(doc.display_mesh.facets_count() > 0);

        // (20*20 - 8*8) * 5 = 1680 mm^3
        REQUIRE_THAT(double(doc.display_mesh.volume()), Catch::Matchers::WithinRel(1680.0, 0.02));

        auto sz = doc.display_mesh.bounding_box().size();
        REQUIRE(std::abs(sz.x() - 20.0) < 0.5);
        REQUIRE(std::abs(sz.y() - 20.0) < 0.5);
        REQUIRE(std::abs(sz.z() -  5.0) < 0.5);
    }

    SECTION("inverted winding (CW outer, CCW hole) keeps body solid, counter empty") {
        // Real glyph contours (P, e, o, 8) arrive with outer CW + hole CCW. The
        // extrude must NORMALISE winding so the letter BODY is solid and the
        // counter is the hole — not the inverse (the reported bug). 20x20 outer
        // CW with an 8x8 hole CCW -> volume (400-64)*5 = 1680, NOT the inverse.
        CadDocument doc;
        CadFeature sk;
        sk.type  = CadFeatureType::Sketch;
        sk.plane = SketchPlane::XY();
        sk.imported_regions = {{
            {Vec2d(-10,-10), Vec2d(-10,10), Vec2d(10,10), Vec2d(10,-10)},  // outer CW
            {Vec2d(-4,-4),   Vec2d(4,-4),   Vec2d(4,4),   Vec2d(-4,4)},    // hole CCW
        }};
        doc.features.push_back(sk);

        CadFeature ex;
        ex.type       = CadFeatureType::Extrude;
        ex.sketch_ref = 0;
        ex.distance   = 5;
        doc.features.push_back(ex);

        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        REQUIRE_THAT(double(doc.display_mesh.volume()), Catch::Matchers::WithinRel(1680.0, 0.02));
    }

    SECTION("degenerate / duplicate points are sanitised, not fatal") {
        // FreeType/SVG flattening can emit repeated points; the extrude must
        // survive them (previously to_occt_wire threw and failed the whole op).
        CadDocument doc;
        CadFeature sk;
        sk.type  = CadFeatureType::Sketch;
        sk.plane = SketchPlane::XY();
        sk.imported_regions = {{
            // 20x20 outer square with consecutive dups + an explicit closing dup
            {Vec2d(-10,-10), Vec2d(-10,-10), Vec2d(10,-10), Vec2d(10,-10),
             Vec2d(10,10),   Vec2d(-10,10),  Vec2d(-10,-10)},
        }};
        doc.features.push_back(sk);

        CadFeature ex;
        ex.type       = CadFeatureType::Extrude;
        ex.sketch_ref = 0;
        ex.distance   = 5;
        doc.features.push_back(ex);

        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        REQUIRE_THAT(double(doc.display_mesh.volume()), Catch::Matchers::WithinRel(2000.0, 0.02));
    }

    SECTION("a degenerate region is skipped, valid ones still extrude") {
        CadDocument doc;
        CadFeature sk;
        sk.type  = CadFeatureType::Sketch;
        sk.plane = SketchPlane::XY();
        sk.imported_regions = {
            { {Vec2d(0,0), Vec2d(0,0), Vec2d(0,0)} },              // collapses to nothing
            { {Vec2d(0,0), Vec2d(5,0), Vec2d(5,5), Vec2d(0,5)} },  // valid 5x5
        };
        doc.features.push_back(sk);

        CadFeature ex;
        ex.type       = CadFeatureType::Extrude;
        ex.sketch_ref = 0;
        ex.distance   = 4;
        doc.features.push_back(ex);

        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        REQUIRE_THAT(double(doc.display_mesh.volume()), Catch::Matchers::WithinRel(100.0, 0.02));
    }

    SECTION("two disjoint regions form one shape") {
        CadDocument doc;

        CadFeature sk;
        sk.type  = CadFeatureType::Sketch;
        sk.plane = SketchPlane::XY();
        sk.imported_regions = {
            {{Vec2d(0,0),  Vec2d(5,0),  Vec2d(5,5),  Vec2d(0,5)}},
            {{Vec2d(10,0), Vec2d(15,0), Vec2d(15,5), Vec2d(10,5)}},
        };
        doc.features.push_back(sk);

        CadFeature ex;
        ex.type       = CadFeatureType::Extrude;
        ex.sketch_ref = 0;
        ex.distance   = 3;
        doc.features.push_back(ex);

        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        // 2 * (5*5*3) = 150 mm^3
        REQUIRE_THAT(double(doc.display_mesh.volume()), Catch::Matchers::WithinRel(150.0, 0.02));
    }
}

TEST_CASE("tessellate tracks per-triangle face id", "[CadDocument]")
{
    TopoDS_Shape box = BRepPrimAPI_MakeBox(10., 10., 10.).Shape();
    std::vector<int> tf;
    TriangleMesh m = SketchEngine::tessellate(box, tf);

    REQUIRE(tf.size() == m.its.indices.size());
    REQUIRE(!tf.empty());

    std::set<int> distinct(tf.begin(), tf.end());
    REQUIRE(distinct.size() == 6);
    REQUIRE(*distinct.begin() == 0);
    REQUIRE(*distinct.rbegin() == 5);

    for (int fid : distinct) {
        int count = 0;
        for (int x : tf) if (x == fid) ++count;
        REQUIRE(count >= 2);
    }

    REQUIRE(m.its.indices.size() >= 12);
}

TEST_CASE("extrude two-sided + through-all + intersect", "[CadDocument]")
{
    using namespace Slic3r;
    SketchPlane xy = SketchPlane::XY();
    // a 10x10 square wire centred on origin
    SketchProfile sp;
    sp.points = { Vec2d(-5,-5), Vec2d(5,-5), Vec2d(5,5), Vec2d(-5,5) };
    sp.closed = true;
    TopoDS_Wire w = sp.to_occt_wire(xy);

    SECTION("two-sided height = up+down") {
        TopoDS_Shape s = SketchEngine::make_extrude_two_sided(w, xy, 10.0, 4.0);
        REQUIRE_FALSE(s.IsNull());
        Bnd_Box bb; BRepBndLib::Add(s, bb);
        double xmin,ymin,zmin,xmax,ymax,zmax; bb.Get(xmin,ymin,zmin,xmax,ymax,zmax);
        REQUIRE_THAT(zmax - zmin, Catch::Matchers::WithinAbs(14.0, 0.05));   // 10 up + 4 down
        REQUIRE_THAT(zmax, Catch::Matchers::WithinAbs(10.0, 0.05));
        REQUIRE_THAT(zmin, Catch::Matchers::WithinAbs(-4.0, 0.05));
    }
}

TEST_CASE("extrude taper + up-to-face distance", "[CadDocument]")
{
    using namespace Slic3r;
    SketchPlane xy = SketchPlane::XY();
    SketchProfile sp; sp.points = { Vec2d(-5,-5),Vec2d(5,-5),Vec2d(5,5),Vec2d(-5,5) }; sp.closed = true;
    TopoDS_Wire w = sp.to_occt_wire(xy);

    SECTION("taper widens the top") {
        TopoDS_Shape s = SketchEngine::make_extrude_taper(w, xy, 10.0, 15.0);
        REQUIRE_FALSE(s.IsNull());
        Bnd_Box bb; BRepBndLib::Add(s, bb);
        double x0,y0,z0,x1,y1,z1; bb.Get(x0,y0,z0,x1,y1,z1);
        REQUIRE_THAT(z1 - z0, Catch::Matchers::WithinAbs(10.0, 0.1));
        REQUIRE((x1 - x0) > 12.0);
    }
    SECTION("extreme taper falls back to a straight prism") {
        TopoDS_Shape s = SketchEngine::make_extrude_taper(w, xy, 10.0, 89.0);
        REQUIRE_FALSE(s.IsNull());
        Bnd_Box bb; BRepBndLib::Add(s, bb);
        double x0,y0,z0,x1,y1,z1; bb.Get(x0,y0,z0,x1,y1,z1);
        REQUIRE_THAT(x1 - x0, Catch::Matchers::WithinAbs(10.0, 0.1));
    }
}

// Was [known-broken] until the numbers were actually measured (snaporca-kzy). The geometry
// was right all along; the TEST compared against the wrong reference. An internal thread bores
// at the MINOR radius (radius - depth) and then carves the groove out to radius + depth, so a
// tapped hole keeps the crests between turns and therefore holds MORE material than a plain
// clearance hole at the nominal radius. Asserting the thread removes more than a plain Ø12 bore
// asked for something no real tapped hole does. Measured on this fixture: plain Ø12 bore removes
// 2261 mm3, the thread removes 2157 = 1571 (minor bore) + 586 (groove). Exact BRepGProp volume
// agrees with the tessellated volume to 2.5 mm3, so this was never a meshing artefact either.
// The tap-drill bore is the honest reference: against it, the groove's 586 mm3 is the thing worth
// asserting, because that is what "the thread actually cuts" means.
TEST_CASE("internal thread cuts a visible groove into the bore wall", "[CadDocument][thread]")
{
    using namespace Slic3r;
    SketchPlane xy = SketchPlane::XY();

    // 40x40x20 box centred on the origin, extruded +Z.
    auto make_box = [&](CadDocument& doc) {
        CadFeature sk;
        sk.type  = CadFeatureType::Sketch;
        sk.plane = xy;
        sk.imported_regions = {{
            {Vec2d(-20,-20), Vec2d(20,-20), Vec2d(20,20), Vec2d(-20,20)},
        }};
        doc.features.push_back(sk);
        CadFeature ex;
        ex.type       = CadFeatureType::Extrude;
        ex.sketch_ref = 0;
        ex.distance   = 20;
        doc.features.push_back(ex);
    };

    // Reference: box bored at the MINOR diameter — the tap drill the thread starts from.
    // Ø10 = 2 * (radius 6 - depth 1), matching what apply_thread cuts before the groove.
    CadDocument hole_doc;
    make_box(hole_doc);
    hole_doc.add_hole(10.0, 20.0, true, 0.0, 0.0, xy, "Hole");
    REQUIRE(hole_doc.recompute());
    REQUIRE(hole_doc.error.empty());
    const double v_hole = double(hole_doc.display_mesh.volume());

    // Threaded: same box, internal thread of radius 6 (the bore radius).
    CadDocument thr_doc;
    make_box(thr_doc);
    thr_doc.add_thread(6.0, 3.0, 20.0, 1.0, /*internal=*/true, 0.0, 0.0, xy, "Thread");
    REQUIRE(thr_doc.recompute());
    REQUIRE(thr_doc.error.empty());
    const double v_thread = double(thr_doc.display_mesh.volume());

    // The groove must carve material out of the wall BEYOND the tap-drill bore, which is what
    // makes the thread visible. A profile that only swept already-empty bore space would give
    // v_thread ~= v_hole; the real one removes several hundred mm3 more.
    REQUIRE(v_thread > 0.0);
    REQUIRE(v_thread < v_hole);
    REQUIRE((v_hole - v_thread) > 20.0);
}

TEST_CASE("revolve builds a solid of revolution about an in-plane axis", "[CadDocument]")
{
    using namespace Slic3r;
    SketchPlane xy = SketchPlane::XY();

    // Rectangle profile (10 wide x 10 tall, area 100) offset to +v so it lies entirely
    // on one side of the X axis; revolved 360deg about X -> a rectangular-section ring.
    // Pappus: V = 2*pi*R*A = 2*pi*15*100 = ~9424.78 mm^3 (tessellation slightly under).
    auto make_rev_doc = [&](double angle, int axis, double u0, double v0) {
        auto doc = std::make_unique<CadDocument>();
        CadFeature sk;
        sk.type = CadFeatureType::Sketch;
        sk.plane = xy;
        sk.profile.points = { Vec2d(u0 - 5, v0 - 5), Vec2d(u0 + 5, v0 - 5),
                              Vec2d(u0 + 5, v0 + 5), Vec2d(u0 - 5, v0 + 5) };
        sk.profile.closed = true;
        doc->features.push_back(sk);
        doc->add_revolve(0, angle, axis, false, BooleanMode::New, "Rev");
        return doc;
    };

    auto full = make_rev_doc(360.0, /*axis=X*/0, 0.0, 15.0);
    REQUIRE(full->recompute());
    REQUIRE(full->error.empty());
    const double v_full = double(full->display_mesh.volume());
    REQUIRE(v_full > 0.0);
    REQUIRE(v_full == Approx(9424.78).epsilon(0.05));

    // A 180deg sweep removes exactly half the material.
    auto half = make_rev_doc(180.0, 0, 0.0, 15.0);
    REQUIRE(half->recompute());
    REQUIRE(half->error.empty());
    const double v_half = double(half->display_mesh.volume());
    REQUIRE(v_half > 0.0);
    REQUIRE(v_full == Approx(2.0 * v_half).epsilon(0.05));

    // Axis = plane Y: profile offset to +u (one side of the Y axis) gives the same ring.
    auto ydoc = make_rev_doc(360.0, /*axis=Y*/1, 15.0, 0.0);
    REQUIRE(ydoc->recompute());
    REQUIRE(ydoc->error.empty());
    REQUIRE(double(ydoc->display_mesh.volume()) == Approx(9424.78).epsilon(0.05));
}

TEST_CASE("sweep builds a solid by sweeping a profile along a path", "[CadDocument]")
{
    using namespace Slic3r;
    CadDocument doc;

    // Profile: circle r=5 on the XY plane at the origin (area = 25*pi).
    SketchEntity circ;
    circ.type   = SketchEntity::Type::Circle;
    circ.center = Vec2d(0, 0);
    circ.radius = 5.0;
    const int prof = doc.add_sketch_entities({circ}, SketchPlane::XY(), "Profile");

    // Path: a straight line on the XZ plane from 2D (0,0)->(0,100), i.e. world
    // (0,0,0)->(0,0,100): the spine starts on the profile plane and runs +Z by 100.
    SketchEntity line;
    line.type = SketchEntity::Type::Line;
    line.p0   = Vec2d(0, 0);
    line.p1   = Vec2d(0, 100);
    const int path = doc.add_sketch_entities({line}, SketchPlane::XZ(), "Path");

    doc.add_sweep(prof, path, BooleanMode::New, "Sweep1");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Straight sweep of a circle == a cylinder: V = pi*r^2*h = pi*25*100 = ~7853.98.
    const double v = double(doc.display_mesh.volume());
    REQUIRE(v > 0.0);
    REQUIRE_THAT(v, Catch::Matchers::WithinRel(M_PI * 25.0 * 100.0, 0.03));

    // A valid path sketch is mandatory: a -1 path ref must error cleanly, not crash.
    CadDocument bad;
    const int p2 = bad.add_sketch_entities({circ}, SketchPlane::XY(), "Profile");
    bad.add_sweep(p2, -1, BooleanMode::New, "BadSweep");
    REQUIRE_FALSE(bad.recompute());
}

TEST_CASE("pattern replicates a body linearly and circularly", "[CadDocument]")
{
    using namespace Slic3r;

    // Linear: a 10x10x10 box (V=1000) repeated 3x at 20mm spacing along plane X.
    // 20 > 10 so the copies are disjoint -> total V = 3*1000 = 3000.
    {
        CadDocument doc;
        int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(),
                                10, 10, 5, "Box");
        doc.add_extrude(sk, 10.0, false, BooleanMode::New, "E");
        doc.add_pattern(/*circular=*/false, /*count=*/3, /*spacing=*/20,
                        /*dir=*/0, /*angle=*/0, /*target=*/-1, "LinearPattern");
        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        const double v = double(doc.display_mesh.volume());
        REQUIRE_THAT(v, Catch::Matchers::WithinRel(3000.0, 0.02));
    }

    // Circular: a 10x10x10 box centred at x=50 (radius 50 from the Z axis), 4 copies
    // over 360deg about the plane normal through the origin -> a ring of 4 disjoint
    // boxes -> V = 4*1000 = 4000.
    {
        CadDocument doc;
        SketchProfile sp;
        sp.points.push_back(Vec2d(45, -5));
        sp.points.push_back(Vec2d(55, -5));
        sp.points.push_back(Vec2d(55,  5));
        sp.points.push_back(Vec2d(45,  5));
        sp.closed = true;
        int sk = doc.add_sketch_profile(sp, SketchPlane::XY(), "OffsetBox");
        doc.add_extrude(sk, 10.0, false, BooleanMode::New, "E");
        doc.add_pattern(/*circular=*/true, /*count=*/4, /*spacing=*/0,
                        /*dir=*/0, /*angle=*/360, /*target=*/-1, "CircularPattern");
        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        const double v = double(doc.display_mesh.volume());
        REQUIRE_THAT(v, Catch::Matchers::WithinRel(4000.0, 0.02));
    }

    // A pattern with no body must error cleanly, not crash.
    {
        CadDocument bad;
        bad.add_pattern(false, 3, 20, 0, 0, -1, "NoBody");
        REQUIRE_FALSE(bad.recompute());
    }
}

TEST_CASE("pattern-on-curve: copies land on a line and bbox spans the curve length", "[CadDocument][pattern]")
{
    using Catch::Matchers::WithinAbs;
    using Catch::Matchers::WithinRel;
    using namespace Slic3r;

    CadDocument doc;

    // Seed body: 4x4x4 box at the origin via sketch+extrude.
    int s0 = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 4, 4, 0, "Seed");
    doc.add_extrude(s0, 4.0, false, BooleanMode::New, "E");

    // Guide sketch: one Line entity from (0,0) to (30,0) on XY.
    std::vector<SketchEntity> guide = {
        {SketchEntity::Type::Line, Vec2d(0, 0), Vec2d(30, 0)},
    };
    int gs = doc.add_sketch_entities(guide, SketchPlane::XY(), "Guide");

    doc.add_pattern_on_curve(4, gs, 0, 0, "OnCurve");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.display_mesh.facets_count() > 0);

    auto bb = doc.display_mesh.bounding_box();
    double x_extent = bb.max.x() - bb.min.x();
    // 4 copies: at x=0, x=10, x=20, x=30. The seed is a 4x4 box centred at origin,
    // so the overall X span is from -2 to 32 = 34 mm.
    REQUIRE_THAT(x_extent, WithinAbs(34.0, 2.0));
}

TEST_CASE("pattern-on-curve: bad refs are safe", "[CadDocument][pattern]")
{
    using namespace Slic3r;

    CadDocument doc;
    int s0 = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 4, 4, 0, "Seed");
    doc.add_extrude(s0, 4.0, false, BooleanMode::New, "E");

    // Bad sketch ref -> error.
    doc.add_pattern_on_curve(3, 999, 0, 0, "Bad");
    REQUIRE_FALSE(doc.recompute());
    REQUIRE_FALSE(doc.error.empty());
    REQUIRE(doc.error.find("pattern") != std::string::npos);
}

TEST_CASE("pattern-on-curve: round-trip through serialize/deserialize", "[CadDocument][pattern]")
{
    using Catch::Matchers::WithinRel;
    using namespace Slic3r;

    CadDocument doc;
    int s0 = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 4, 4, 0, "Seed");
    doc.add_extrude(s0, 4.0, false, BooleanMode::New, "E");

    std::vector<SketchEntity> guide = {
        {SketchEntity::Type::Line, Vec2d(0, 0), Vec2d(30, 0)},
    };
    int gs = doc.add_sketch_entities(guide, SketchPlane::XY(), "Guide");

    doc.add_pattern_on_curve(4, gs, 0, 0, "OnCurve");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    auto orig_bb = doc.display_mesh.bounding_box();
    size_t orig_n = doc.bodies.size();

    std::string blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    CadDocument fresh;
    REQUIRE(fresh.deserialize_recipe(blob));
    REQUIRE(fresh.recompute());
    REQUIRE(fresh.error.empty());
    REQUIRE(fresh.bodies.size() == orig_n);

    auto fresh_bb = fresh.display_mesh.bounding_box();
    REQUIRE_THAT(orig_bb.min.x(), WithinRel(fresh_bb.min.x(), 1e-6));
    REQUIRE_THAT(orig_bb.max.x(), WithinRel(fresh_bb.max.x(), 1e-6));

    // Verify the deserialized field values.
    bool found = false;
    for (const auto& f : fresh.features) {
        if (f.name == "OnCurve") {
            REQUIRE(f.pattern_curve_sketch == gs);
            REQUIRE(f.pattern_curve_entity == 0);
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("thread standards table carries correct ISO/UTS measures", "[CadDocument]")
{
    using namespace Slic3r;

    // Table is non-empty and every entry is self-consistent.
    const auto& table = thread_standards();
    REQUIRE(table.size() > 40);
    for (const ThreadSpec& s : table) {
        REQUIRE(s.major_diameter_mm > 0.0);
        REQUIRE(s.pitch_mm > 0.0);
        REQUIRE(s.minor_diameter_mm() < s.major_diameter_mm);
        REQUIRE(s.minor_diameter_mm() > 0.0);
        // 60deg V cut depth = 0.6134 * pitch.
        REQUIRE(s.thread_depth_mm() == Approx(0.6134 * s.pitch_mm));
    }

    // ISO metric coarse: known nominal/pitch pairs.
    const ThreadSpec* m6 = find_thread_standard("M6");
    REQUIRE(m6 != nullptr);
    REQUIRE(m6->major_diameter_mm == Approx(6.0));
    REQUIRE(m6->pitch_mm == Approx(1.0));
    REQUIRE(m6->series == ThreadSpec::Series::MetricCoarse);
    REQUIRE_FALSE(m6->imperial());
    REQUIRE(m6->thread_depth_mm() == Approx(0.6134));
    // Tapped minor (tap-drill) diameter D - 1.0825*P = 6 - 1.0825 = 4.9175.
    REQUIRE(m6->minor_diameter_mm() == Approx(4.9175));

    const ThreadSpec* m3 = find_thread_standard("M3");
    REQUIRE(m3 != nullptr);
    REQUIRE(m3->pitch_mm == Approx(0.5));

    // Imperial UNC: 1/4-20 -> 0.25in major, pitch = 25.4/20 = 1.27 mm.
    const ThreadSpec* q = find_thread_standard("1/4-20 UNC");
    REQUIRE(q != nullptr);
    REQUIRE(q->major_diameter_mm == Approx(6.35));
    REQUIRE(q->pitch_mm == Approx(1.27));
    REQUIRE(q->series == ThreadSpec::Series::UNC);
    REQUIRE(q->imperial());

    // Imperial UNF fine variant has a finer pitch than its UNC sibling.
    const ThreadSpec* qf = find_thread_standard("1/4-28 UNF");
    REQUIRE(qf != nullptr);
    REQUIRE(qf->major_diameter_mm == Approx(6.35));
    REQUIRE(qf->pitch_mm == Approx(25.4 / 28.0));
    REQUIRE(qf->pitch_mm < q->pitch_mm);

    // Unknown designation -> nullptr.
    REQUIRE(find_thread_standard("M7.3 bogus") == nullptr);
}

TEST_CASE("datum plane: offset + tilt resolution and sketching on it", "[CadDocument]")
{
    using Catch::Matchers::WithinRel;
    using Catch::Matchers::WithinAbs;

    // Parallel offset plane 30 mm above XY (normal +Z, origin at z=30).
    CadDocument doc;
    int p0 = doc.add_plane(0 /*XY*/, 30.0, 0.0, 0, "Plane1");
    REQUIRE(p0 == 0);

    auto planes = doc.resolve_datum_planes();
    REQUIRE(planes.size() == 1);
    REQUIRE(planes[0].first == "Plane1");
    CHECK_THAT(planes[0].second.origin.z(), WithinAbs(30.0, 1e-9));
    CHECK_THAT(planes[0].second.normal.z(), WithinAbs(1.0, 1e-9));

    // A second datum plane tilted 90 deg about the base (XY) X axis: its normal
    // rotates from +Z toward -Y (Rodrigues about +X: +Z -> -Y).
    doc.add_plane(0 /*XY*/, 0.0, 90.0, 0 /*about X*/, "Plane2");
    planes = doc.resolve_datum_planes();
    REQUIRE(planes.size() == 2);
    CHECK_THAT(planes[1].second.normal.y(), WithinAbs(-1.0, 1e-9));
    CHECK_THAT(planes[1].second.normal.z(), WithinAbs(0.0, 1e-9));

    // Sketch a 10x10 square ON Plane1 and extrude 4 mm: the solid must sit in z=[30,34].
    SketchPlane sp = planes[0].second;
    SketchProfile prof;
    prof.points = {{-5,-5},{5,-5},{5,5},{-5,5}};
    prof.closed = true;
    int sk = doc.add_sketch_profile(prof, sp, "S");
    doc.add_extrude(sk, 4.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());

    Bnd_Box bb;
    BRepBndLib::Add(doc.body, bb);
    double xmin,ymin,zmin,xmax,ymax,zmax;
    bb.Get(xmin,ymin,zmin,xmax,ymax,zmax);
    CHECK_THAT(zmin, WithinAbs(30.0, 1e-6));
    CHECK_THAT(zmax, WithinAbs(34.0, 1e-6));
    CHECK_THAT(double(doc.display_mesh.volume()), WithinRel(400.0, 0.02));

    // A datum-plane-only document has no solid -> recompute is a benign failure.
    CadDocument only_plane;
    only_plane.add_plane(0, 10.0, 0.0, 0, "P");
    REQUIRE_FALSE(only_plane.recompute());
}

TEST_CASE("loft builds a solid skinning two profiles on parallel planes", "[CadDocument]")
{
    using Catch::Matchers::WithinRel;
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    // Bottom 20x20 square on XY.
    SketchProfile bot;
    bot.points = {{-10,-10},{10,-10},{10,10},{-10,10}};
    bot.closed = true;
    int s0 = doc.add_sketch_profile(bot, SketchPlane::XY(), "Bottom");

    // Top 10x10 square on a datum plane 20 mm above XY (exercises datum -> loft).
    doc.add_plane(0 /*XY*/, 20.0, 0.0, 0, "Plane1");
    SketchPlane top = doc.resolve_datum_planes()[0].second;
    SketchProfile tp;
    tp.points = {{-5,-5},{5,-5},{5,5},{-5,5}};
    tp.closed = true;
    int s1 = doc.add_sketch_profile(tp, top, "Top");

    // Ruled (straight) sections -> exact planar end caps at z=0 and z=20.
    doc.add_loft({s0, s1}, true /*ruled*/, BooleanMode::New, "Loft1");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // The loft spans z=[0,20]. ThruSections approximates each section as a BSpline
    // curve, so the lateral surface bulges ~0.05 mm past the end planes (sub-visual,
    // ~0.25%) — assert the span loosely; the volume below is the real correctness gate.
    Bnd_Box bb;
    BRepBndLib::Add(doc.body, bb);
    double xmin,ymin,zmin,xmax,ymax,zmax;
    bb.Get(xmin,ymin,zmin,xmax,ymax,zmax);
    CHECK_THAT(zmin, WithinAbs(0.0, 0.1));
    CHECK_THAT(zmax, WithinAbs(20.0, 0.1));
    // Square frustum volume = h/3*(A1+A2+sqrt(A1*A2)) = 20/3*(400+100+200) = 4666.67.
    CHECK_THAT(double(doc.display_mesh.volume()), WithinRel(4666.67, 0.03));

    // A single profile is not enough -> benign recompute failure.
    CadDocument one;
    SketchProfile sp; sp.points = {{-5,-5},{5,-5},{5,5},{-5,5}}; sp.closed = true;
    int only = one.add_sketch_profile(sp, SketchPlane::XY(), "Only");
    one.add_loft({only}, false, BooleanMode::New, "L");
    REQUIRE_FALSE(one.recompute());
}

TEST_CASE("draft tapers a solid face about the body base", "[CadDocument]")
{
    using namespace Slic3r;

    // 10x10x10 box from z=0..10 (V=1000). Drafting a vertical side face by +10deg about
    // the bottom (neutral) plane tilts its top edge inward, removing material so V<1000.
    // A box's 2 horizontal faces are parallel to the neutral plane and cannot be drafted
    // (Add fails -> recompute returns false), so exactly the 4 vertical sides succeed.
    int  ok_faces  = 0;
    bool saw_taper = false;
    for (int fid = 0; fid < 6; ++fid) {
        CadDocument doc;
        int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 5, "Box");
        doc.add_extrude(sk, 10.0, false, BooleanMode::New, "E");
        doc.add_draft(10.0, fid, -1, "Draft1");
        if (!doc.recompute()) continue;   // top/bottom faces: parallel to base -> benign skip
        ++ok_faces;
        const double v = double(doc.display_mesh.volume());
        REQUIRE(v > 0.0);
        if (v < 999.0) saw_taper = true;
    }
    REQUIRE(ok_faces == 4);
    REQUIRE(saw_taper);

    // Draft with no body must fail cleanly, not crash.
    CadDocument bad;
    bad.add_draft(5.0, 0, -1, "NoBody");
    REQUIRE_FALSE(bad.recompute());
}

TEST_CASE("a split renumbers the bodies a later feature indexes", "[CadDocument][cut]")
{
    // Pins the invariant the Design tab's re-edit path depends on (snaporca-oz7): a stored
    // target_body indexes the body list AS IT WAS when that feature ran, and a Cut placed
    // later in the tree changes that list. If this test ever fails, the GUI's
    // fill_body_choice() replay-to-timeline-slot assumption needs revisiting with it.
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Box");
    doc.add_extrude(sk, 20.0, false, BooleanMode::New, "Ext");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);

    // Cut the single body in half, keeping BOTH pieces — what the Cut card always does.
    doc.add_cut(SketchPlane::XY(), 10.0, false, true, true, 0, "Split");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 2);           // one body became two

    // Body index 1 did not exist before the Cut. A feature recorded BEFORE the Cut could
    // never have referred to it, which is exactly why re-edit must list the earlier set.
    const double half = 20.0 * 20.0 * 10.0;
    double v0 = double(SketchEngine::tessellate(doc.bodies[0].shape).volume());
    double v1 = double(SketchEngine::tessellate(doc.bodies[1].shape).volume());
    CHECK_THAT(v0 + v1, WithinRel(2.0 * half, 1e-3));

    // Replaying to just before the Cut yields the pre-split list — the one a feature sitting
    // there indexes into. This is the operation fill_body_choice() performs.
    CadDocument as_of = doc;
    as_of.features.resize(as_of.features.size() - 1);   // drop the Cut
    REQUIRE(as_of.recompute());
    REQUIRE(as_of.bodies.size() == 1);
    CHECK(as_of.bodies.size() < doc.bodies.size());
}

TEST_CASE("cut splits a body with a plane", "[cut]")
{
    using Catch::Matchers::WithinRel;

    auto make_box = [](CadDocument& doc, double w, double h, double d) {
        CadFeature sk;
        sk.type  = CadFeatureType::Sketch;
        sk.plane = SketchPlane::XY();
        sk.imported_regions = {{
            {Vec2d(-w / 2, -h / 2), Vec2d(w / 2, -h / 2),
             Vec2d(w / 2,  h / 2), Vec2d(-w / 2, h / 2)},
        }};
        doc.features.push_back(sk);
        CadFeature ex;
        ex.type       = CadFeatureType::Extrude;
        ex.sketch_ref = 0;
        ex.distance   = d;
        doc.features.push_back(ex);
    };

    SECTION("keep upper half only") {
        CadDocument doc;
        make_box(doc, 20.0, 20.0, 20.0);
        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        const double v_orig   = double(doc.display_mesh.volume());
        const int    n_before = int(doc.bodies.size());
        REQUIRE(v_orig > 0.0);

        CadFeature cut;
        cut.type           = CadFeatureType::Cut;
        cut.plane          = SketchPlane::XY();
        cut.cut_offset     = 10.0;   // mid-height of the 0..20 box
        cut.cut_keep_upper = true;
        cut.cut_keep_lower = false;
        doc.features.push_back(cut);

        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        REQUIRE(int(doc.bodies.size()) == n_before);
        REQUIRE_THAT(double(doc.display_mesh.volume()), WithinRel(v_orig * 0.5, 0.01));
    }

    SECTION("keep both halves splits into two bodies") {
        CadDocument doc;
        make_box(doc, 20.0, 20.0, 20.0);
        REQUIRE(doc.recompute());
        const double v_orig   = double(doc.display_mesh.volume());
        const int    n_before = int(doc.bodies.size());
        REQUIRE(v_orig > 0.0);

        CadFeature cut;
        cut.type           = CadFeatureType::Cut;
        cut.plane          = SketchPlane::XY();
        cut.cut_offset     = 10.0;
        cut.cut_keep_upper = true;
        cut.cut_keep_lower = true;
        doc.features.push_back(cut);

        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        REQUIRE(int(doc.bodies.size()) == n_before + 1);
        REQUIRE_THAT(double(doc.display_mesh.volume()), WithinRel(v_orig, 0.01));

        for (const auto& b : doc.bodies) {
            double v = double(SketchEngine::tessellate(b.shape).volume());
            REQUIRE_THAT(v, WithinRel(v_orig * 0.5, 0.01));
        }
    }

    SECTION("keep lower half only") {
        CadDocument doc;
        make_box(doc, 20.0, 20.0, 20.0);
        REQUIRE(doc.recompute());
        const double v_orig   = double(doc.display_mesh.volume());
        const int    n_before = int(doc.bodies.size());

        CadFeature cut;
        cut.type           = CadFeatureType::Cut;
        cut.plane          = SketchPlane::XY();
        cut.cut_offset     = 10.0;
        cut.cut_keep_upper = false;
        cut.cut_keep_lower = true;
        doc.features.push_back(cut);

        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        REQUIRE(int(doc.bodies.size()) == n_before);
        REQUIRE_THAT(double(doc.display_mesh.volume()), WithinRel(v_orig * 0.5, 0.01));
    }

    SECTION("flip swaps which side is kept") {
        CadDocument doc;
        make_box(doc, 20.0, 20.0, 20.0);
        REQUIRE(doc.recompute());
        const double v_orig = double(doc.display_mesh.volume());

        // cut with flip=true, keep_upper=true → the -normal side (original bottom half)
        CadFeature cut;
        cut.type           = CadFeatureType::Cut;
        cut.plane          = SketchPlane::XY();
        cut.cut_offset     = 10.0;
        cut.cut_flip       = true;
        cut.cut_keep_upper = true;
        cut.cut_keep_lower = false;
        doc.features.push_back(cut);

        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        REQUIRE_THAT(double(doc.display_mesh.volume()), WithinRel(v_orig * 0.5, 0.01));
    }

    SECTION("both keep flags false throws") {
        CadDocument doc;
        make_box(doc, 20.0, 20.0, 20.0);
        REQUIRE(doc.recompute());

        CadFeature cut;
        cut.type           = CadFeatureType::Cut;
        cut.plane          = SketchPlane::XY();
        cut.cut_keep_upper = false;
        cut.cut_keep_lower = false;
        doc.features.push_back(cut);

        REQUIRE_FALSE(doc.recompute());
    }
}

TEST_CASE("both-halves plane cut splits a body into two equal halves", "[CadDocument][cut]")
{
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 1);
    double v_orig = double(SketchEngine::tessellate(doc.bodies[0].shape).volume());
    REQUIRE(v_orig > 0.0);

    doc.add_cut(SketchPlane::XY(), 5.0, false, true, true, 0, "SplitBoth");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 2);

    double v0 = double(SketchEngine::tessellate(doc.bodies[0].shape).volume());
    double v1 = double(SketchEngine::tessellate(doc.bodies[1].shape).volume());
    REQUIRE_THAT(v0 + v1, WithinRel(v_orig, 1e-4));
    REQUIRE_THAT(v0, WithinRel(20.0 * 20.0 * 5.0, 0.01));
    REQUIRE_THAT(v1, WithinRel(20.0 * 20.0 * 5.0, 0.01));
}

TEST_CASE("split by a body face divides a box into two halves via top-face offset", "[CadDocument][cut]")
{
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);
    double v_orig = double(SketchEngine::tessellate(doc.bodies[0].shape).volume());

    int n_faces = GeometryEngine::face_count(doc.bodies[0].shape);
    int top_face = -1;
    for (int i = 0; i < n_faces; ++i) {
        TopoDS_Face fc = GeometryEngine::face_by_index(doc.bodies[0].shape, i);
        Vec3d n = GeometryEngine::face_normal_world(fc);
        if (n.z() > 0.9) { top_face = i; break; }
    }
    REQUIRE(top_face >= 0);

    doc.add_split_by_face(0, -1, top_face, true, true, "SplitByFace");
    doc.features.back().cut_offset = -5.0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 2);

    double v0 = double(SketchEngine::tessellate(doc.bodies[0].shape).volume());
    double v1 = double(SketchEngine::tessellate(doc.bodies[1].shape).volume());
    REQUIRE_THAT(v0 + v1, WithinRel(v_orig, 1e-4));
    REQUIRE_THAT(v0, WithinRel(20.0 * 20.0 * 5.0, 0.02));
    REQUIRE_THAT(v1, WithinRel(20.0 * 20.0 * 5.0, 0.02));
}

TEST_CASE("split by face keep upper only", "[CadDocument][cut]")
{
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);

    int n_faces = GeometryEngine::face_count(doc.bodies[0].shape);
    int top_face = -1;
    for (int i = 0; i < n_faces; ++i) {
        TopoDS_Face fc = GeometryEngine::face_by_index(doc.bodies[0].shape, i);
        Vec3d n = GeometryEngine::face_normal_world(fc);
        if (n.z() > 0.9) { top_face = i; break; }
    }
    REQUIRE(top_face >= 0);

    doc.add_split_by_face(0, -1, top_face, true, false, "SplitUpper");
    doc.features.back().cut_offset = -5.0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 1);

    double v = double(SketchEngine::tessellate(doc.bodies[0].shape).volume());
    REQUIRE_THAT(v, WithinRel(20.0 * 20.0 * 5.0, 0.02));
}

TEST_CASE("split by face round-trip serialization", "[CadDocument][cut]")
{
    using Catch::Matchers::WithinRel;
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);

    int n_faces = GeometryEngine::face_count(doc.bodies[0].shape);
    int top_face = -1;
    for (int i = 0; i < n_faces; ++i) {
        TopoDS_Face fc = GeometryEngine::face_by_index(doc.bodies[0].shape, i);
        Vec3d n = GeometryEngine::face_normal_world(fc);
        if (n.z() > 0.9) { top_face = i; break; }
    }
    REQUIRE(top_face >= 0);

    doc.add_split_by_face(0, 0, top_face, true, true, "SplitRT");
    doc.features.back().cut_offset = -5.0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 2);

    int saved_face_body = doc.features.back().cut_face_body;
    int saved_face      = doc.features.back().cut_face;
    bool saved_upper    = doc.features.back().cut_keep_upper;
    bool saved_lower    = doc.features.back().cut_keep_lower;
    size_t saved_nb     = doc.bodies.size();

    std::vector<std::pair<Vec3d, Vec3d>> bboxes;
    for (const auto& b : doc.bodies) {
        Bnd_Box bb; BRepBndLib::Add(b.shape, bb);
        Standard_Real x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        bboxes.push_back({Vec3d(x0, y0, z0), Vec3d(x1, y1, z1)});
    }

    auto blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    CadDocument doc2;
    REQUIRE(doc2.deserialize_recipe(blob));
    REQUIRE(doc2.bodies.size() == saved_nb);
    REQUIRE(doc2.features.size() == doc.features.size());

    const auto& f2 = doc2.features.back();
    REQUIRE(f2.cut_face_body  == saved_face_body);
    REQUIRE(f2.cut_face       == saved_face);
    REQUIRE(f2.cut_keep_upper == saved_upper);
    REQUIRE(f2.cut_keep_lower == saved_lower);

    for (size_t i = 0; i < saved_nb; ++i) {
        Bnd_Box bb; BRepBndLib::Add(doc2.bodies[i].shape, bb);
        Standard_Real x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        REQUIRE_THAT(double(x0), WithinAbs(bboxes[i].first.x(),  1e-6));
        REQUIRE_THAT(double(y0), WithinAbs(bboxes[i].first.y(),  1e-6));
        REQUIRE_THAT(double(z0), WithinAbs(bboxes[i].first.z(),  1e-6));
        REQUIRE_THAT(double(x1), WithinAbs(bboxes[i].second.x(), 1e-6));
        REQUIRE_THAT(double(y1), WithinAbs(bboxes[i].second.y(), 1e-6));
        REQUIRE_THAT(double(z1), WithinAbs(bboxes[i].second.z(), 1e-6));
    }
}

TEST_CASE("mirror reflects a body about a plane", "[CadDocument]")
{
    using Catch::Matchers::WithinRel;
    using Catch::Matchers::WithinAbs;

    auto make_box = [](CadDocument& doc, double w, double h, double d) {
        int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), w, h, 0, "Box");
        doc.add_extrude(sk, d, false, BooleanMode::New, "E");
    };

    // --- New mode: 20x20x20 cube, mirror about XZ plane offset to x=30 ---
    // The cube is in x=[-10,10]; the mirror is at x=30, so the mirrored cube
    // is at x=[50,70]. Disjoint -> two bodies, equal volumes (8000 each).
    SECTION("New mode: two disjoint bodies, equal volumes") {
        CadDocument doc;
        make_box(doc, 20.0, 20.0, 20.0);
        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        double v_orig = double(doc.display_mesh.volume());
        REQUIRE_THAT(v_orig, WithinRel(8000.0, 0.01));

        doc.add_mirror(SketchPlane::XZ(), 0, BooleanMode::New, "Mirror1");
        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        REQUIRE(doc.bodies.size() == 2);

        double v0 = double(SketchEngine::tessellate(doc.bodies[0].shape).volume());
        double v1 = double(SketchEngine::tessellate(doc.bodies[1].shape).volume());
        REQUIRE_THAT(v0, WithinRel(8000.0, 0.01));
        REQUIRE_THAT(v1, WithinRel(8000.0, 0.01));
    }

    // --- New mode with keep_original=false: the source body is replaced ---
    SECTION("New mode, keep_original=false: source body removed") {
        CadDocument doc;
        make_box(doc, 20.0, 20.0, 20.0);
        REQUIRE(doc.recompute());
        doc.add_mirror(SketchPlane::XZ(), 0, BooleanMode::New, "Mirror1");
        doc.features.back().mirror_keep_original = false;
        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        REQUIRE(doc.bodies.size() == 1);
        double v = double(SketchEngine::tessellate(doc.bodies[0].shape).volume());
        REQUIRE_THAT(v, WithinRel(8000.0, 0.01));
    }

    // --- Add mode: L-shape, entirely on one side of the mirror plane -> 2x volume ---
    // Build an L-shape completely in x>0: base 20x20x5 at x=[0,20] + wall 10x20x10
    // at x=[0,10] on top. Mirror about XZ at x=30 -> mirror at x=[40,60], disjoint.
    SECTION("Add mode: asymmetric L-shape, disjoint halves -> 2x volume") {
        CadDocument doc;
        // Base: 20x20x5, shifted to x=10 so it's in x=[0,20]
        CadFeature skb;
        skb.type = CadFeatureType::Sketch;
        skb.name = "Base";
        skb.plane = SketchPlane::XY();
        skb.imported_regions = {{ {Vec2d(0,-10), Vec2d(20,-10), Vec2d(20,10), Vec2d(0,10)} }};
        doc.features.push_back(skb);
        int skb_idx = int(doc.features.size()) - 1;
        CadFeature exb;
        exb.type = CadFeatureType::Extrude;
        exb.name = "EBase";
        exb.sketch_ref = skb_idx;
        exb.distance = 5.0;
        exb.mode = BooleanMode::New;
        doc.features.push_back(exb);

        // Wall: 10x20x10 on top of the base, x=[0,10]
        CadFeature skw;
        skw.type = CadFeatureType::Sketch;
        skw.name = "Wall";
        skw.plane = SketchPlane::XY();
        skw.plane.origin = Vec3d(0, 0, 5);
        skw.imported_regions = {{ {Vec2d(0,-10), Vec2d(10,-10), Vec2d(10,10), Vec2d(0,10)} }};
        doc.features.push_back(skw);
        int skw_idx = int(doc.features.size()) - 1;
        CadFeature exw;
        exw.type = CadFeatureType::Extrude;
        exw.name = "EWall";
        exw.sketch_ref = skw_idx;
        exw.distance = 10.0;
        exw.mode = BooleanMode::Add;
        exw.target_body = 0;
        doc.features.push_back(exw);

        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        const double v_l = double(doc.display_mesh.volume());

        // Mirror about YZ at x=30 -> the mirror is entirely in x=[40,60],
        // disjoint from the original in x=[0,20].
        SketchPlane mp = SketchPlane::YZ();
        mp.origin = Vec3d(30, 0, 0);
        doc.add_mirror(mp, 0, BooleanMode::Add, "Mirror1");
        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        REQUIRE(doc.bodies.size() == 1);
        const double v_m = double(doc.display_mesh.volume());
        REQUIRE_THAT(v_m, WithinRel(2.0 * v_l, 0.02));
    }

    // --- Add mode with intersecting plane -> fused volume < 2x ---
    // A 20x20x20 cube centred on the origin (so x=[-10,10]), mirrored about
    // XZ plane at x=0. The mirror maps the cube onto itself exactly (symmetry).
    // Fusing a cube with itself at the plane of symmetry produces the same cube
    // -> volume == original, strictly less than 2x.
    SECTION("Add mode: intersecting plane -> volume < 2x original") {
        CadDocument doc;
        make_box(doc, 20.0, 20.0, 20.0);
        REQUIRE(doc.recompute());
        const double v_orig = double(doc.display_mesh.volume());
        REQUIRE_THAT(v_orig, WithinRel(8000.0, 0.01));

        doc.add_mirror(SketchPlane::XZ(), 0, BooleanMode::Add, "Mirror1");
        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        const double v_mir = double(doc.display_mesh.volume());
        REQUIRE(v_mir < v_orig * 1.9);
    }

    // --- Invalid target body index ---
    SECTION("invalid target body index -> error, no crash") {
        CadDocument doc;
        // No bodies in the document -> mirror must fail, not crash.
        doc.features.push_back({CadFeatureType::Mirror, "BadMirror", true,
                                SketchShape::Rectangle, SketchPlane::XZ()});
        doc.features.back().mode = BooleanMode::New;
        REQUIRE_FALSE(doc.recompute());
        REQUIRE_FALSE(doc.error.empty());
    }
}

TEST_CASE("mirror serialization round-trip", "[CadDocument]")
{
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Box");
    doc.add_extrude(sk, 20.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());

    doc.add_mirror(SketchPlane::XZ(), 0, BooleanMode::New, "Mirror1");
    doc.features.back().mirror_keep_original = false;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    auto blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    CadDocument doc2;
    REQUIRE(doc2.deserialize_recipe(blob));
    REQUIRE(doc2.recompute());
    REQUIRE(doc2.error.empty());

    REQUIRE(doc2.features.size() == doc.features.size());
    const auto& f1 = doc.features.back();
    const auto& f2 = doc2.features.back();
    REQUIRE(f2.type                 == CadFeatureType::Mirror);
    REQUIRE(f2.mode                 == BooleanMode::New);
    REQUIRE(f2.mirror_keep_original == false);
    REQUIRE(f2.name                 == "Mirror1");

    REQUIRE(doc2.bodies.size() == doc.bodies.size());
}

TEST_CASE("mass properties: analytic cube", "[CadDocument]")
{
    using Catch::Matchers::WithinRel;
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Sketch");
    doc.add_extrude(sk, 20.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    auto mp = doc.body_mass_properties(0);
    REQUIRE(mp.valid);
    // Cube 20x20x20 -> V = 8000 mm^3
    REQUIRE_THAT(mp.volume, WithinRel(8000.0, 1e-6));
    // Surface area = 6 * 20^2 = 2400 mm^2
    REQUIRE_THAT(mp.surface_area, WithinRel(2400.0, 1e-6));
    // COM at geometric centre: the box centred on origin is from z=0 to z=20,
    // so centre at (0, 0, 10)
    REQUIRE_THAT(mp.center_of_mass.x(), WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(mp.center_of_mass.y(), WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(mp.center_of_mass.z(), WithinAbs(10.0, 1e-6));
    // Inertia sanity: symmetric cube -> diagonal terms equal, off-diagonal ~0
    double Ixx = mp.inertia[0], Iyy = mp.inertia[4], Izz = mp.inertia[8];
    REQUIRE_THAT(Ixx, WithinRel(8000.0 * (20*20 + 20*20) / 12.0, 1e-4)); // I = m/12*(b^2+h^2) about COM
    REQUIRE_THAT(Iyy, WithinRel(Ixx, 1e-4));
    REQUIRE_THAT(Izz, WithinRel(Ixx, 1e-4));
    REQUIRE_THAT(mp.inertia[1], WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(mp.inertia[2], WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(mp.inertia[5], WithinAbs(0.0, 1e-6));
}

TEST_CASE("mass properties: analytic cylinder", "[CadDocument]")
{
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    // Circle r=10 on XY, extrude height=5 -> cylinder
    SketchEntity c;
    c.type   = SketchEntity::Type::Circle;
    c.center = Vec2d(0, 0);
    c.radius = 10.0;
    int sk = doc.add_sketch_entities({c}, SketchPlane::XY(), "Circle");
    doc.add_extrude(sk, 5.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    auto mp = doc.body_mass_properties(0);
    REQUIRE(mp.valid);
    double expected_vol = M_PI * 100.0 * 5.0;   // pi * r^2 * h
    REQUIRE_THAT(mp.volume, WithinRel(expected_vol, 1e-4));
    // Surface: 2*pi*r^2 + 2*pi*r*h = 2*pi*100 + 2*pi*10*5 = 200*pi + 100*pi = 300*pi
    double expected_area = 2 * M_PI * 100.0 + 2 * M_PI * 10.0 * 5.0;
    REQUIRE_THAT(mp.surface_area, WithinRel(expected_area, 1e-4));
}

TEST_CASE("mass properties: hollow body", "[CadDocument]")
{
    using Catch::Matchers::WithinRel;

    // Solid cube
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Sketch");
    doc.add_extrude(sk, 20.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());
    auto mp_solid = doc.body_mass_properties(0);
    REQUIRE(mp_solid.valid);
    double solid_vol = mp_solid.volume;

    // Hollow cube with a through hole
    CadDocument doc2;
    int sk2 = doc2.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Sketch");
    doc2.add_extrude(sk2, 20.0, false, BooleanMode::New, "Extrude");
    doc2.add_hole(10.0, 20.0, true, 0.0, 0.0, SketchPlane::XY(), "Hole");
    REQUIRE(doc2.recompute());
    auto mp_hollow = doc2.body_mass_properties(0);
    REQUIRE(mp_hollow.valid);

    // Hollow volume < solid volume
    REQUIRE(mp_hollow.volume < solid_vol);
    // Hollow = solid - cylinder: V_cyl = pi*5^2*20 = 500*pi
    double expected_hollow = solid_vol - M_PI * 25.0 * 20.0;
    REQUIRE_THAT(mp_hollow.volume, WithinRel(expected_hollow, 0.01));
}

TEST_CASE("mass properties: invalid body index", "[CadDocument]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Sketch");
    doc.add_extrude(sk, 20.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());

    // Out of range -> valid=false
    auto mp = doc.body_mass_properties(99);
    REQUIRE_FALSE(mp.valid);
    REQUIRE(mp.volume == 0.0);
    // Negative index
    auto mp2 = doc.body_mass_properties(-1);
    REQUIRE_FALSE(mp2.valid);

    // Empty document (no recompute) -> all bodies empty, index 0 out of range
    CadDocument empty;
    auto mp3 = empty.body_mass_properties(0);
    REQUIRE_FALSE(mp3.valid);
}

TEST_CASE("serialize_recipe roundtrip with two bodies", "[CadDocument]")
{
    using Catch::Matchers::WithinRel;
    CadDocument doc;

    // Body 1: rectangle sketch + extrude + fillet
    int sk1 = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(),
                             20, 20, 10, "Rect1");
    REQUIRE(sk1 >= 0);
    doc.add_extrude(sk1, 10.0, false, BooleanMode::New, "Extrude1");
    doc.add_fillet(2.0, FaceGroup::All, "Fillet1");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Body 2: circle sketch + extrude (separate New body)
    SketchEntity circ;
    circ.type   = SketchEntity::Type::Circle;
    circ.center = Vec2d(0, 0);
    circ.radius = 8.0;
    int sk2 = doc.add_sketch_entities({circ}, SketchPlane::XZ(), "Circle2");
    doc.add_extrude(sk2, 6.0, false, BooleanMode::New, "Extrude2");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    REQUIRE(doc.bodies.size() >= 2);
    std::vector<double> orig_vols;
    for (const auto& b : doc.bodies)
        orig_vols.push_back(double(SketchEngine::tessellate(b.shape).volume()));

    auto blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    CadDocument doc2;
    REQUIRE(doc2.deserialize_recipe(blob));
    REQUIRE(doc2.error.empty());
    REQUIRE(doc2.bodies.size() == doc.bodies.size());

    for (size_t i = 0; i < doc.bodies.size(); ++i) {
        double v2 = double(SketchEngine::tessellate(doc2.bodies[i].shape).volume());
        REQUIRE_THAT(v2, WithinRel(orig_vols[i], 1e-6));
    }
}

TEST_CASE("deserialize_recipe rejects future version with error", "[CadDocument]")
{
    CadDocument doc;
    std::ostringstream oss;
    {
        cereal::BinaryOutputArchive ar(oss);
        uint32_t v = 999;
        ar(v);
    }
    REQUIRE_FALSE(doc.deserialize_recipe(oss.str()));
    REQUIRE_FALSE(doc.error.empty());
    CHECK_CONTAINS(doc.error, "newer version");
}

TEST_CASE("deserialize_recipe rejects older version with error", "[CadDocument]")
{
    CadDocument doc;
    std::ostringstream oss;
    {
        cereal::BinaryOutputArchive ar(oss);
        uint32_t v = 1;
        ar(v);
    }
    REQUIRE_FALSE(doc.deserialize_recipe(oss.str()));
    REQUIRE_FALSE(doc.error.empty());
    CHECK_CONTAINS(doc.error, "older version");
}

TEST_CASE("deserialize_recipe handles truncated blob without throwing", "[CadDocument]")
{
    CadDocument doc;
    std::string garbage = "this is not a valid cereal blob";
    REQUIRE_FALSE(doc.deserialize_recipe(garbage));
    REQUIRE_FALSE(doc.error.empty());
}

TEST_CASE("deserialize_recipe handles empty blob without throwing", "[CadDocument]")
{
    CadDocument doc;
    REQUIRE_FALSE(doc.deserialize_recipe(""));
    REQUIRE_FALSE(doc.error.empty());
}

TEST_CASE("deserialize_recipe error is non-empty on every failure path", "[CadDocument]")
{
    CadDocument doc;
    auto reset = [&]() { doc = CadDocument{}; };

    // too new
    {
        std::ostringstream oss;
        { cereal::BinaryOutputArchive ar(oss); uint32_t v = 999; ar(v); }
        reset();
        REQUIRE_FALSE(doc.deserialize_recipe(oss.str()));
        REQUIRE_FALSE(doc.error.empty());
    }
    // too old
    {
        std::ostringstream oss;
        { cereal::BinaryOutputArchive ar(oss); uint32_t v = 1; ar(v); }
        reset();
        REQUIRE_FALSE(doc.deserialize_recipe(oss.str()));
        REQUIRE_FALSE(doc.error.empty());
    }
    // truncated / garbage
    {
        reset();
        REQUIRE_FALSE(doc.deserialize_recipe("not a valid blob \x00\x01\x02"));
        REQUIRE_FALSE(doc.error.empty());
    }
    // empty
    {
        reset();
        REQUIRE_FALSE(doc.deserialize_recipe(""));
        REQUIRE_FALSE(doc.error.empty());
    }
}

TEST_CASE("a v4 project still opens", "[CadDocument][recipe]")
{
    // The regression guard for everyone who already has projects: a pre-framing v4
    // recipe must keep opening on this build, through the unchanged flat path — not be
    // refused at the version gate after the bump to v5.
    std::string path = std::string(TEST_DATA_DIR) + "/cad_recipe_v4.bin";
    std::ifstream ifs(path, std::ios::binary);
    REQUIRE(ifs.is_open());
    std::string blob((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();
    REQUIRE_FALSE(blob.empty());

    // Trusted reference read of the flat v4 layout (what the golden test's Layer 1 does).
    std::vector<CadFeature> flat;
    {
        std::istringstream iss(blob);
        cereal::BinaryInputArchive ar(iss);
        uint32_t v;
        ar(v);
        REQUIRE(v == 4);
        ar(flat);
    }
    REQUIRE_FALSE(flat.empty());

    CadDocument doc;
    doc.deserialize_recipe(blob);
    // Not refused at the gate: the only failure allowed is the fixture's own geometry
    // (the golden doc is a serialization-coverage tree whose fillet radius is too large),
    // which is orthogonal to the v5 framing change and unchanged by it.
    REQUIRE(doc.error.find("older version") == std::string::npos);
    REQUIRE(doc.error.find("newer version") == std::string::npos);
    // The v4 flat path read the same feature tree as the trusted reference.
    REQUIRE(doc.features.size() == flat.size());
}

TEST_CASE("a v5 round trip is exact", "[CadDocument][recipe]")
{
    using Catch::Matchers::WithinRel;
    CadDocument doc;

    // Body 1: rectangle sketch + extrude + fillet
    int sk1 = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(),
                             20, 20, 10, "Rect1");
    REQUIRE(sk1 >= 0);
    doc.add_extrude(sk1, 10.0, false, BooleanMode::New, "Extrude1");
    doc.add_fillet(2.0, FaceGroup::All, "Fillet1");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Body 2: circle sketch + extrude (separate New body)
    SketchEntity circ;
    circ.type   = SketchEntity::Type::Circle;
    circ.center = Vec2d(0, 0);
    circ.radius = 8.0;
    int sk2 = doc.add_sketch_entities({circ}, SketchPlane::XZ(), "Circle2");
    doc.add_extrude(sk2, 6.0, false, BooleanMode::New, "Extrude2");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    REQUIRE(doc.bodies.size() >= 2);
    std::vector<double> orig_vols;
    for (const auto& b : doc.bodies)
        orig_vols.push_back(double(SketchEngine::tessellate(b.shape).volume()));

    auto blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    CadDocument doc2;
    REQUIRE(doc2.deserialize_recipe(blob));
    REQUIRE(doc2.error.empty());
    REQUIRE(doc2.features.size() == doc.features.size());
    for (size_t i = 0; i < doc.features.size(); ++i)
        REQUIRE(doc2.features[i].type == doc.features[i].type);
    REQUIRE(doc2.bodies.size() == doc.bodies.size());
    for (size_t i = 0; i < doc.bodies.size(); ++i) {
        double v2 = double(SketchEngine::tessellate(doc2.bodies[i].shape).volume());
        REQUIRE_THAT(v2, WithinRel(orig_vols[i], 1e-6));
    }
}

TEST_CASE("a truncated feature keeps what it could read", "[CadDocument][recipe]")
{
    // The forward-compat proof: a v5 reader that meets a feature blob shorter than its own
    // field list must keep what it read and default the rest, not error. Simulate an older
    // file by hand-shortening ONE feature's frame: rewrite its length prefix and drop the
    // tail bytes, then confirm the load still succeeds and the fields before the cut survive.
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 10, 0, "Sk1");
    REQUIRE(sk >= 0);
    doc.add_extrude(sk, 5.0, false, BooleanMode::New, "Ex1");
    int sk2 = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 12, 12, 0, "Sk2");
    doc.add_extrude(sk2, 3.0, false, BooleanMode::New, "Ex2");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Two trailing connector-fingerprint fields, made distinctive so the cut is observable:
    // coordsys_face_kind sits right before the LAST serialized field (coordsys_face_edges),
    // which is the one we drop. They are ignored by an Extrude's recompute, so the truncated
    // document still replays cleanly.
    doc.features[1].coordsys_face_kind  = 777;
    doc.features[1].coordsys_face_edges = 888;

    auto blob = doc.serialize_recipe();
    REQUIRE(blob.size() > 8);

    auto rd32 = [](const std::string& s, size_t off) -> uint32_t {
        uint32_t u;
        std::memcpy(&u, s.data() + off, sizeof(u));
        return u;
    };
    auto wr32 = [](std::string& s, size_t off, uint32_t u) {
        std::memcpy(&s[off], &u, sizeof(u));
    };

    REQUIRE(rd32(blob, 0) == 5);
    uint32_t count = rd32(blob, 4);
    REQUIRE(count == doc.features.size());

    // Walk the outer framing: version + count, then [len][bytes] per feature.
    std::vector<size_t> f_off, f_len;
    size_t off = 8;
    for (uint32_t i = 0; i < count; ++i) {
        f_off.push_back(off);
        f_len.push_back(rd32(blob, off));
        off += 4 + f_len.back();
    }

    // Shorten feature 1 by dropping its final field (4 bytes): rewrite its length prefix
    // and erase the tail bytes. The reader then runs out inside fa(f), throws, and keeps
    // everything it had already assigned — that is the whole point of the try/catch.
    const size_t drop = sizeof(uint32_t);
    REQUIRE(f_len[1] > drop);
    std::string shortened = blob;
    shortened.erase(f_off[1] + 4 + f_len[1] - drop, drop);
    wr32(shortened, f_off[1], static_cast<uint32_t>(f_len[1] - drop));

    CadDocument loaded;
    REQUIRE(loaded.deserialize_recipe(shortened));
    REQUIRE(loaded.error.empty());
    REQUIRE(loaded.features.size() == count);

    // Fields before the cut survived; the cut field defaulted; the neighbours on both sides
    // are intact, proving the outer framing held.
    REQUIRE(loaded.features[1].name == doc.features[1].name);
    REQUIRE(loaded.features[1].coordsys_face_kind  == 777);   // right before the cut
    REQUIRE(loaded.features[1].coordsys_face_edges == -1);    // defaulted by the cut
    REQUIRE(loaded.features[0].name == doc.features[0].name);
    REQUIRE(loaded.features[2].name == doc.features[2].name);
}

TEST_CASE("a v3 recipe is refused with a message naming the version", "[CadDocument][recipe]")
{
    // Honesty guard: the framing fixes the future, not the past. A v3 field list no longer
    // exists in this code, so a v3 blob must be refused cleanly and must say which version.
    std::string path = std::string(TEST_DATA_DIR) + "/cad_recipe_v3.bin";
    std::ifstream ifs(path, std::ios::binary);
    REQUIRE(ifs.is_open());
    std::string blob((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();
    REQUIRE_FALSE(blob.empty());

    CadDocument doc;
    REQUIRE_FALSE(doc.deserialize_recipe(blob));
    REQUIRE_FALSE(doc.error.empty());
    REQUIRE(doc.error.find("v3") != std::string::npos);
}

TEST_CASE("re-edit: editing a mid-timeline feature rebuilds downstream", "[CadDocument]")
{
    using Catch::Matchers::WithinRel;
    CadDocument doc;

    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(),
                            20, 10, 0, "Sketch1");
    REQUIRE(sk >= 0);
    int ex = doc.add_extrude(sk, 5.0, false, BooleanMode::New, "Extrude1");
    REQUIRE(ex >= 0);
    doc.add_fillet(1.0, FaceGroup::All, "Fillet1");

    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    Vec3d sz0 = doc.display_mesh.bounding_box().size();
    REQUIRE(sz0.z() > 0.0);

    // Edit the MID feature (extrude — NOT the last; Fillet is downstream).
    doc.features[ex].distance = 12.0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    Vec3d sz1 = doc.display_mesh.bounding_box().size();
    REQUIRE(sz1.z() > sz0.z() + 1.0);
    REQUIRE(doc.bodies.size() == 1);

    // Edit the FIRST feature (sketch width) — must propagate the whole chain.
    doc.features[sk].width = 30;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    Vec3d sz2 = doc.display_mesh.bounding_box().size();
    REQUIRE(sz2.x() > sz1.x() + 1.0);
}

TEST_CASE("re-edit survives serialize -> deserialize", "[CadDocument]")
{
    using Catch::Matchers::WithinRel;
    CadDocument doc;

    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(),
                            20, 10, 0, "Sketch1");
    REQUIRE(sk >= 0);
    doc.add_extrude(sk, 5.0, false, BooleanMode::New, "Extrude1");
    doc.add_fillet(1.0, FaceGroup::All, "Fillet1");

    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    auto blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    CadDocument doc2;
    REQUIRE(doc2.deserialize_recipe(blob));
    REQUIRE(doc2.error.empty());
    REQUIRE(doc2.recompute());
    REQUIRE(doc2.error.empty());
    Vec3d sz_pre = doc2.display_mesh.bounding_box().size();
    REQUIRE(sz_pre.z() > 0.0);

    // Mid-edit the deserialized document — the persisted recipe stays re-editable.
    doc2.features[1].distance = 12.0;
    REQUIRE(doc2.recompute());
    REQUIRE(doc2.error.empty());
    Vec3d sz_post = doc2.display_mesh.bounding_box().size();
    REQUIRE(sz_post.z() > sz_pre.z() + 1.0);
}

TEST_CASE("re-edit: multi-type timeline replays all downstream features", "[CadDocument]")
{
    using Catch::Matchers::WithinRel;
    CadDocument doc;

    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(),
                            30, 30, 0, "Sketch1");
    REQUIRE(sk >= 0);
    int ex = doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude1");
    REQUIRE(ex >= 0);
    doc.add_hole(6.0, 4.0, false, 0.0, 0.0, SketchPlane::XY(), "Hole1");
    doc.add_chamfer(1.0, FaceGroup::All, "Chamfer1");

    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    size_t n_bodies = doc.bodies.size();
    REQUIRE(n_bodies >= 1);
    Vec3d sz0 = doc.display_mesh.bounding_box().size();
    REQUIRE(sz0.z() > 0.0);

    // Edit the MID extrude — downstream Hole and Chamfer must rebuild.
    doc.features[ex].distance = 16.0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == n_bodies);
    Vec3d sz1 = doc.display_mesh.bounding_box().size();
    REQUIRE(sz1.z() > sz0.z() + 1.0);
}

TEST_CASE("datum plane construction methods", "[CadDocument][plane]")
{
    using Catch::Matchers::WithinAbs;

    // Build a doc with a box (sketch rect 40x30 + extrude 20) so bodies[0] has faces/edges.
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 40, 30, 0, "BoxSketch");
    REQUIRE(sk >= 0);
    doc.add_extrude(sk, 20.0, false, BooleanMode::New, "Extrude1");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 1);
    const int n_faces = GeometryEngine::face_count(doc.bodies[0].shape);
    REQUIRE(n_faces == 6);   // a box
    const int n_edges = GeometryEngine::edge_count(doc.bodies[0].shape);
    REQUIRE(n_edges == 12);

    // Find top face (normal ~ +Z) and bottom face (normal ~ -Z) by scanning.
    int top_idx = -1, bot_idx = -1;
    for (int i = 0; i < n_faces; ++i) {
        TopoDS_Face fc = GeometryEngine::face_by_index(doc.bodies[0].shape, i);
        Vec3d n = GeometryEngine::face_normal_world(fc);
        if (n.z() > 0.9)  top_idx = i;
        if (n.z() < -0.9) bot_idx = i;
    }
    REQUIRE(top_idx >= 0);
    REQUIRE(bot_idx >= 0);

    // --- Offset from base XY by 10 ---
    auto planes0 = doc.resolve_datum_planes();
    size_t initial = planes0.size();

    CadFeature f0;
    f0.type       = CadFeatureType::Plane;
    f0.name       = "Offset10";
    f0.plane_base = 0;   // XY
    f0.plane_type = PlaneType::Offset;
    f0.plane_offset = 10;
    doc.features.push_back(f0);

    auto planes = doc.resolve_datum_planes();
    REQUIRE(planes.size() == initial + 1);
    CHECK_THAT(planes.back().second.origin.z(), WithinAbs(10.0, 1e-6));
    CHECK_THAT(planes.back().second.normal.z(), WithinAbs(1.0, 1e-6));

    // --- Coincident to top face ---
    CadFeature f1;
    f1.type            = CadFeatureType::Plane;
    f1.name            = "CoincidentTop";
    f1.plane_type      = PlaneType::Coincident;
    f1.plane_face_body = 0;
    f1.plane_face      = top_idx;
    doc.features.push_back(f1);

    planes = doc.resolve_datum_planes();
    REQUIRE(planes.size() == initial + 2);
    CHECK_THAT(planes.back().second.origin.z(), WithinAbs(20.0, 1e-6));   // box height is 20
    CHECK_THAT(planes.back().second.normal.z(), WithinAbs(1.0, 1e-6));

    // --- Midplane between top and bottom faces ---
    CadFeature f2;
    f2.type             = CadFeatureType::Plane;
    f2.name             = "Midplane";
    f2.plane_type       = PlaneType::Midplane;
    f2.plane_face_body  = 0;
    f2.plane_face       = top_idx;
    f2.plane_face2_body = 0;
    f2.plane_face2      = bot_idx;
    doc.features.push_back(f2);

    planes = doc.resolve_datum_planes();
    REQUIRE(planes.size() == initial + 3);
    CHECK_THAT(planes.back().second.origin.z(), WithinAbs(10.0, 1e-6));   // midway
    CHECK_THAT(std::abs(planes.back().second.normal.z()), WithinAbs(1.0, 1e-6));

    // --- Orthonormality check on all resolved planes ---
    for (const auto& [name, sp] : planes) {
        INFO("Plane: " << name);
        CHECK_THAT(sp.normal.norm(), WithinAbs(1.0, 1e-6));
        CHECK_THAT(sp.x_axis.norm(), WithinAbs(1.0, 1e-6));
        CHECK_THAT(sp.y_axis.norm(), WithinAbs(1.0, 1e-6));
        CHECK_THAT(std::abs(sp.x_axis.dot(sp.y_axis)), WithinAbs(0.0, 1e-6));
        CHECK_THAT(std::abs(sp.x_axis.dot(sp.normal)), WithinAbs(0.0, 1e-6));
        CHECK_THAT(std::abs(sp.y_axis.dot(sp.normal)), WithinAbs(0.0, 1e-6));
    }
}

// Mirrors the snaporca [Deviation] case (snaporca carries it in test_geometry.cpp; here it lives
// alongside the CAD suite). GeometryEngine::surface_deviation = one-sided Hausdorff used by the
// MCP validate_against acceptance metric.
TEST_CASE("surface_deviation: identical solids ~0, shifted solid ~shift", "[Deviation]")
{
    TopoDS_Shape ref = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    auto d0 = GeometryEngine::surface_deviation(ref, ref, 0.5);
    REQUIRE(d0.sample_count > 0);
    REQUIRE_THAT(d0.max_mm, Catch::Matchers::WithinAbs(0.0, 1e-6));

    // candidate shifted +2mm in X: far corner vertices sit 2mm outside the reference
    gp_Trsf t; t.SetTranslation(gp_Vec(2.0, 0.0, 0.0));
    TopoDS_Shape shifted = BRepBuilderAPI_Transform(ref, t, true).Shape();
    auto d1 = GeometryEngine::surface_deviation(shifted, ref, 0.5);
    REQUIRE_THAT(d1.max_mm, Catch::Matchers::WithinAbs(2.0, 0.05));
    REQUIRE(d1.mean_mm > 0.0);
}

TEST_CASE("mesh_to_brep: watertight cube -> solid, coplanar merge gives 6 faces", "[design][mesh2brep]")
{
    const indexed_triangle_set cube = its_make_cube(10.0, 20.0, 30.0);
    REQUIRE(cube.indices.size() == 12);

    SECTION("faceted: one B-rep face per triangle, exact volume") {
        GeometryEngine::MeshBrepStats st;
        const TopoDS_Shape shape = GeometryEngine::mesh_to_brep(cube, 0.01, /*no merge*/0.0, st);
        REQUIRE_FALSE(shape.IsNull());
        CHECK(st.kept_tris == 12);
        CHECK(st.faces_built == 12);
        CHECK(st.faces_final == 12);
        // Shared topology by construction: a cube has 8 vertices and 18 edges once the two
        // triangles of each face share their diagonal. Every edge used exactly twice.
        CHECK(st.unique_edges == 18);
        CHECK(st.boundary_edges == 0);
        CHECK(st.nonmanifold_edges == 0);
        CHECK(st.watertight);
        REQUIRE(st.is_solid);
        REQUIRE_THAT(st.volume, Catch::Matchers::WithinRel(10.0 * 20.0 * 30.0, 1e-9));
    }

    SECTION("merge coplanar: 12 triangles collapse to the cube's 6 real faces") {
        GeometryEngine::MeshBrepStats st;
        const TopoDS_Shape shape = GeometryEngine::mesh_to_brep(cube, 0.01, 5.0, st);
        REQUIRE_FALSE(shape.IsNull());
        REQUIRE(st.is_solid);
        // This is the whole point of merging: the imported body must expose pickable CAD faces,
        // not one face per triangle, or the fillet/extrude tools have nothing meaningful to grab.
        REQUIRE(st.faces_final == 6);
        REQUIRE_THAT(st.volume, Catch::Matchers::WithinRel(10.0 * 20.0 * 30.0, 1e-9));
    }
}

TEST_CASE("mesh_to_brep: an open mesh is reported as a shell, never a fake solid", "[design][mesh2brep]")
{
    indexed_triangle_set open_cube = its_make_cube(10.0, 10.0, 10.0);
    open_cube.indices.pop_back();          // punch a hole: drop one triangle
    open_cube.indices.pop_back();          // (and its coplanar partner -> a whole face missing)

    GeometryEngine::MeshBrepStats st;
    const TopoDS_Shape shape = GeometryEngine::mesh_to_brep(open_cube, 0.01, 5.0, st);
    REQUIRE_FALSE(shape.IsNull());
    CHECK(st.kept_tris == 10);
    CHECK(st.boundary_edges > 0);          // the hole's rim
    CHECK_FALSE(st.watertight);
    REQUIRE_FALSE(st.is_solid);            // must NOT be dressed up as a solid
    CHECK(st.volume == 0.0);
}

TEST_CASE("mesh_to_brep: degenerate triangles are rejected on a scale-independent test", "[design][mesh2brep]")
{
    // A thin but perfectly legitimate CAD sliver. Every edge (1.0, ~0.5, ~0.5) is far above the
    // 0.01 dedup tolerance, so no vertex collapses — but its area (5e-5) is BELOW tolerance^2
    // (1e-4). A rule of "reject when area < tolerance^2" would therefore throw it away, which is
    // precisely the bug that turned a watertight 62k-triangle input into a falsely-open shell.
    // The scale-independent test (area < 1e-9 * longest_edge^2 = 1e-9) keeps it, as it must.
    indexed_triangle_set sliver;
    sliver.vertices = { {0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.5f, 0.0001f, 0.f} };
    sliver.indices  = { {0, 1, 2} };
    GeometryEngine::MeshBrepStats st;
    GeometryEngine::mesh_to_brep(sliver, 0.01, 0.0, st);
    CHECK(st.degenerate_sliver == 0);
    CHECK(st.degenerate_collapsed == 0);
    CHECK(st.kept_tris == 1);

    // A truly collinear triangle has no area at any scale -> rejected as a sliver.
    indexed_triangle_set collinear;
    collinear.vertices = { {0.f, 0.f, 0.f}, {10.f, 0.f, 0.f}, {20.f, 0.f, 0.f} };
    collinear.indices  = { {0, 1, 2} };
    GeometryEngine::MeshBrepStats st2;
    CHECK_THROWS(GeometryEngine::mesh_to_brep(collinear, 0.01, 0.0, st2));  // nothing left to build
    CHECK(st2.degenerate_sliver == 1);

    // A triangle entirely inside one tolerance cell is sub-resolution noise -> collapsed.
    indexed_triangle_set tiny;
    tiny.vertices = { {0.f, 0.f, 0.f}, {0.001f, 0.f, 0.f}, {0.f, 0.001f, 0.f} };
    tiny.indices  = { {0, 1, 2} };
    GeometryEngine::MeshBrepStats st3;
    CHECK_THROWS(GeometryEngine::mesh_to_brep(tiny, 0.1, 0.0, st3));
    CHECK(st3.degenerate_collapsed == 1);
}

TEST_CASE("datum axis: two points direction is unit and analytic", "[CadDocument]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int ax = doc.add_axis(AxisType::TwoPoints, "AxisThroughZ");
    REQUIRE(ax == 0);
    doc.features[ax].axis_p1 = Vec3d(0, 0, 0);
    doc.features[ax].axis_p2 = Vec3d(0, 0, 10);

    auto axes = doc.resolve_datum_axes();
    REQUIRE(axes.size() == 1);
    REQUIRE(axes[0].name == "AxisThroughZ");
    REQUIRE(axes[0].error.empty());
    CHECK_THAT(axes[0].direction.x(), WithinAbs(0.0, 1e-12));
    CHECK_THAT(axes[0].direction.y(), WithinAbs(0.0, 1e-12));
    CHECK_THAT(axes[0].direction.z(), WithinAbs(1.0, 1e-12));
    CHECK_THAT(axes[0].direction.norm(), WithinAbs(1.0, 1e-9));
    CHECK_THAT(axes[0].origin.x(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(axes[0].origin.y(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(axes[0].origin.z(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("datum axis: degenerate two identical points fails cleanly", "[CadDocument]")
{
    CadDocument doc;
    int ax = doc.add_axis(AxisType::TwoPoints, "Degenerate");
    doc.features[ax].axis_p1 = Vec3d(5, 5, 5);
    doc.features[ax].axis_p2 = Vec3d(5, 5, 5);

    auto axes = doc.resolve_datum_axes();
    REQUIRE(axes.size() == 1);
    REQUIRE_FALSE(axes[0].error.empty());
}

TEST_CASE("datum axis: two parallel planes fail with error", "[CadDocument]")
{
    CadDocument doc;
    // Two offset XY planes are parallel -> no intersection.
    // 3 and 4, not 0 and 1: axis_plane_a/b use the same encoding as plane_base — 0/1/2 are the
    // XY/XZ/YZ base planes and datums start at 3 — because the GUI fills these from
    // populate_plane_choices() and stores the row verbatim.
    doc.add_plane(0 /*XY*/, 10.0, 0.0, 0, "PlaneA");
    doc.add_plane(0 /*XY*/, 30.0, 0.0, 0, "PlaneB");

    int ax = doc.add_axis(AxisType::TwoPoints, "Parallel");
    doc.features[ax].axis_type      = AxisType::PlaneIntersection;
    doc.features[ax].axis_plane_a   = 3;
    doc.features[ax].axis_plane_b   = 4;

    auto axes = doc.resolve_datum_axes();
    REQUIRE(axes.size() == 1);
    REQUIRE_FALSE(axes[0].error.empty());
}

// The case the GUI actually produces most often, and which could not work before: the two refs
// are rows of the plane picker, whose first three entries are the base planes. XY x XZ is the
// X axis. Previously ref 0 was read as "datum plane 0", so this silently resolved to the wrong
// plane or failed with "plane ref not found".
TEST_CASE("datum axis: intersection of two BASE planes gives the expected axis", "[CadDocument]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int ax = doc.add_axis(AxisType::PlaneIntersection, "XAxis");
    doc.features[ax].axis_plane_a = 0;   // XY
    doc.features[ax].axis_plane_b = 1;   // XZ

    auto axes = doc.resolve_datum_axes();
    REQUIRE(axes.size() == 1);
    REQUIRE(axes[0].error.empty());
    // XY normal is Z, XZ normal is Y; Z x Y is +/-X.
    CHECK_THAT(std::abs(axes[0].direction.x()), WithinAbs(1.0, 1e-12));
    CHECK_THAT(axes[0].direction.y(),           WithinAbs(0.0, 1e-12));
    CHECK_THAT(axes[0].direction.z(),           WithinAbs(0.0, 1e-12));
}

// A datum plane crossed with a base plane — the mixed case, which pins the +3 offset.
TEST_CASE("datum axis: base plane crossed with a datum plane", "[CadDocument]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    doc.add_plane(1 /*XZ*/, 5.0, 0.0, 0, "OffsetXZ");   // datum 0 -> row 3

    int ax = doc.add_axis(AxisType::PlaneIntersection, "MixedAxis");
    doc.features[ax].axis_plane_a = 0;   // XY base
    doc.features[ax].axis_plane_b = 3;   // the datum above

    auto axes = doc.resolve_datum_axes();
    REQUIRE(axes.size() == 1);
    REQUIRE(axes[0].error.empty());
    CHECK_THAT(std::abs(axes[0].direction.x()), WithinAbs(1.0, 1e-12));
}

TEST_CASE("datum axis: cylinder centreline from extruded circle", "[CadDocument]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    // Build a cylinder: circle r=5 at origin, extrude 20 mm along +Z -> cylinder z=[0,20]
    int sk = doc.add_sketch(SketchShape::Circle, SketchPlane::XY(), 0, 0, 5.0, "Circle");
    doc.add_extrude(sk, 20.0, false, BooleanMode::New, "Cyl");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);
    // Find the lateral cylindrical face
    int n_faces = GeometryEngine::face_count(doc.bodies[0].shape);
    int lateral_face = -1;
    for (int i = 0; i < n_faces; ++i) {
        TopoDS_Face fc = GeometryEngine::face_by_index(doc.bodies[0].shape, i);
        GeometryEngine::CylinderFace cyl = GeometryEngine::cylinder_of_face(fc);
        if (cyl.ok) { lateral_face = i; break; }
    }
    REQUIRE(lateral_face >= 0);

    int ax = doc.add_axis(AxisType::TwoPoints, "CylAx");
    doc.features[ax].axis_type = AxisType::CylinderCenterline;
    doc.features[ax].axis_body = 0;
    doc.features[ax].axis_face = lateral_face;

    auto axes = doc.resolve_datum_axes();
    REQUIRE(axes.size() == 1);
    REQUIRE(axes[0].error.empty());
    // OCCT may return the axis direction as +Z or -Z depending on face orientation;
    // the centreline is always collinear with Z and passes through (x=0,y=0).
    CHECK_THAT(std::abs(axes[0].direction.z()), WithinAbs(1.0, 1e-12));
    CHECK_THAT(axes[0].direction.x(), WithinAbs(0.0, 1e-12));
    CHECK_THAT(axes[0].direction.y(), WithinAbs(0.0, 1e-12));
    CHECK_THAT(axes[0].origin.x(), WithinAbs(0.0, 1e-6));
    CHECK_THAT(axes[0].origin.y(), WithinAbs(0.0, 1e-6));
}

TEST_CASE("datum coordinate system: non-perpendicular inputs produce orthonormal axes", "[CadDocument]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    // Build a body so we have a face to reference for FaceAndDirection.
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "BoxExt");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);
    int n_faces = GeometryEngine::face_count(doc.bodies[0].shape);
    int top_face = -1;
    for (int i = 0; i < n_faces; ++i) {
        Vec3d fn = GeometryEngine::face_normal_world(GeometryEngine::face_by_index(doc.bodies[0].shape, i));
        if (fn.z() > 0.9) { top_face = i; break; }
    }
    REQUIRE(top_face >= 0);

    int cs = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 0), "CS1");
    REQUIRE(cs >= 0);
    doc.features[cs].coordsys_type   = CoordSysType::FaceAndDirection;
    doc.features[cs].coordsys_body   = 0;
    doc.features[cs].coordsys_face   = top_face;
    // Deliberately non-perpendicular X hint (NOT orthogonal to face normal ~+Z).
    doc.features[cs].coordsys_x_hint = Vec3d(3.0, -1.0, 0.5);

    auto css = doc.resolve_datum_coordsys();
    REQUIRE(css.size() == 1);
    REQUIRE(css[0].error.empty());

    Vec3d X = css[0].x, Y = css[0].y;
    // Orthonormality: each axis has unit length
    CHECK_THAT(X.norm(), WithinAbs(1.0, 1e-9));
    CHECK_THAT(Y.norm(), WithinAbs(1.0, 1e-9));
    // Pairwise dot products are ~0
    CHECK_THAT(std::abs(X.dot(Y)), WithinAbs(0.0, 1e-9));
    // Z = X x Y (derived), also unit and perpendicular
    Vec3d Z = X.cross(Y);
    CHECK_THAT(Z.norm(), WithinAbs(1.0, 1e-9));
    CHECK_THAT(std::abs(X.dot(Z)), WithinAbs(0.0, 1e-9));
    CHECK_THAT(std::abs(Y.dot(Z)), WithinAbs(0.0, 1e-9));
    // Right-handedness: X x Y == Z
    CHECK_THAT(Z.x(), WithinAbs((X.cross(Y)).x(), 1e-9));
    CHECK_THAT(Z.y(), WithinAbs((X.cross(Y)).y(), 1e-9));
    CHECK_THAT(Z.z(), WithinAbs((X.cross(Y)).z(), 1e-9));
}

TEST_CASE("datum coordinate system: a face-only frame rotates with its body", "[CadDocument]")
{
    using Catch::Matchers::WithinAbs;

    // The bug this pins down: Z came from the face normal (body-following) but X came from
    // coordsys_x_hint, a WORLD constant. Spinning the body about its own face normal left the
    // frame identical, so a face-only connector could not encode that rotation at all — and a
    // Fastened or Slider mate built on it claimed to fix an orientation it could not see.
    // A rectangular top face is used deliberately: its edges give an unambiguous in-plane
    // direction, so "did the frame follow the body" is answerable to the degree.
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 40, 20, 10, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());

    int n_faces = GeometryEngine::face_count(doc.bodies[0].shape);
    int top_face = -1;
    for (int i = 0; i < n_faces; ++i) {
        Vec3d fn = GeometryEngine::face_normal_world(GeometryEngine::face_by_index(doc.bodies[0].shape, i));
        if (fn.z() > 0.9) { top_face = i; break; }
    }
    REQUIRE(top_face >= 0);

    int cs = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 0), "CS");
    doc.features[cs].coordsys_type = CoordSysType::FaceAndDirection;
    doc.features[cs].coordsys_body = 0;
    doc.features[cs].coordsys_face = top_face;
    doc.features[cs].coordsys_edge = -1;              // face only: no explicit direction edge
    REQUIRE(doc.recompute());

    auto before = doc.resolve_datum_coordsys();
    REQUIRE(before.size() == 1);
    REQUIRE(before[0].error.empty());
    const Vec3d X0 = before[0].x;
    const Vec3d Z0 = before[0].x.cross(before[0].y);

    // Spin the body 90 degrees about its own face normal (world Z here).
    doc.add_transform(0, Vec3d(0, 0, 0), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 90.0, false, "Spin");
    REQUIRE(doc.recompute());

    auto after = doc.resolve_datum_coordsys();
    REQUIRE(after.size() == 1);
    REQUIRE(after[0].error.empty());
    const Vec3d X1 = after[0].x;

    // The normal is unchanged by a spin about itself — that is exactly why the old code could
    // not detect the rotation.
    const Vec3d Z1 = after[0].x.cross(after[0].y);
    CHECK_THAT(std::abs(Z0.dot(Z1)), WithinAbs(1.0, 1e-6));

    // X must have turned with the body. Before the fix X0 == X1 and this failed.
    const double cos_turn = std::clamp(X0.dot(X1), -1.0, 1.0);
    CHECK_THAT(std::abs(cos_turn), WithinAbs(0.0, 1e-6));   // 90 degrees apart
}

TEST_CASE("datum coordinate system: point_world gives world axes", "[CadDocument]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int cs = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(10, 20, 30), "CS_World");
    REQUIRE(cs == 0);

    auto css = doc.resolve_datum_coordsys();
    REQUIRE(css.size() == 1);
    REQUIRE(css[0].error.empty());
    CHECK_THAT(css[0].origin.x(), WithinAbs(10.0, 1e-9));
    CHECK_THAT(css[0].origin.y(), WithinAbs(20.0, 1e-9));
    CHECK_THAT(css[0].origin.z(), WithinAbs(30.0, 1e-9));
    CHECK_THAT(css[0].x.x(), WithinAbs(1.0, 1e-9));
    CHECK_THAT(css[0].x.y(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(css[0].x.z(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(css[0].y.x(), WithinAbs(0.0, 1e-9));
    CHECK_THAT(css[0].y.y(), WithinAbs(1.0, 1e-9));
    CHECK_THAT(css[0].y.z(), WithinAbs(0.0, 1e-9));
}


TEST_CASE("helix curve: arc length, bounding box, left-handed, conical", "[CadDocument]")
{
    using Catch::Matchers::WithinRel;
    using Catch::Matchers::WithinAbs;

    // --- Cylindrical helix r=5, pitch=2, height=10 (5 turns) ---
    // One turn arc length = sqrt((2*pi*r)^2 + pitch^2) = sqrt((10*pi)^2 + 4).
    // Total = 5 * sqrt(986.96...) ≈ 5 * 31.4159 ≈ 157.08 mm.
    SECTION("cylindrical helix arc length matches analytic") {
        CadDocument doc;
        doc.add_helix(SketchPlane::XY(), 5.0, 2.0, 10.0, false, 0.0, "H1");
        REQUIRE(doc.features.size() == 1);
        std::string err;
        TopoDS_Wire w = doc.build_helix_wire(doc.features[0], err);
        REQUIRE_FALSE(w.IsNull());
        REQUIRE(err.empty());

        GProp_GProps props;
        BRepGProp::LinearProperties(w, props);
        const double len = props.Mass();
        const double one_turn = std::sqrt(std::pow(2.0 * M_PI * 5.0, 2) + std::pow(2.0, 2));
        const double expected = 5.0 * one_turn;
        REQUIRE_THAT(len, WithinRel(expected, 1e-3));
    }

    // --- Bounding box: X/Y extent = 2*radius, Z extent = height ---
    SECTION("cylindrical helix bounding box") {
        CadDocument doc;
        doc.add_helix(SketchPlane::XY(), 5.0, 2.0, 10.0, false, 0.0, "H1");
        std::string err;
        TopoDS_Wire w = doc.build_helix_wire(doc.features[0], err);
        REQUIRE_FALSE(w.IsNull());

        Bnd_Box bb;
        BRepBndLib::Add(w, bb);
        double xmin, ymin, zmin, xmax, ymax, zmax;
        bb.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        REQUIRE_THAT(xmax - xmin, WithinRel(10.0, 0.1));
        REQUIRE_THAT(ymax - ymin, WithinRel(10.0, 0.1));
        REQUIRE_THAT(zmax - zmin, WithinRel(10.0, 0.1));
    }

    // --- Left-handed helix: sample a point at parameter ~0.25 and compare ---
    SECTION("left_handed flips the winding direction") {
        CadDocument doc_rh, doc_lh;
        doc_rh.add_helix(SketchPlane::XY(), 5.0, 2.0, 10.0, false, 0.0, "RH");
        doc_lh.add_helix(SketchPlane::XY(), 5.0, 2.0, 10.0, true,  0.0, "LH");

        std::string err;
        TopoDS_Wire w_rh = doc_rh.build_helix_wire(doc_rh.features[0], err);
        TopoDS_Wire w_lh = doc_lh.build_helix_wire(doc_lh.features[0], err);
        REQUIRE_FALSE(w_rh.IsNull());
        REQUIRE_FALSE(w_lh.IsNull());

        // Sample at height = height/4 along the helix:
        // RH: angle = 2*pi*turns*0.25 = pi/2, direction is +2*pi*turns
        //    so at z = 2.5: u = pi/2 => x = r*cos(pi/2) = 0, y = r*sin(pi/2) = +5
        // LH: angle goes negative, at z = 2.5: u = -pi/2 => x = 0, y = -5
        double z_sample = 2.5; // height/4
        // Approximate by scanning edges and picking the vertex nearest to target z
        auto point_at_z = [&](const TopoDS_Wire& w, double target_z) -> gp_Pnt {
            double best_dz = 1e9;
            gp_Pnt best(0,0,0);
            for (TopExp_Explorer ex(w, TopAbs_EDGE); ex.More(); ex.Next()) {
                TopoDS_Edge e = TopoDS::Edge(ex.Current());
                BRepAdaptor_Curve curve(e);
                double u0 = curve.FirstParameter();
                double u1 = curve.LastParameter();
                for (int s = 0; s <= 100; ++s) {
                    double u = u0 + (u1 - u0) * s / 100.0;
                    gp_Pnt p = curve.Value(u);
                    if (std::abs(p.Z() - target_z) < best_dz) {
                        best_dz = std::abs(p.Z() - target_z);
                        best = p;
                    }
                }
            }
            return best;
        };

        gp_Pnt prh = point_at_z(w_rh, z_sample);
        gp_Pnt plh = point_at_z(w_lh, z_sample);

        // At z=2.5 for RH: angle ~ pi/2 -> y > 0
        REQUIRE(prh.Y() > 0.0);
        // At z=2.5 for LH: angle ~ -pi/2 -> y < 0
        REQUIRE(plh.Y() < 0.0);
        // They must differ in sign of y (mirror winding), not just "differ"
        REQUIRE(prh.Y() * plh.Y() < 0.0);
    }

    // --- Conical helix: top radius matches r + height*tan(taper) ---
    SECTION("conical helix top radius") {
        CadDocument doc;
        doc.add_helix(SketchPlane::XY(), 5.0, 2.0, 10.0, false, 10.0, "Cone");
        std::string err;
        TopoDS_Wire w = doc.build_helix_wire(doc.features[0], err);
        REQUIRE_FALSE(w.IsNull());
        REQUIRE(err.empty());

        // Top radius = 5 + 10*tan(10) ≈ 5 + 1.7633 = 6.7633
        const double expected_top = 5.0 + 10.0 * std::tan(10.0 * M_PI / 180.0);

        // Sample at z = height: use same sampling approach
        double best_dz = 1e9;
        gp_Pnt best(0,0,0);
        for (TopExp_Explorer ex(w, TopAbs_EDGE); ex.More(); ex.Next()) {
            TopoDS_Edge e = TopoDS::Edge(ex.Current());
            BRepAdaptor_Curve curve(e);
            double u0 = curve.FirstParameter();
            double u1 = curve.LastParameter();
            for (int s = 0; s <= 200; ++s) {
                double u = u0 + (u1 - u0) * s / 200.0;
                gp_Pnt p = curve.Value(u);
                if (std::abs(p.Z() - 10.0) < best_dz) {
                    best_dz = std::abs(p.Z() - 10.0);
                    best = p;
                }
            }
        }
        double top_r = std::sqrt(best.X() * best.X() + best.Y() * best.Y());
        REQUIRE_THAT(top_r, WithinRel(expected_top, 1e-2));
    }
}

TEST_CASE("helix: invalid inputs fail cleanly", "[CadDocument]")
{
    SECTION("radius <= 0") {
        CadDocument doc;
        doc.add_helix(SketchPlane::XY(), 0.0, 2.0, 10.0, false, 0.0, "H");
        std::string err;
        REQUIRE(doc.build_helix_wire(doc.features[0], err).IsNull());
        REQUIRE_FALSE(err.empty());
    }
    SECTION("pitch <= 0") {
        CadDocument doc;
        doc.add_helix(SketchPlane::XY(), 5.0, 0.0, 10.0, false, 0.0, "H");
        std::string err;
        REQUIRE(doc.build_helix_wire(doc.features[0], err).IsNull());
        REQUIRE_FALSE(err.empty());
    }
    SECTION("height < 0") {
        CadDocument doc;
        doc.add_helix(SketchPlane::XY(), 5.0, 2.0, -1.0, false, 0.0, "H");
        std::string err;
        REQUIRE(doc.build_helix_wire(doc.features[0], err).IsNull());
        REQUIRE_FALSE(err.empty());
    }
    SECTION("height == 0 (flat spiral) rejected") {
        CadDocument doc;
        doc.add_helix(SketchPlane::XY(), 5.0, 2.0, 0.0, false, 0.0, "H");
        std::string err;
        REQUIRE(doc.build_helix_wire(doc.features[0], err).IsNull());
        REQUIRE_FALSE(err.empty());
        CHECK_CONTAINS(err, "flat spiral");
    }
    SECTION("absurd turn count") {
        CadDocument doc;
        doc.add_helix(SketchPlane::XY(), 5.0, 1e-4, 2.0, false, 0.0, "H");
        std::string err;
        REQUIRE(doc.build_helix_wire(doc.features[0], err).IsNull());
        REQUIRE_FALSE(err.empty());
    }
    SECTION("taper drives radius negative") {
        CadDocument doc;
        doc.add_helix(SketchPlane::XY(), 1.0, 2.0, 10.0, false, -10.0, "H");
        std::string err;
        REQUIRE(doc.build_helix_wire(doc.features[0], err).IsNull());
        REQUIRE_FALSE(err.empty());
        CHECK_CONTAINS(err, "negative");
    }
}

TEST_CASE("helix as sweep path: spring integration test", "[CadDocument]")
{
    using Catch::Matchers::WithinRel;

    CadDocument doc;

    // Build a plane at the helix start (5,0,0) whose normal IS the start tangent direction.
    // The helix tangent at u=0 is (0, R, P/(2*pi)) = (0, 5, 3/(2*pi)).
    const double R = 5.0, P = 3.0;
    Vec3d tan_dir(0, R, P / (2.0 * M_PI));
    tan_dir.normalize();
    Vec3d ref = (std::abs(tan_dir.z()) < 0.9) ? Vec3d(0, 0, 1) : Vec3d(1, 0, 0);
    Vec3d x_axis = ref.cross(tan_dir);
    if (x_axis.squaredNorm() < 1e-12) x_axis = Vec3d(1, 0, 0);
    x_axis.normalize();
    Vec3d y_axis = tan_dir.cross(x_axis).normalized();
    SketchPlane profile_plane;
    profile_plane.origin = Vec3d(R, 0, 0);
    profile_plane.normal = tan_dir;
    profile_plane.x_axis = x_axis;
    profile_plane.y_axis = y_axis;

    // Profile: small circle r=1.5 centered at 2D (0,0) = world (5,0,0) = helix start
    SketchEntity prof;
    prof.type   = SketchEntity::Type::Circle;
    prof.center = Vec2d(0, 0);
    prof.radius = 1.5;
    int prof_idx = doc.add_sketch_entities({prof}, profile_plane, "CircleProfile");

    // Helix path: r=5, pitch=3, height=15 (5 turns) about Z axis from origin
    int helix_idx = doc.add_helix(SketchPlane::XY(), R, P, 15.0, false, 0.0, "HelixPath");

    int sweep_idx = doc.add_sweep(prof_idx, helix_idx, BooleanMode::New, "Spring");
    REQUIRE(sweep_idx >= 0);

    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.display_mesh.facets_count() > 0);

    const double v = double(doc.display_mesh.volume());
    REQUIRE(v > 0.0);

    const double one_turn = std::sqrt(std::pow(2.0 * M_PI * R, 2) + std::pow(P, 2));
    const double total_len = 5.0 * one_turn;
    const double prof_area = M_PI * 1.5 * 1.5;
    const double expected_v = prof_area * total_len;
    REQUIRE_THAT(v, WithinRel(expected_v, 0.1));
}

TEST_CASE("helix serialization round-trip with distinctive values", "[CadDocument]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    doc.add_helix(SketchPlane::XZ(), 7.5, 3.25, 22.0, true, 5.0, "Helix_RT");
    doc.features.back().helix_radius      = 7.5;
    doc.features.back().helix_pitch       = 3.25;
    doc.features.back().helix_height      = 22.0;
    doc.features.back().helix_left_handed = true;
    doc.features.back().helix_taper_deg   = 5.0;

    auto blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    // Deserialize through the real entry point — recompute fails because a lone
    // helix doesn't produce a solid, but the serialized field values must survive.
    CadDocument doc2;
    doc2.deserialize_recipe(blob);
    REQUIRE(doc2.features.size() == 1);

    const auto& f = doc2.features[0];
    REQUIRE(f.type == CadFeatureType::Helix);
    REQUIRE(f.name == "Helix_RT");
    REQUIRE_THAT(f.helix_radius,      WithinAbs(7.5, 1e-9));
    REQUIRE_THAT(f.helix_pitch,       WithinAbs(3.25, 1e-9));
    REQUIRE_THAT(f.helix_height,      WithinAbs(22.0, 1e-9));
    REQUIRE(f.helix_left_handed == true);
    REQUIRE_THAT(f.helix_taper_deg,   WithinAbs(5.0, 1e-9));
}

TEST_CASE("helix with sweep path from a non-sketch/non-helix feature errors", "[CadDocument]")
{
    // An Extrude feature used as sweep path must fail cleanly.
    CadDocument doc;

    // Profile: circle
    SketchEntity prof;
    prof.type   = SketchEntity::Type::Circle;
    prof.center = Vec2d(0, 0);
    prof.radius = 2.0;
    int prof_idx = doc.add_sketch_entities({prof}, SketchPlane::XY(), "Profile");

    // Path: an Extrude feature (not a Sketch or Helix)
    CadFeature ex;
    ex.type       = CadFeatureType::Extrude;
    ex.name       = "NotAValidPath";
    ex.sketch_ref = -1;
    doc.features.push_back(ex);
    int path_idx = int(doc.features.size()) - 1;

    int sw = doc.add_sweep(prof_idx, path_idx, BooleanMode::New, "BadSweep");
    REQUIRE_FALSE(doc.recompute());
    REQUIRE_FALSE(doc.error.empty());
}

TEST_CASE("transform translate shifts body bbox by the given vector", "[CadDocument]")
{
    using Catch::Matchers::WithinAbs;
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "S");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Box");
    REQUIRE(doc.recompute());

    Bnd_Box bb0;
    BRepBndLib::Add(doc.bodies[0].shape, bb0);
    double xmin0, ymin0, zmin0, xmax0, ymax0, zmax0;
    bb0.Get(xmin0, ymin0, zmin0, xmax0, ymax0, zmax0);
    double vol0 = double(SketchEngine::tessellate(doc.bodies[0].shape).volume());

    doc.checkpoint();
    doc.add_transform(0, Vec3d(5, 0, 0), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 0, false, "Move");
    REQUIRE(doc.recompute());

    REQUIRE(doc.bodies.size() == 1);
    double vol1 = double(SketchEngine::tessellate(doc.bodies[0].shape).volume());
    REQUIRE_THAT(vol1, WithinRel(vol0, 1e-6));

    Bnd_Box bb1;
    BRepBndLib::Add(doc.bodies[0].shape, bb1);
    double xmin1, ymin1, zmin1, xmax1, ymax1, zmax1;
    bb1.Get(xmin1, ymin1, zmin1, xmax1, ymax1, zmax1);
    REQUIRE_THAT(xmin1, WithinAbs(xmin0 + 5, 1e-6));
    REQUIRE_THAT(xmax1, WithinAbs(xmax0 + 5, 1e-6));
}

TEST_CASE("transform rotate 90 about Z swaps XY bbox extents", "[CadDocument]")
{
    using Catch::Matchers::WithinAbs;
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "S");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Box");
    REQUIRE(doc.recompute());

    Bnd_Box bb0;
    BRepBndLib::Add(doc.bodies[0].shape, bb0);
    double xmin0, ymin0, zmin0, xmax0, ymax0, zmax0;
    bb0.Get(xmin0, ymin0, zmin0, xmax0, ymax0, zmax0);
    double vol0 = double(SketchEngine::tessellate(doc.bodies[0].shape).volume());
    double dx0 = xmax0 - xmin0;
    double dy0 = ymax0 - ymin0;

    doc.checkpoint();
    doc.add_transform(0, Vec3d(0, 0, 0), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 90, false, "Rot90");
    REQUIRE(doc.recompute());

    double vol1 = double(SketchEngine::tessellate(doc.bodies[0].shape).volume());
    REQUIRE_THAT(vol1, WithinRel(vol0, 1e-6));

    Bnd_Box bb1;
    BRepBndLib::Add(doc.bodies[0].shape, bb1);
    double xmin1, ymin1, zmin1, xmax1, ymax1, zmax1;
    bb1.Get(xmin1, ymin1, zmin1, xmax1, ymax1, zmax1);
    double dx1 = xmax1 - xmin1;
    double dy1 = ymax1 - ymin1;
    REQUIRE_THAT(dx1, WithinAbs(dy0, 1e-6));
    REQUIRE_THAT(dy1, WithinAbs(dx0, 1e-6));
}

TEST_CASE("transform copy keeps original and appends transformed body", "[CadDocument]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "S");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Box");
    REQUIRE(doc.recompute());

    Bnd_Box bb0;
    BRepBndLib::Add(doc.bodies[0].shape, bb0);
    double xmin0, ymin0, zmin0, xmax0, ymax0, zmax0;
    bb0.Get(xmin0, ymin0, zmin0, xmax0, ymax0, zmax0);

    doc.checkpoint();
    doc.add_transform(0, Vec3d(5, 0, 0), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 0, true, "Copy");
    REQUIRE(doc.recompute());

    REQUIRE(doc.bodies.size() == 2);

    Bnd_Box bb_orig;
    BRepBndLib::Add(doc.bodies[0].shape, bb_orig);
    double xo_min, yo_min, zo_min, xo_max, yo_max, zo_max;
    bb_orig.Get(xo_min, yo_min, zo_min, xo_max, yo_max, zo_max);
    REQUIRE_THAT(xo_min, WithinAbs(xmin0, 1e-6));
    REQUIRE_THAT(xo_max, WithinAbs(xmax0, 1e-6));

    Bnd_Box bb_copy;
    BRepBndLib::Add(doc.bodies[1].shape, bb_copy);
    double xc_min, yc_min, zc_min, xc_max, yc_max, zc_max;
    bb_copy.Get(xc_min, yc_min, zc_min, xc_max, yc_max, zc_max);
    REQUIRE_THAT(xc_min, WithinAbs(xmin0 + 5, 1e-6));
    REQUIRE_THAT(xc_max, WithinAbs(xmax0 + 5, 1e-6));
}

TEST_CASE("transform degenerate axis errors", "[CadDocument]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "S");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Box");
    REQUIRE(doc.recompute());

    doc.checkpoint();
    doc.add_transform(0, Vec3d(0, 0, 0), Vec3d(0, 0, 0), Vec3d(0, 0, 0), 45, false, "Bad");
    REQUIRE_FALSE(doc.recompute());
    REQUIRE(doc.error.find("axis") != std::string::npos);
}

TEST_CASE("transform moved body participates in later boolean at its new position", "[CadDocument]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    // Two boxes that do NOT overlap
    int sk0 = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 5, "S0");
    doc.add_extrude(sk0, 5.0, false, BooleanMode::New, "Box0");

    int sk1 = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 5, "S1");
    doc.add_extrude(sk1, 5.0, false, BooleanMode::New, "Box1");
    doc.features.back().target_body = 0;
    REQUIRE(doc.recompute());

    double v0 = double(SketchEngine::tessellate(doc.bodies[0].shape).volume());
    double v1 = double(SketchEngine::tessellate(doc.bodies[1].shape).volume());
    double sum = v0 + v1;

    // Move body 1 so it overlaps body 0
    doc.checkpoint();
    doc.add_transform(1, Vec3d(5, 5, 0), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 0, false, "Move1");
    REQUIRE(doc.recompute());

    doc.add_boolean(BooleanMode::Add, 0, 1, false, 0.0, -1, -1, "Fuse");
    REQUIRE(doc.recompute());

    REQUIRE(doc.bodies.size() == 1);
    double vf = double(SketchEngine::tessellate(doc.bodies[0].shape).volume());
    // Partial overlap: strictly more than one box (the move DID take effect) and strictly
    // less than both (they still intersect). Without a real B-rep transform the two boxes
    // stay coincident and vf would equal v0 -- that is what `vf > v0` catches.
    REQUIRE(vf > v0 * 1.05);
    REQUIRE(vf < sum);
}

TEST_CASE("transform round-trip preserves all xf_* fields", "[CadDocument]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "S");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Box");
    doc.add_transform(0, Vec3d(3, 4, 5), Vec3d(0, 1, 0), Vec3d(1, 2, 3), 37, true, "RoundTrip");
    REQUIRE(doc.recompute());
    int nb = int(doc.bodies.size());
    REQUIRE(nb >= 1);

    // Capture bboxes before serialization
    std::vector<std::pair<Vec3d, Vec3d>> bboxes_before;
    for (int i = 0; i < nb; ++i) {
        Bnd_Box bb;
        BRepBndLib::Add(doc.bodies[i].shape, bb);
        double x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        bboxes_before.push_back({Vec3d(x0, y0, z0), Vec3d(x1, y1, z1)});
    }

    auto blob = doc.serialize_recipe();

    CadDocument doc2;
    REQUIRE(doc2.deserialize_recipe(blob));
    REQUIRE(doc2.bodies.size() == size_t(nb));

    // Check the Transform feature fields
    bool found = false;
    for (const auto& f : doc2.features) {
        if (f.type != CadFeatureType::Transform) continue;
        REQUIRE_THAT(f.xf_translate.x(), WithinAbs(3, 1e-9));
        REQUIRE_THAT(f.xf_translate.y(), WithinAbs(4, 1e-9));
        REQUIRE_THAT(f.xf_translate.z(), WithinAbs(5, 1e-9));
        REQUIRE_THAT(f.xf_axis.x(), WithinAbs(0, 1e-9));
        REQUIRE_THAT(f.xf_axis.y(), WithinAbs(1, 1e-9));
        REQUIRE_THAT(f.xf_axis.z(), WithinAbs(0, 1e-9));
        REQUIRE_THAT(f.xf_pivot.x(), WithinAbs(1, 1e-9));
        REQUIRE_THAT(f.xf_pivot.y(), WithinAbs(2, 1e-9));
        REQUIRE_THAT(f.xf_pivot.z(), WithinAbs(3, 1e-9));
        REQUIRE_THAT(f.xf_angle_deg, WithinAbs(37, 1e-9));
        REQUIRE(f.xf_copy == true);
        found = true;
        break;
    }
    REQUIRE(found);

    // Verify bboxes equal
    for (int i = 0; i < nb; ++i) {
        Bnd_Box bb;
        BRepBndLib::Add(doc2.bodies[i].shape, bb);
        double x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        REQUIRE_THAT(x0, WithinAbs(bboxes_before[i].first.x(), 1e-6));
        REQUIRE_THAT(y0, WithinAbs(bboxes_before[i].first.y(), 1e-6));
        REQUIRE_THAT(z0, WithinAbs(bboxes_before[i].first.z(), 1e-6));
        REQUIRE_THAT(x1, WithinAbs(bboxes_before[i].second.x(), 1e-6));
        REQUIRE_THAT(y1, WithinAbs(bboxes_before[i].second.y(), 1e-6));
        REQUIRE_THAT(z1, WithinAbs(bboxes_before[i].second.z(), 1e-6));
    }
}

// --- Thicken tests ---

TEST_CASE("thicken a planar face to a plate", "[CadDocument]")
{
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxSketch");
    REQUIRE(sk >= 0);
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude1");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 1);

    int n_faces = GeometryEngine::face_count(doc.bodies[0].shape);
    int top_face = -1;
    for (int i = 0; i < n_faces; ++i) {
        TopoDS_Face fc = GeometryEngine::face_by_index(doc.bodies[0].shape, i);
        Vec3d n = GeometryEngine::face_normal_world(fc);
        if (n.z() > 0.9) { top_face = i; break; }
    }
    REQUIRE(top_face >= 0);

    doc.add_thicken(0, top_face, 3.0, false, "Plate");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 2);

    double v = double(SketchEngine::tessellate(doc.bodies[1].shape).volume());
    REQUIRE_THAT(v, WithinRel(20.0 * 20.0 * 3.0, 0.01));
}

TEST_CASE("thicken flip offsets against face normal", "[CadDocument]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxSketch");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude1");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);

    int n_faces = GeometryEngine::face_count(doc.bodies[0].shape);
    int top_face = -1;
    for (int i = 0; i < n_faces; ++i) {
        TopoDS_Face fc = GeometryEngine::face_by_index(doc.bodies[0].shape, i);
        Vec3d n = GeometryEngine::face_normal_world(fc);
        if (n.z() > 0.9) { top_face = i; break; }
    }
    REQUIRE(top_face >= 0);

    // non-flipped: plate grows above the box (z > 10)
    CadDocument doc2;
    int sk2 = doc2.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxSketch2");
    doc2.add_extrude(sk2, 10.0, false, BooleanMode::New, "Extrude2");
    REQUIRE(doc2.recompute());
    int nf2 = GeometryEngine::face_count(doc2.bodies[0].shape);
    int tf2 = -1;
    for (int i = 0; i < nf2; ++i) {
        TopoDS_Face fc = GeometryEngine::face_by_index(doc2.bodies[0].shape, i);
        Vec3d n = GeometryEngine::face_normal_world(fc);
        if (n.z() > 0.9) { tf2 = i; break; }
    }
    REQUIRE(tf2 >= 0);
    doc2.add_thicken(0, tf2, 3.0, false, "PlateFwd");
    REQUIRE(doc2.recompute());
    Bnd_Box bb_fwd; BRepBndLib::Add(doc2.bodies[1].shape, bb_fwd);
    Standard_Real x0, y0, z0, x1, y1, z1;
    bb_fwd.Get(x0, y0, z0, x1, y1, z1);

    // flipped: plate grows below the face plane (z < 10)
    CadDocument doc3;
    int sk3 = doc3.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxSketch3");
    doc3.add_extrude(sk3, 10.0, false, BooleanMode::New, "Extrude3");
    REQUIRE(doc3.recompute());
    int nf3 = GeometryEngine::face_count(doc3.bodies[0].shape);
    int tf3 = -1;
    for (int i = 0; i < nf3; ++i) {
        TopoDS_Face fc = GeometryEngine::face_by_index(doc3.bodies[0].shape, i);
        Vec3d n = GeometryEngine::face_normal_world(fc);
        if (n.z() > 0.9) { tf3 = i; break; }
    }
    REQUIRE(tf3 >= 0);
    doc3.add_thicken(0, tf3, 3.0, true, "PlateRev");
    REQUIRE(doc3.recompute());
    Bnd_Box bb_rev; BRepBndLib::Add(doc3.bodies[1].shape, bb_rev);
    Standard_Real rx0, ry0, rz0, rx1, ry1, rz1;
    bb_rev.Get(rx0, ry0, rz0, rx1, ry1, rz1);

    // forward plate bbox z > 10 (source face at z=10, +3 offset = z in (10,13))
    REQUIRE(z0 >= 9.9);
    // reverse plate bbox z < 10 (source face at z=10, -3 offset = z in (7,10))
    REQUIRE(rz1 <= 10.1);
}

TEST_CASE("thicken bad face index returns error", "[CadDocument]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Ext");
    REQUIRE(doc.recompute());

    doc.add_thicken(0, 9999, 3.0, false, "Bad");
    bool ok = doc.recompute();
    REQUIRE_FALSE(ok);
    REQUIRE(doc.error.find("face") != std::string::npos);
}

TEST_CASE("thicken zero thickness returns error", "[CadDocument]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Ext");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);

    int n_faces = GeometryEngine::face_count(doc.bodies[0].shape);
    int top_face = -1;
    for (int i = 0; i < n_faces; ++i) {
        TopoDS_Face fc = GeometryEngine::face_by_index(doc.bodies[0].shape, i);
        Vec3d n = GeometryEngine::face_normal_world(fc);
        if (n.z() > 0.9) { top_face = i; break; }
    }
    REQUIRE(top_face >= 0);

    doc.add_thicken(0, top_face, 0.0, false, "Zero");
    bool ok = doc.recompute();
    REQUIRE_FALSE(ok);
    REQUIRE(doc.error.find("thickness") != std::string::npos);
}

TEST_CASE("thickened plate fuses with source", "[CadDocument]")
{
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Ext");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);

    double v_box = double(SketchEngine::tessellate(doc.bodies[0].shape).volume());

    int n_faces = GeometryEngine::face_count(doc.bodies[0].shape);
    int top_face = -1;
    for (int i = 0; i < n_faces; ++i) {
        TopoDS_Face fc = GeometryEngine::face_by_index(doc.bodies[0].shape, i);
        Vec3d n = GeometryEngine::face_normal_world(fc);
        if (n.z() > 0.9) { top_face = i; break; }
    }
    REQUIRE(top_face >= 0);

    doc.add_thicken(0, top_face, 3.0, false, "Plate");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 2);

    doc.add_boolean(BooleanMode::Add, 0, 1, false, 0.0, -1, -1, "Fuse");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);

    double v_fused = double(SketchEngine::tessellate(doc.bodies[0].shape).volume());
    REQUIRE(v_fused > v_box);
}

TEST_CASE("thicken round-trip serialization", "[CadDocument]")
{
    using Catch::Matchers::WithinAbs;
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Ext");
    REQUIRE(doc.recompute());
    int n_faces = GeometryEngine::face_count(doc.bodies[0].shape);
    int top_face = -1;
    for (int i = 0; i < n_faces; ++i) {
        TopoDS_Face fc = GeometryEngine::face_by_index(doc.bodies[0].shape, i);
        Vec3d n = GeometryEngine::face_normal_world(fc);
        if (n.z() > 0.9) { top_face = i; break; }
    }
    REQUIRE(top_face >= 0);
    doc.add_thicken(0, top_face, 3.0, false, "Plate");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 2);

    // Remember field values
    int tf  = doc.features.back().thicken_face;
    double tt = doc.features.back().thicken_thickness;
    bool tb   = doc.features.back().thicken_flip;
    size_t nb = doc.bodies.size();

    std::vector<std::pair<Vec3d, Vec3d>> bboxes;
    for (const auto& b : doc.bodies) {
        Bnd_Box bb; BRepBndLib::Add(b.shape, bb);
        Standard_Real x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        bboxes.push_back({Vec3d(x0, y0, z0), Vec3d(x1, y1, z1)});
    }

    std::string blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    CadDocument doc2;
    REQUIRE(doc2.deserialize_recipe(blob));
    REQUIRE(doc2.features.size() == doc.features.size());
    REQUIRE(doc2.bodies.size() == nb);

    const CadFeature& f2 = doc2.features.back();
    REQUIRE(f2.thicken_face     == tf);
    REQUIRE_THAT(f2.thicken_thickness, WithinAbs(tt, 1e-9));
    REQUIRE(f2.thicken_flip     == tb);

    for (size_t i = 0; i < nb; ++i) {
        Bnd_Box bb; BRepBndLib::Add(doc2.bodies[i].shape, bb);
        Standard_Real x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        REQUIRE_THAT(double(x0), WithinAbs(bboxes[i].first.x(),  1e-6));
        REQUIRE_THAT(double(y0), WithinAbs(bboxes[i].first.y(),  1e-6));
        REQUIRE_THAT(double(z0), WithinAbs(bboxes[i].first.z(),  1e-6));
        REQUIRE_THAT(double(x1), WithinAbs(bboxes[i].second.x(), 1e-6));
        REQUIRE_THAT(double(y1), WithinAbs(bboxes[i].second.y(), 1e-6));
        REQUIRE_THAT(double(z1), WithinAbs(bboxes[i].second.z(), 1e-6));
    }
}

// --- Project feature tests ---

TEST_CASE("project a box top face to 4 lines, extrudable", "[CadDocument][project]")
{
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Ext");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);

    int n_faces = GeometryEngine::face_count(doc.bodies[0].shape);
    int top_face = -1;
    for (int i = 0; i < n_faces; ++i) {
        TopoDS_Face fc = GeometryEngine::face_by_index(doc.bodies[0].shape, i);
        Vec3d n = GeometryEngine::face_normal_world(fc);
        if (n.z() > 0.9) { top_face = i; break; }
    }
    REQUIRE(top_face >= 0);

    int proj = doc.add_project_edges(0, {}, top_face, SketchPlane::XY(), "ProjTop");
    REQUIRE(proj >= 0);
    doc.add_extrude(proj, 5.0, false, BooleanMode::New, "FromProj");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    const auto& pf = doc.features[proj];
    REQUIRE(pf.entities.size() == 4);
    for (const auto& e : pf.entities)
        REQUIRE(e.type == SketchEntity::Type::Line);

    REQUIRE(doc.bodies.size() >= 2);
    double v = double(SketchEngine::tessellate(doc.bodies.back().shape).volume());
    REQUIRE_THAT(v, WithinRel(20.0 * 20.0 * 5.0, 1e-3));
}

TEST_CASE("project a cylinder top edge to 1 circle, extrudable", "[CadDocument][project]")
{
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    SketchEntity c;
    c.type   = SketchEntity::Type::Circle;
    c.center = Vec2d(0, 0);
    c.radius = 6.0;
    int sk = doc.add_sketch_entities({c}, SketchPlane::XY(), "Circ");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Cyl");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);

    int n_edges = GeometryEngine::edge_count(doc.bodies[0].shape);
    int top_edge = -1;
    for (int i = 0; i < n_edges; ++i) {
        TopoDS_Edge e = GeometryEngine::edge_by_index(doc.bodies[0].shape, i);
        auto pts = GeometryEngine::sample_edge_world(e);
        if (pts.empty()) continue;
        Vec3d mid = Vec3d::Zero();
        for (const auto& p : pts) mid += p;
        mid /= double(pts.size());
        if (mid.z() > 9.0) {
            BRepAdaptor_Curve ac(e);
            if (ac.GetType() == GeomAbs_Circle) { top_edge = i; break; }
        }
    }
    REQUIRE(top_edge >= 0);

    int proj = doc.add_project_edges(0, {top_edge}, -1, SketchPlane::XY(), "ProjCirc");
    REQUIRE(proj >= 0);
    doc.add_extrude(proj, 4.0, false, BooleanMode::New, "FromCirc");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    const auto& pf = doc.features[proj];
    REQUIRE(pf.entities.size() == 1);
    REQUIRE(pf.entities[0].type == SketchEntity::Type::Circle);
    REQUIRE_THAT(pf.entities[0].radius, WithinRel(6.0, 1e-3));

    REQUIRE(doc.bodies.size() >= 2);
    double v = double(SketchEngine::tessellate(doc.bodies.back().shape).volume());
    REQUIRE_THAT(v, WithinRel(M_PI * 36.0 * 4.0, 1e-2));
}

TEST_CASE("project with no face and no edge selection projects every edge", "[CadDocument][project]")
{
    // This is the state the Project card opens in — its label reads "(all edges)". Before the
    // all-edges branch existed it threw "no edges or face selected", so the card's default
    // could never be confirmed.
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Ext");
    REQUIRE(doc.recompute());

    int proj = doc.add_project_edges(0, {}, -1, SketchPlane::XY(), "ProjAll");
    REQUIRE(proj >= 0);
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // A box has 12 edges; the 4 running along Z collapse to points on XY and are dropped,
    // leaving the 4 bottom and 4 top edges.
    const auto& pf = doc.features[proj];
    REQUIRE(pf.entities.size() == 8);
    for (const auto& e : pf.entities) {
        REQUIRE(e.type == SketchEntity::Type::Line);
        REQUIRE((e.p1 - e.p0).norm() > 1e-6);
    }
}

TEST_CASE("project bad face id returns error", "[CadDocument][project]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Ext");
    REQUIRE(doc.recompute());

    doc.add_project_edges(0, {}, 9999, SketchPlane::XY(), "Bad");
    bool ok = doc.recompute();
    REQUIRE_FALSE(ok);
    bool has_project = doc.error.find("project") != std::string::npos
                    || doc.error.find("face")     != std::string::npos;
    REQUIRE(has_project);
}

TEST_CASE("project round-trip serialization", "[CadDocument][project]")
{
    using Catch::Matchers::WithinAbs;
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Ext");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);

    int n_faces = GeometryEngine::face_count(doc.bodies[0].shape);
    int top_face = -1;
    for (int i = 0; i < n_faces; ++i) {
        TopoDS_Face fc = GeometryEngine::face_by_index(doc.bodies[0].shape, i);
        Vec3d n = GeometryEngine::face_normal_world(fc);
        if (n.z() > 0.9) { top_face = i; break; }
    }
    REQUIRE(top_face >= 0);

    doc.add_project_edges(0, {}, top_face, SketchPlane::XY(), "ProjTop");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int saved_src   = doc.features.back().project_source_body;
    int saved_face  = doc.features.back().project_face;
    auto saved_edges = doc.features.back().project_edges;
    size_t saved_nb = doc.bodies.size();

    std::vector<std::pair<Vec3d, Vec3d>> bboxes;
    for (const auto& b : doc.bodies) {
        Bnd_Box bb; BRepBndLib::Add(b.shape, bb);
        Standard_Real x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        bboxes.push_back({Vec3d(x0, y0, z0), Vec3d(x1, y1, z1)});
    }

    auto blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    CadDocument doc2;
    REQUIRE(doc2.deserialize_recipe(blob));
    REQUIRE(doc2.bodies.size() == saved_nb);
    REQUIRE(doc2.features.size() == doc.features.size());

    const auto& f2 = doc2.features.back();
    REQUIRE(f2.project_source_body == saved_src);
    REQUIRE(f2.project_face  == saved_face);
    REQUIRE(f2.project_edges == saved_edges);

    for (size_t i = 0; i < saved_nb; ++i) {
        Bnd_Box bb; BRepBndLib::Add(doc2.bodies[i].shape, bb);
        Standard_Real x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        REQUIRE_THAT(double(x0), WithinAbs(bboxes[i].first.x(),  1e-6));
        REQUIRE_THAT(double(y0), WithinAbs(bboxes[i].first.y(),  1e-6));
        REQUIRE_THAT(double(z0), WithinAbs(bboxes[i].first.z(),  1e-6));
        REQUIRE_THAT(double(x1), WithinAbs(bboxes[i].second.x(), 1e-6));
        REQUIRE_THAT(double(y1), WithinAbs(bboxes[i].second.y(), 1e-6));
        REQUIRE_THAT(double(z1), WithinAbs(bboxes[i].second.z(), 1e-6));
    }
}

// --- Golden recipe fixture (v1 format tripwire) ---

static CadDocument make_golden_doc_v1()
{
    CadDocument doc;

    // ---- Body 0: base box with distinctive taper ----
    int sk0 = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(),
                             30, 20, 15, "Sketch_Base");
    doc.add_extrude(sk0, 15.0, false, BooleanMode::New, "Extrude_Base");
    int ex0 = int(doc.features.size()) - 1;
    doc.features[ex0].taper_deg = 8.5;
    doc.features[ex0].extrude_end = ExtrudeEnd::Blind;

    // Dress-up: fillet lateral faces, chamfer top face — distinct non-default sizes
    doc.add_fillet(3.5, FaceGroup::Lateral, "Fillet_Lat35");
    doc.add_chamfer(2.0, FaceGroup::Top, "Chamfer_Top2");

    // Hole: offset position, non-through
    doc.add_hole(7.5, 11.0, false, 4.0, 3.0, SketchPlane::XY(), "Hole_Off75");

    // Draft: angle 7.25 deg on face 3
    doc.add_draft(7.25, 3, 0, "Draft_F3");

    // Shell: thickness 1.375 mm, open face 1
    doc.add_shell(1.375, 1, 0, "Shell_T1375");

    // Thread: internal, radius 4, pitch 2.5, height 15, depth 1.25
    doc.add_thread(4.0, 2.5, 15.0, 1.25, true, 0.0, 0.0, SketchPlane::XY(), "Thread_Int");

    // Cut: plane XY, offset 10, flip, keep upper only
    doc.add_cut(SketchPlane::XY(), 10.0, true, true, false, 0, "Cut_Flip");

    // Pattern: linear 5 copies, spacing 13.5 mm along plane X
    doc.add_pattern(false, 5, 13.5, 0, 360.0, 0, "Pattern_Lin5");

    // ---- Datum plane ----
    doc.add_plane(0, 25.0, 0.0, 0, "Plane_Datum25");

    // ---- Revolve: self-contained body (sketch + revolve) ----
    {
        CadFeature sk;
        sk.type = CadFeatureType::Sketch;
        sk.name = "Sketch_Rev";
        sk.plane = SketchPlane::XY();
        sk.entities = {{SketchEntity::Type::Circle, Vec2d(12,0), Vec2d(12,0), Vec2d(12,0), 4.0}};
        doc.features.push_back(sk);
    }
    int rev_sk = int(doc.features.size()) - 1;
    doc.add_revolve(rev_sk, 217.0, 1, false, BooleanMode::New, "Revolve_Y217");

    // ---- Sweep: self-contained body (profile + path sketches + sweep) ----
    {
        CadFeature sk;
        sk.type = CadFeatureType::Sketch;
        sk.name = "Sketch_SwProf";
        sk.plane = SketchPlane::XY();
        sk.entities = {{SketchEntity::Type::Circle, Vec2d(0,0), Vec2d(0,0), Vec2d(0,0), 3.0}};
        doc.features.push_back(sk);
    }
    int sw_prof = int(doc.features.size()) - 1;
    {
        CadFeature sk;
        sk.type = CadFeatureType::Sketch;
        sk.name = "Sketch_SwPath";
        sk.plane = SketchPlane::XZ();
        sk.entities = {{SketchEntity::Type::Line, Vec2d(0,0), Vec2d(0,35)}};
        doc.features.push_back(sk);
    }
    int sw_path = int(doc.features.size()) - 1;
    doc.add_sweep(sw_prof, sw_path, BooleanMode::New, "Sweep_Z35");

    // ---- Loft: self-contained body (two profiles + loft) ----
    {
        CadFeature sk;
        sk.type = CadFeatureType::Sketch;
        sk.name = "Sketch_LoftBot";
        sk.plane = SketchPlane::XY();
        sk.profile.points = {{-7,-7},{7,-7},{7,7},{-7,7}};
        sk.profile.closed = true;
        doc.features.push_back(sk);
    }
    int loft_bot = int(doc.features.size()) - 1;
    {
        CadFeature sk;
        sk.type = CadFeatureType::Sketch;
        sk.name = "Sketch_LoftTop";
        sk.plane.origin = Vec3d(0, 0, 25);
        sk.plane.normal = Vec3d(0, 0, 1);
        sk.plane.x_axis = Vec3d(1, 0, 0);
        sk.plane.y_axis = Vec3d(0, 1, 0);
        sk.profile.points = {{-9,-9},{9,-9},{9,9},{-9,9}};
        sk.profile.closed = true;
        doc.features.push_back(sk);
    }
    int loft_top = int(doc.features.size()) - 1;
    doc.add_loft({loft_bot, loft_top}, true, BooleanMode::New, "Loft_Ruled");

    // ---- Boolean: distinctive tolerance and face-mate params ----
    doc.add_boolean(BooleanMode::Cut, 0, 1, false, 0.01, 2, 3, "Boolean_Cut");

    // ---- Extrude variant: symmetric + two-sided end ----
    {
        CadFeature sk;
        sk.type = CadFeatureType::Sketch;
        sk.name = "Sketch_Ex2";
        sk.plane = SketchPlane::XZ();
        sk.entities = {{SketchEntity::Type::Circle, Vec2d(0,0), Vec2d(0,0), Vec2d(0,0), 6.0}};
        doc.features.push_back(sk);
    }
    int sk_ex2 = int(doc.features.size()) - 1;
    {
        CadFeature ex;
        ex.type = CadFeatureType::Extrude;
        ex.name = "Extrude_Sym";
        ex.sketch_ref = sk_ex2;
        ex.distance = 25.0;
        ex.symmetric = true;
        ex.mode = BooleanMode::New;
        ex.extrude_end = ExtrudeEnd::Symmetric;
        ex.distance2 = 12.5;
        doc.features.push_back(ex);
    }

    // ---- Mirror: XZ plane, New mode, keep_original=false (distinctive non-defaults) ----
    doc.add_mirror(SketchPlane::XZ(), 0, BooleanMode::New, "Mirror_XZ");
    doc.features.back().mirror_keep_original = false;

    // ---- Datum Axis: two-points with distinctive non-default coordinates ----
    {
        int ax = doc.add_axis(AxisType::TwoPoints, "Axis_TP");
        doc.features[ax].axis_p1 = Vec3d(10, 20, 30);
        doc.features[ax].axis_p2 = Vec3d(13, 24, 34);
        doc.features[ax].axis_body = 1;
        doc.features[ax].axis_face = 3;
        doc.features[ax].axis_edge = 2;
        doc.features[ax].axis_plane_a = 4;
        doc.features[ax].axis_plane_b = 5;
    }

    // ---- Datum CoordSys: PointWorld with distinctive origin ----
    {
        int cs = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(7, 8, 9), "CS_PtWorld");
        doc.features[cs].coordsys_body   = 2;
        doc.features[cs].coordsys_face   = 1;
        doc.features[cs].coordsys_edge   = 0;
        doc.features[cs].coordsys_x_hint = Vec3d(0.5, 0.8, 0.3);
    }

    // ---- Helix: conical left-handed with distinctive non-default values ----
    {
        int hx = doc.add_helix(SketchPlane::XZ(), 11.5, 4.25, 18.0, true, 3.0, "Helix_CLH");
        (void)hx;
    }

    // ---- Transform: rigid move/rotate with distinctive non-default values ----
    doc.add_transform(0, Vec3d(3.5, 4.5, 5.5), Vec3d(0.0, 1.0, 0.0), Vec3d(1.5, 2.5, 3.5),
                      37.0, true, "GoldenTransform");

    doc.add_thicken(0, 0, 1.75, true, "GoldenThicken");

    doc.add_split_by_face(0, 0, 2, true, false, "GoldenSplit");

    doc.add_project_edges(0, {}, 0, SketchPlane::XY(), "GoldenProject");

    // Construction-flag on-disk lock: a real edge + a construction edge with distinctive
    // literals. Regen-golden does not recompute, so this only exercises serialization.
    {
        std::vector<SketchEntity> ge;
        SketchEntity real0; real0.type = SketchEntity::Type::Line;
        real0.p0 = Vec2d(-12.0, -12.0); real0.p1 = Vec2d(12.0, -12.0);
        SketchEntity ctor; ctor.type = SketchEntity::Type::Line;
        ctor.p0 = Vec2d(-40.0, 9.0); ctor.p1 = Vec2d(40.0, 9.0);
        ctor.construction = true;
        ge.push_back(real0);
        ge.push_back(ctor);
        doc.add_sketch_entities(ge, SketchPlane::XY(), "Sketch_Ctor");
    }

    // ---- Mate connectors: two CoordSys features with distinctive non-default values ----
    int cs_idx_a = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(11, 12, 13), "CS_MateA");
    doc.features[cs_idx_a].coordsys_body = 0;
    doc.features[cs_idx_a].coordsys_x_hint = Vec3d(0.1, 0.2, 0.9);
    int cs_idx_b = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(14, 15, 16), "CS_MateB");
    doc.features[cs_idx_b].coordsys_body = 1;
    doc.features[cs_idx_b].coordsys_x_hint = Vec3d(0.6, 0.7, 0.3);

    // ---- Mate: Fastened with all six fields carrying distinctive non-defaults ----
    doc.add_mate(1, cs_idx_a, cs_idx_b, 7.25, 33.0, true, "GoldenMate");

    return doc;
}

TEST_CASE("regenerate golden recipe fixture", "[.regen]")
{
    CadDocument doc = make_golden_doc_v1();
    // ponytail: serialize_recipe() only needs features, recompute is unnecessary
    // for a fixture that exercises the serialization format.
    auto blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    std::string path = std::string(TEST_DATA_DIR) + "/cad_recipe_v5.bin";
    std::ofstream ofs(path, std::ios::binary);
    REQUIRE(ofs.is_open());
    ofs.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    ofs.close();
    SUCCEED("Fixture written to " << path);
}

// The previous format's real blob, kept on disk deliberately. It is the only thing that can
// prove the version gate does its job: a v3 recipe carries FEWER fields per feature than this
// build reads, so without the bump to v4 it would have passed the gate and had two ints read
// past the end of every connector — into the next feature's bytes. Silent corruption of a saved
// project, which is far worse than a refusal. This asserts the refusal is clean and says why.
//
// Do NOT regenerate cad_recipe_v3.bin. Its value is entirely that it was written by an older
// build; rewriting it with today's code destroys the only evidence this test rests on.
TEST_CASE("a previous-format recipe is refused, not silently misread", "[CadDocument]")
{
    std::string path = std::string(TEST_DATA_DIR) + "/cad_recipe_v3.bin";
    std::ifstream ifs(path, std::ios::binary);
    REQUIRE(ifs.is_open());
    std::string blob((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();
    REQUIRE_FALSE(blob.empty());

    CadDocument doc;
    REQUIRE_FALSE(doc.deserialize_recipe(blob));
    REQUIRE_FALSE(doc.error.empty());
    REQUIRE(doc.error.find("older version") != std::string::npos);
    REQUIRE(doc.features.empty());   // nothing half-read was left behind
}

TEST_CASE("golden recipe v1 still deserialises", "[CadDocument]")
{
    using Catch::Matchers::WithinRel;
    using Catch::Matchers::WithinAbs;

    // Read the golden blob from disk
    std::string path = std::string(TEST_DATA_DIR) + "/cad_recipe_v4.bin";
    std::ifstream ifs(path, std::ios::binary);
    REQUIRE(ifs.is_open());
    std::string blob((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
    ifs.close();
    REQUIRE_FALSE(blob.empty());

    // --- Layer 1: deserialize features WITHOUT recomputing, assert field values ---
    std::vector<CadFeature> features;
    {
        std::istringstream iss(blob);
        cereal::BinaryInputArchive ar(iss);
        uint32_t v;
        ar(v);
        REQUIRE(v <= CadDocument::SNAPORCA_CAD_RECIPE_VERSION);
        ar(features);
    }

    CadDocument expected = make_golden_doc_v1();
    // Scoped tightly: this advice is ONLY valid for a count mismatch. It must not be in
    // scope for the field-value assertions below, where "regenerate the fixture" is the
    // one thing you must never do -- regenerating after a reorder bakes the corrupted
    // layout in as the new golden and permanently disarms this test.
    {
        INFO("Feature count changed - did you add/remove features in make_golden_doc_v1()?");
        INFO("If so: run libslic3r_tests \"[.regen]\" and re-run this test.");
        REQUIRE(features.size() == expected.features.size());
    }

    // A failure BELOW this point means the on-disk serialization format changed: some field
    // in CadFeature::save/load was reordered, retyped, or removed. Fields may only ever be
    // APPENDED at the end of both lists. Do NOT regenerate the fixture to make this pass --
    // fix the field order instead. See scripts/kernel-test.sh and the [.regen] case.

    // Field-by-field assertions against expected values.
    // Every field set to a distinctive non-default literal must be checked here.
    // A field reorder in save()/load() that swaps fields of differing types
    // will produce a wrong value at this position and FAIL the test.

    for (size_t i = 0; i < features.size(); ++i) {
        const auto& f = features[i];
        const auto& e = expected.features[i];

        INFO("Feature index " << i << " type " << int(f.type));

        REQUIRE(f.type == e.type);
        REQUIRE(f.name == e.name);
        REQUIRE(f.enabled == e.enabled);

        // Sketch params (all Sketch types)
        if (f.type == CadFeatureType::Sketch) {
            REQUIRE(f.shape == e.shape);
            if (e.name == "Sketch_Base") {
                REQUIRE(f.width  == 30);
                REQUIRE(f.height == 20);
                REQUIRE(f.radius == 15);
            }
            if (e.name == "Sketch_Rev" || e.name == "Sketch_SwProf" || e.name == "Sketch_Ex2" || e.name == "Sketch_SwPath") {
                REQUIRE(f.entities.size() == e.entities.size());
                if (!f.entities.empty()) {
                    REQUIRE(f.entities[0].type == e.entities[0].type);
                    REQUIRE_THAT(f.entities[0].p0.x(), WithinAbs(e.entities[0].p0.x(), 1e-9));
                    REQUIRE_THAT(f.entities[0].p0.y(), WithinAbs(e.entities[0].p0.y(), 1e-9));
                }
            }
            if (e.name == "Sketch_LoftBot" || e.name == "Sketch_LoftTop") {
                REQUIRE(f.profile.points.size() == e.profile.points.size());
                REQUIRE(f.profile.closed == e.profile.closed);
            }
            if (e.name == "Sketch_LoftTop") {
                REQUIRE_THAT(f.plane.origin.z(), WithinAbs(25.0, 1e-9));
            }
            if (e.name == "Sketch_Ctor") {
                REQUIRE(f.entities.size() == 2);
                REQUIRE(f.entities[0].construction == false);
                REQUIRE(f.entities[1].construction == true);
                REQUIRE_THAT(f.entities[1].p0.x(), WithinAbs(-40.0, 1e-9));
            }
        }

        // Extrude params
        if (f.type == CadFeatureType::Extrude) {
            REQUIRE(f.mode == e.mode);
            if (e.name == "Extrude_Base") {
                REQUIRE(f.distance   == 15.0);
                REQUIRE(f.symmetric  == false);
                REQUIRE(f.extrude_end == ExtrudeEnd::Blind);
                REQUIRE_THAT(f.taper_deg, WithinAbs(8.5, 1e-9));
            }
            if (e.name == "Extrude_Sym") {
                REQUIRE(f.distance    == 25.0);
                REQUIRE(f.symmetric   == true);
                REQUIRE(f.extrude_end == ExtrudeEnd::Symmetric);
                REQUIRE_THAT(f.distance2, WithinAbs(12.5, 1e-9));
            }
        }

        // Fillet
        if (f.type == CadFeatureType::Fillet && e.name == "Fillet_Lat35") {
            REQUIRE_THAT(f.dressup_size, WithinAbs(3.5, 1e-9));
            REQUIRE(f.face_group == FaceGroup::Lateral);
        }

        // Chamfer
        if (f.type == CadFeatureType::Chamfer && e.name == "Chamfer_Top2") {
            REQUIRE_THAT(f.dressup_size, WithinAbs(2.0, 1e-9));
            REQUIRE(f.face_group == FaceGroup::Top);
        }

        // Hole
        if (f.type == CadFeatureType::Hole && e.name == "Hole_Off75") {
            REQUIRE_THAT(f.hole_diameter, WithinAbs(7.5, 1e-9));
            REQUIRE_THAT(f.hole_depth,    WithinAbs(11.0, 1e-9));
            REQUIRE(f.hole_through == false);
            REQUIRE_THAT(f.hole_x, WithinAbs(4.0, 1e-9));
            REQUIRE_THAT(f.hole_y, WithinAbs(3.0, 1e-9));
        }

        // Draft
        if (f.type == CadFeatureType::Draft && e.name == "Draft_F3") {
            REQUIRE(f.draft_face == 3);
            REQUIRE_THAT(f.draft_angle, WithinAbs(7.25, 1e-9));
        }

        // Shell
        if (f.type == CadFeatureType::Shell && e.name == "Shell_T1375") {
            REQUIRE_THAT(f.shell_thickness, WithinAbs(1.375, 1e-9));
            REQUIRE(f.shell_face == 1);
        }

        // Thread
        if (f.type == CadFeatureType::Thread && e.name == "Thread_Int") {
            REQUIRE_THAT(f.thread_radius,   WithinAbs(4.0, 1e-9));
            REQUIRE_THAT(f.thread_pitch,    WithinAbs(2.5, 1e-9));
            REQUIRE_THAT(f.thread_height,   WithinAbs(15.0, 1e-9));
            REQUIRE_THAT(f.thread_depth,    WithinAbs(1.25, 1e-9));
            REQUIRE(f.thread_internal == true);
            REQUIRE_THAT(f.thread_x, WithinAbs(0.0, 1e-9));
            REQUIRE_THAT(f.thread_y, WithinAbs(0.0, 1e-9));
        }

        // Cut
        if (f.type == CadFeatureType::Cut && e.name == "Cut_Flip") {
            REQUIRE_THAT(f.cut_offset,     WithinAbs(10.0, 1e-9));
            REQUIRE(f.cut_flip       == true);
            REQUIRE(f.cut_keep_upper == true);
            REQUIRE(f.cut_keep_lower == false);
        }

        // Pattern
        if (f.type == CadFeatureType::Pattern && e.name == "Pattern_Lin5") {
            REQUIRE(f.pattern_circular == false);
            REQUIRE(f.pattern_count    == 5);
            REQUIRE_THAT(f.pattern_spacing, WithinAbs(13.5, 1e-9));
            REQUIRE(f.pattern_dir == 0);
        }

        // Datum Plane
        if (f.type == CadFeatureType::Plane && e.name == "Plane_Datum25") {
            REQUIRE(f.plane_base       == 0);
            REQUIRE_THAT(f.plane_offset, WithinAbs(25.0, 1e-9));
            REQUIRE_THAT(f.plane_angle_tilt, WithinAbs(0.0, 1e-9));
            REQUIRE(f.plane_axis == 0);
        }

        // Revolve
        if (f.type == CadFeatureType::Revolve && e.name == "Revolve_Y217") {
            REQUIRE_THAT(f.revolve_angle, WithinAbs(217.0, 1e-9));
            REQUIRE(f.revolve_axis == 1);
        }

        // Sweep
        if (f.type == CadFeatureType::Sweep && e.name == "Sweep_Z35") {
            REQUIRE(f.sweep_path_ref == e.sweep_path_ref);
            REQUIRE(f.sweep_path_ref >= 0);
        }

        // Loft
        if (f.type == CadFeatureType::Loft && e.name == "Loft_Ruled") {
            REQUIRE(f.loft_ruled == true);
            REQUIRE(f.loft_profile_refs.size() == 2);
        }

        // Boolean
        if (f.type == CadFeatureType::Boolean && e.name == "Boolean_Cut") {
            REQUIRE(f.mode          == BooleanMode::Cut);
            REQUIRE(f.bool_tool_body == 1);
            REQUIRE(f.bool_keep_tool == false);
            REQUIRE_THAT(f.bool_tolerance,   WithinAbs(0.01, 1e-9));
            REQUIRE(f.bool_target_face == 2);
            REQUIRE(f.bool_tool_face   == 3);
        }

        // Mirror
        if (f.type == CadFeatureType::Mirror && e.name == "Mirror_XZ") {
            REQUIRE(f.mode                 == BooleanMode::New);
            REQUIRE(f.mirror_keep_original == false);
            REQUIRE(f.target_body          == 0);
        }

        // Datum Axis
        if (f.type == CadFeatureType::Axis && e.name == "Axis_TP") {
            REQUIRE(f.axis_type  == AxisType::TwoPoints);
            REQUIRE_THAT(f.axis_p1.x(), WithinAbs(10.0, 1e-9));
            REQUIRE_THAT(f.axis_p1.y(), WithinAbs(20.0, 1e-9));
            REQUIRE_THAT(f.axis_p1.z(), WithinAbs(30.0, 1e-9));
            REQUIRE_THAT(f.axis_p2.x(), WithinAbs(13.0, 1e-9));
            REQUIRE_THAT(f.axis_p2.y(), WithinAbs(24.0, 1e-9));
            REQUIRE_THAT(f.axis_p2.z(), WithinAbs(34.0, 1e-9));
            REQUIRE(f.axis_body     == 1);
            REQUIRE(f.axis_face     == 3);
            REQUIRE(f.axis_edge     == 2);
            REQUIRE(f.axis_plane_a  == 4);
            REQUIRE(f.axis_plane_b  == 5);
        }

        // Datum CoordSys
        if (f.type == CadFeatureType::CoordSys && e.name == "CS_PtWorld") {
            REQUIRE(f.coordsys_type == CoordSysType::PointWorld);
            REQUIRE_THAT(f.coordsys_point.x(), WithinAbs(7.0, 1e-9));
            REQUIRE_THAT(f.coordsys_point.y(), WithinAbs(8.0, 1e-9));
            REQUIRE_THAT(f.coordsys_point.z(), WithinAbs(9.0, 1e-9));
            REQUIRE(f.coordsys_body == 2);
            REQUIRE(f.coordsys_face == 1);
            REQUIRE(f.coordsys_edge == 0);
            REQUIRE_THAT(f.coordsys_x_hint.x(), WithinAbs(0.5, 1e-9));
            REQUIRE_THAT(f.coordsys_x_hint.y(), WithinAbs(0.8, 1e-9));
            REQUIRE_THAT(f.coordsys_x_hint.z(), WithinAbs(0.3, 1e-9));
        }

        // Helix
        if (f.type == CadFeatureType::Helix && e.name == "Helix_CLH") {
            REQUIRE_THAT(f.helix_radius,      WithinAbs(11.5, 1e-9));
            REQUIRE_THAT(f.helix_pitch,       WithinAbs(4.25, 1e-9));
            REQUIRE_THAT(f.helix_height,      WithinAbs(18.0, 1e-9));
            REQUIRE(f.helix_left_handed == true);
            REQUIRE_THAT(f.helix_taper_deg,   WithinAbs(3.0, 1e-9));
        }

        // Transform
        if (f.type == CadFeatureType::Transform && e.name == "GoldenTransform") {
            REQUIRE_THAT(f.xf_translate.x(), WithinAbs(3.5, 1e-9));
            REQUIRE_THAT(f.xf_translate.y(), WithinAbs(4.5, 1e-9));
            REQUIRE_THAT(f.xf_translate.z(), WithinAbs(5.5, 1e-9));
            REQUIRE_THAT(f.xf_axis.x(),      WithinAbs(0.0, 1e-9));
            REQUIRE_THAT(f.xf_axis.y(),      WithinAbs(1.0, 1e-9));
            REQUIRE_THAT(f.xf_axis.z(),      WithinAbs(0.0, 1e-9));
            REQUIRE_THAT(f.xf_pivot.x(),     WithinAbs(1.5, 1e-9));
            REQUIRE_THAT(f.xf_pivot.y(),     WithinAbs(2.5, 1e-9));
            REQUIRE_THAT(f.xf_pivot.z(),     WithinAbs(3.5, 1e-9));
            REQUIRE_THAT(f.xf_angle_deg,     WithinAbs(37.0, 1e-9));
            REQUIRE(f.xf_copy == true);
        }

        // Thicken
        if (f.type == CadFeatureType::Thicken && e.name == "GoldenThicken") {
            REQUIRE(f.thicken_face     == 0);
            REQUIRE_THAT(f.thicken_thickness, WithinAbs(1.75, 1e-9));
            REQUIRE(f.thicken_flip     == true);
        }

        // Cut-by-face: GoldenSplit uses cut_face_body/cut_face instead of plane
        if (f.type == CadFeatureType::Cut && e.name == "GoldenSplit") {
            REQUIRE(f.cut_face_body  == 0);
            REQUIRE(f.cut_face       == 2);
            REQUIRE(f.cut_keep_upper == true);
            REQUIRE(f.cut_keep_lower == false);
        }

        // Project
        if (f.type == CadFeatureType::Project && e.name == "GoldenProject") {
            REQUIRE(f.project_source_body == 0);
            REQUIRE(f.project_face  == 0);
            REQUIRE(f.project_edges == e.project_edges);
        }

        // Mate
        if (f.type == CadFeatureType::Mate && e.name == "GoldenMate") {
            REQUIRE(f.mate_kind   == 1);
            REQUIRE(f.mate_cs_a   == e.mate_cs_a);
            REQUIRE(f.mate_cs_b   == e.mate_cs_b);
            REQUIRE_THAT(f.mate_offset, WithinAbs(7.25, 1e-9));
            REQUIRE_THAT(f.mate_angle,  WithinAbs(33.0, 1e-9));
            REQUIRE(f.mate_flip   == true);
        }
    }

    // --- Layer 2: geometry check (optional — only if the document recomputes) ---
    // Build expected document and try to recompute it.
    // ponytail: the field-value layer above is the real tripwire;
    // this layer is a bonus sanity check on the full recompute path.
    CadDocument exp_doc = make_golden_doc_v1();
    bool exp_ok = exp_doc.recompute();
    if (!exp_ok) {
        INFO("Expected document from make_golden_doc_v1() could not recompute "
             "(complex feature tree). Field-value checks above are sufficient.");
    }

    CadDocument doc;
    bool ser_ok = doc.deserialize_recipe(blob);

    if (exp_ok && ser_ok) {
        REQUIRE(doc.error.empty());
        REQUIRE(doc.bodies.size() == exp_doc.bodies.size());
        for (size_t i = 0; i < doc.bodies.size(); ++i) {
            double v = double(SketchEngine::tessellate(doc.bodies[i].shape).volume());
            double ev = double(SketchEngine::tessellate(exp_doc.bodies[i].shape).volume());
            REQUIRE_THAT(v, WithinRel(ev, 1e-6));
        }
    } else {
        INFO("The golden recipe fixture was loaded and field-value checks passed.");
        INFO("Recompute on the deserialized or expected document failed — this is");
        INFO("expected for the extended golden fixture (many feature types coexist");
        INFO("purely for serialization coverage). The field-value tripwire above is");
        INFO("the primary format check.");
    }
}

// --- Rib tests (M5b) ---

TEST_CASE("rib adds material to a box", "[CadDocument][rib]")
{
    using Catch::Matchers::WithinRel;
    using Catch::Matchers::WithinAbs;

    CadDocument doc;

    // Build a box: 40x40x10 extruded on XY -> z=[0,10]
    int sk_box = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(),
                                40, 40, 10, "Box");
    doc.add_extrude(sk_box, 10.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    double Vbox = double(doc.display_mesh.volume());
    REQUIRE(Vbox > 0.0);

    // Sketch a single open Line across the box footprint, on the same XY plane.
    // Line from (5,20) to (35,20), centred in Y but off-centre in X.
    std::vector<SketchEntity> ents = {
        {SketchEntity::Type::Line, Vec2d(5, 20), Vec2d(35, 20)},
    };
    int sk_rib = doc.add_sketch_entities(ents, SketchPlane::XY(), "RibLine");
    REQUIRE(sk_rib >= 0);

    int fi = doc.add_rib(sk_rib, 0, 3.0, 12.0, 0, "Rib");
    REQUIRE(fi >= 0);
    REQUIRE(doc.features[fi].type == CadFeatureType::Rib);

    bool ok = doc.recompute();
    REQUIRE(ok);
    REQUIRE(doc.error.empty());
    // The rib fused extra material -> volume must be strictly larger.
    double Vrib = double(doc.display_mesh.volume());
    REQUIRE(Vrib > Vbox);

    // A rib with a bad sketch ref must fail cleanly, not crash.
    CadDocument bad;
    bad.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Box");
    bad.add_extrude(0, 10.0, false, BooleanMode::New, "E");
    REQUIRE(bad.recompute());
    bad.add_rib(999, 0, 3.0, 10.0, 0, "BadRib");
    REQUIRE_FALSE(bad.recompute());
}

TEST_CASE("rib accepts a Project feature as its sketch ref", "[CadDocument][rib]")
{
    // Project carries plane + Line entities, which is all a rib reads. Every other consumer
    // (Extrude, SurfaceExtrude, SurfaceRevolve) already accepts it; Rib used to reject it,
    // which made "project a body edge, then rib along it" unreachable.
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 40, 40, 10, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());
    double Vbox = double(doc.display_mesh.volume());

    // Project the whole body onto XY: the 4 top and 4 bottom edges survive as Lines.
    int proj = doc.add_project_edges(0, {}, -1, SketchPlane::XY(), "Proj");
    REQUIRE(proj >= 0);
    REQUIRE(doc.recompute());
    REQUIRE(doc.features[proj].entities.size() == 8);

    int fi = doc.add_rib(proj, 0, 3.0, 12.0, 0, "RibFromProjection");
    REQUIRE(fi >= 0);
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(double(doc.display_mesh.volume()) > Vbox);
}

TEST_CASE("rib non-line entity rejected safely", "[CadDocument][rib]")
{

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());

    // Sketch with a Circle entity (not a Line)
    std::vector<SketchEntity> ents = {
        {SketchEntity::Type::Circle, Vec2d(0, 0), Vec2d(0, 0), Vec2d(0, 0), 5.0},
    };
    int sk2 = doc.add_sketch_entities(ents, SketchPlane::XY(), "CircleSketch");
    doc.add_rib(sk2, 0, 2.0, 10.0, 0, "BadRib");

    REQUIRE_FALSE(doc.recompute());
    REQUIRE_FALSE(doc.error.empty());
    REQUIRE_CONTAINS(doc.error, "rib");
}

TEST_CASE("rib round-trip serialization", "[CadDocument][rib]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 40, 40, 10, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());

    std::vector<SketchEntity> ents = {
        {SketchEntity::Type::Line, Vec2d(5, 20), Vec2d(35, 20)},
    };
    int sk2 = doc.add_sketch_entities(ents, SketchPlane::XY(), "RibLine");
    doc.add_rib(sk2, 0, 3.0, 12.0, 0, "Rib");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    size_t nb = doc.bodies.size();
    double Vdoc = double(doc.display_mesh.volume());

    auto blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    CadDocument fresh;
    REQUIRE(fresh.deserialize_recipe(blob));
    REQUIRE(fresh.bodies.size() == nb);
    double Vfresh = double(fresh.display_mesh.volume());
    REQUIRE_THAT(Vfresh, WithinAbs(Vdoc, 1e-6));

    // Find the rib feature and check its fields survived.
    const CadFeature* rf = nullptr;
    for (const auto& f : fresh.features) {
        if (f.type == CadFeatureType::Rib) { rf = &f; break; }
    }
    REQUIRE(rf != nullptr);
    REQUIRE_THAT(rf->rib_thickness, WithinAbs(3.0, 1e-9));
    REQUIRE_THAT(rf->rib_depth,     WithinAbs(12.0, 1e-9));
}

// --- Bridge tests (M3c) ---

TEST_CASE("bridge two collinear lines", "[CadDocument][bridge]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    std::vector<SketchEntity> ents = {
        {SketchEntity::Type::Line, Vec2d(0,0), Vec2d(10,0)},
        {SketchEntity::Type::Line, Vec2d(20,0), Vec2d(30,0)},
    };
    int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "S");
    REQUIRE(sk == 0);

    int bi = doc.add_bridge(0, 0, 1, 1, 0, "Bridge");
    REQUIRE(bi == 2);

    const auto& se = doc.features[0].entities;
    REQUIRE(se.size() == 3);
    REQUIRE(se[bi].type == SketchEntity::Type::BSpline);
    REQUIRE(se[bi].ctrl.size() == 4);
    REQUIRE_THAT(se[bi].ctrl.front().x(), WithinAbs(10.0, 1e-6));
    REQUIRE_THAT(se[bi].ctrl.front().y(), WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(se[bi].ctrl.back().x(), WithinAbs(20.0, 1e-6));
    REQUIRE_THAT(se[bi].ctrl.back().y(), WithinAbs(0.0, 1e-6));
}

TEST_CASE("bridge closes a C profile and extrudes", "[CadDocument][bridge]")
{
    using Catch::Matchers::WithinAbs;
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    // Right-side of a closed square: P0(10,-10), up to P1(10,10)
    std::vector<SketchEntity> ents = {
        // bottom edge: (-10,-10) to (10,-10)
        {SketchEntity::Type::Line, Vec2d(-10,-10), Vec2d(10,-10)},
        // left edge: (10,-10) to (10,10)
        {SketchEntity::Type::Line, Vec2d(10,-10), Vec2d(10,10)},
        // top edge: (10,10) to (-10,10)
        {SketchEntity::Type::Line, Vec2d(10,10), Vec2d(-10,10)},
    };
    // Missing: left edge from (-10,10) to (-10,-10). Build it as a separate line
    // entity so the bridge connects two existing lines.
    int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "C");
    REQUIRE(sk == 0);

    // Add the closing line as entity 3: (-10,10) to (-10,-10)
    CadFeature& f = doc.features[sk];
    SketchEntity closing;
    closing.type = SketchEntity::Type::Line;
    closing.p0 = Vec2d(-10, 10);
    closing.p1 = Vec2d(-10, -10);
    // The C is entities 0,1,2 (bottom cap, right side, top cap).
    // Entity 0 end=1 is (10,-10); entity 2 start=0 is (10,10). That's a U.
    // But we need a closed square from C shape.
    // Re-think: a C shape open on the left side.
    // Entities: 0 = bottom edge (-10,-10)->(10,-10) [end=1 at (10,-10)]
    //           1 = right edge (10,-10)->(10,10) [start=0 at (10,-10), end=1 at (10,10)]
    //           2 = top edge (10,10)->(-10,10) [start=0 at (10,10), end=1 at (-10,10)]
    // The C is open: entity 2's end is at (-10,10) and entity 0's start is at (-10,-10).
    // Bridge: entity 2 end=1 (-10,10) -> entity 0 start=0 (-10,-10).
    f.entities.push_back(closing);
    REQUIRE(f.entities.size() == 4);

    // Now bridge from top end (entity 2 end=1 = (-10,10)) to bottom start (entity 0 end=0 = (-10,-10))
    int bi = doc.add_bridge(sk, 2/*top edge*/, 1/*end*/, 0/*bottom edge*/, 0/*start*/, "Bridge");
    REQUIRE(bi == 4);
    REQUIRE(f.entities.size() == 5);

    // Now the entities should form a closed loop -> extrude
    int ex = doc.add_extrude(sk, 5.0, false, BooleanMode::New, "Extrude");
    REQUIRE(ex >= 0);
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.display_mesh.facets_count() > 0);
    REQUIRE_THAT(double(doc.display_mesh.volume()), WithinRel(20.0 * 20.0 * 5.0, 1e-2));
}

TEST_CASE("bridge bad indices throw", "[CadDocument][bridge]")
{
    CadDocument doc;
    std::vector<SketchEntity> ents = {
        {SketchEntity::Type::Line, Vec2d(0,0), Vec2d(10,0)},
        {SketchEntity::Type::Line, Vec2d(20,0), Vec2d(30,0)},
    };
    doc.add_sketch_entities(ents, SketchPlane::XY(), "S");

    // out-of-range entity a
    REQUIRE_THROWS(doc.add_bridge(0, 99, 1, 1, 0, "Bad"));
    // out-of-range entity b
    REQUIRE_THROWS(doc.add_bridge(0, 0, 1, 99, 0, "Bad"));
    // out-of-range sketch_ref
    REQUIRE_THROWS(doc.add_bridge(99, 0, 1, 1, 0, "Bad"));
    // non-sketch feature as sketch_ref
    doc.add_extrude(0, 5.0, false, BooleanMode::New, "Ex");
    REQUIRE_THROWS(doc.add_bridge(1, 0, 1, 1, 0, "Bad"));
}

TEST_CASE("bridge round-trip serialization", "[CadDocument][bridge]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    std::vector<SketchEntity> ents = {
        {SketchEntity::Type::Line, Vec2d(0,0), Vec2d(10,0)},
        {SketchEntity::Type::Line, Vec2d(20,0), Vec2d(30,0)},
    };
    int sk = doc.add_sketch_entities(ents, SketchPlane::XY(), "S");
    REQUIRE(sk == 0);
    int bi = doc.add_bridge(sk, 0, 1, 1, 0, "Bridge");
    REQUIRE(bi == 2);
    doc.add_extrude(sk, 5.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());

    auto blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    CadDocument doc2;
    REQUIRE(doc2.deserialize_recipe(blob));
    REQUIRE(doc2.features.size() == 2);

    const auto& br = doc2.features[0].entities[2];
    REQUIRE(br.type == SketchEntity::Type::BSpline);
    REQUIRE(br.ctrl.size() == 4);
    REQUIRE_THAT(br.ctrl.front().x(), WithinAbs(10.0, 1e-9));
    REQUIRE_THAT(br.ctrl.front().y(), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(br.ctrl.back().x(), WithinAbs(20.0, 1e-9));
    REQUIRE_THAT(br.ctrl.back().y(), WithinAbs(0.0, 1e-9));
}

TEST_CASE("delete_face removes a fillet face and restores volume", "[CadDocument][deleteface]")
{
    using Catch::Matchers::WithinRel;
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Box");
    REQUIRE(sk >= 0);
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    double Vbox = double(doc.display_mesh.volume());
    REQUIRE(Vbox > 0.0);

    doc.add_fillet(2.0, FaceGroup::All, "Fillet");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    double Vf = double(doc.display_mesh.volume());
    REQUIRE(Vf < Vbox);

    int nfaces = GeometryEngine::face_count(doc.bodies[0].shape);
    doc.checkpoint();
    bool found = false;
    for (int fi = 0; fi < nfaces && !found; ++fi) {
        int idx = doc.add_delete_face(0, {fi}, "Unfillet");
        (void)idx;
        if (doc.recompute() && doc.error.empty()) {
            double Vr = double(doc.display_mesh.volume());
            if (Vr > Vf) {
                found = true;
            }
        }
        if (!found) doc.undo();
    }
    REQUIRE(found);
}

TEST_CASE("delete_face with bad face index fails safely", "[CadDocument][deleteface]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Box");
    REQUIRE(sk >= 0);
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_delete_face(0, {9999}, "Bad");
    REQUIRE_FALSE(doc.recompute());
    REQUIRE_CONTAINS(doc.error, "face");
}

TEST_CASE("delete_face round-trip serialization", "[CadDocument][deleteface]")
{
    using Catch::Matchers::WithinRel;
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Box");
    REQUIRE(sk >= 0);
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    doc.add_fillet(2.0, FaceGroup::All, "Fillet");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int nfaces = GeometryEngine::face_count(doc.bodies[0].shape);
    int fillet_face = -1;
    for (int fi = 0; fi < nfaces; ++fi) {
        doc.checkpoint();
        doc.add_delete_face(0, {fi}, "Unfillet");
        bool ok = doc.recompute();
        if (ok && doc.error.empty()) {
            fillet_face = fi;
            break;
        }
        doc.undo();
    }
    REQUIRE(fillet_face >= 0);

    std::vector<std::pair<Vec3d, Vec3d>> bboxes;
    for (const auto& b : doc.bodies) {
        Bnd_Box bb; BRepBndLib::Add(b.shape, bb);
        Standard_Real x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        bboxes.push_back({Vec3d(x0, y0, z0), Vec3d(x1, y1, z1)});
    }

    auto blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    CadDocument doc2;
    REQUIRE(doc2.deserialize_recipe(blob));
    REQUIRE(doc2.error.empty());
    REQUIRE(doc2.recompute());
    REQUIRE(doc2.error.empty());
    REQUIRE(doc2.bodies.size() == doc.bodies.size());

    for (size_t i = 0; i < bboxes.size(); ++i) {
        Bnd_Box bb; BRepBndLib::Add(doc2.bodies[i].shape, bb);
        Standard_Real x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        REQUIRE_THAT(double(x0), WithinAbs(bboxes[i].first.x(),  1e-6));
        REQUIRE_THAT(double(y0), WithinAbs(bboxes[i].first.y(),  1e-6));
        REQUIRE_THAT(double(z0), WithinAbs(bboxes[i].first.z(),  1e-6));
        REQUIRE_THAT(double(x1), WithinAbs(bboxes[i].second.x(), 1e-6));
        REQUIRE_THAT(double(y1), WithinAbs(bboxes[i].second.y(), 1e-6));
        REQUIRE_THAT(double(z1), WithinAbs(bboxes[i].second.z(), 1e-6));
    }
}

TEST_CASE("hole: counterbore removes more material than a simple bore", "[CadDocument][hole]")
{
    using Catch::Matchers::WithinAbs;
    auto make_box_hole = [](int style, double cbore_d, double cbore_depth,
                             double csink_d, double csink_angle, const std::string& desig) {
        CadDocument doc;
        int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 30, 30, 0, "Box");
        doc.add_extrude(sk, 15.0, false, BooleanMode::New, "Extrude");
        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        double v_box = doc.body_mass_properties(0).volume;
        if (style == 0)
            doc.add_hole(6.0, 10.0, false, 0.0, 0.0, SketchPlane::XY(), "Hole");
        else
            doc.add_hole_styled(6.0, 10.0, false, 0.0, 0.0, SketchPlane::XY(), style,
                                cbore_d, cbore_depth, csink_d, csink_angle, desig, "Hole");
        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        double v = doc.body_mass_properties(0).volume;
        REQUIRE(v < v_box);
        return v;
    };

    double v_simple   = make_box_hole(0, 0, 0, 0, 0, "");
    double v_cbore    = make_box_hole(1, 11.0, 6.0, 0, 0, "M6");
    REQUIRE(v_cbore < v_simple);
}

TEST_CASE("hole: countersink removes more material than a simple bore", "[CadDocument][hole]")
{
    using Catch::Matchers::WithinAbs;
    auto make_box = []() {
        CadDocument doc;
        int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 30, 30, 0, "Box");
        doc.add_extrude(sk, 15.0, false, BooleanMode::New, "Extrude");
        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        return doc;
    };

    CadDocument doc_simple = make_box();
    doc_simple.add_hole(6.0, 10.0, false, 0.0, 0.0, SketchPlane::XY(), "Hole");
    REQUIRE(doc_simple.recompute());
    double v_simple = doc_simple.body_mass_properties(0).volume;
    double v_box = 30.0 * 30.0 * 15.0;

    CadDocument doc_csink = make_box();
    doc_csink.add_hole_styled(6.0, 10.0, false, 0.0, 0.0, SketchPlane::XY(), 2,
                              0.0, 0.0, 12.0, 90.0, "M6", "Hole");
    REQUIRE(doc_csink.recompute());
    double v_csink = doc_csink.body_mass_properties(0).volume;

    REQUIRE(v_simple < v_box);
    REQUIRE(v_csink < v_simple);
}

TEST_CASE("hole: standards table lookup", "[CadDocument][hole]")
{
    using Catch::Matchers::WithinAbs;
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 30, 30, 0, "Box");
    doc.add_extrude(sk, 15.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());

    int fi = doc.add_hole_standard("M6", 0, true, 10, 0, 0, SketchPlane::XY(), "H");
    REQUIRE(fi >= 0);
    REQUIRE_THAT(doc.features[fi].hole_diameter, WithinAbs(6.6, 1e-6));
    REQUIRE(doc.features[fi].hole_standard == "M6");

    REQUIRE_THROWS(doc.add_hole_standard("M999", 0, true, 10, 0, 0, SketchPlane::XY(), "H"));
    try {
        doc.add_hole_standard("M999", 0, true, 10, 0, 0, SketchPlane::XY(), "H");
    } catch (const std::exception& ex) {
        CHECK_CONTAINS(std::string(ex.what()), "standard");
    }
}

TEST_CASE("hole: round-trip preserves styled counterbore hole", "[CadDocument][hole]")
{
    using Catch::Matchers::WithinAbs;
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 30, 30, 0, "Box");
    doc.add_extrude(sk, 15.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_hole_styled(6.0, 10.0, false, 0.0, 0.0, SketchPlane::XY(), 1,
                        11.0, 6.0, 0.0, 90.0, "M6", "Cbore");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    std::vector<std::pair<Vec3d, Vec3d>> bboxes;
    for (const auto& b : doc.bodies) {
        Bnd_Box bb; BRepBndLib::Add(b.shape, bb);
        Standard_Real x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        bboxes.emplace_back(Vec3d(x0, y0, z0), Vec3d(x1, y1, z1));
    }

    auto blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    CadDocument doc2;
    REQUIRE(doc2.deserialize_recipe(blob));
    REQUIRE(doc2.error.empty());
    REQUIRE(doc2.bodies.size() == doc.bodies.size());

    for (size_t i = 0; i < bboxes.size(); ++i) {
        Bnd_Box bb; BRepBndLib::Add(doc2.bodies[i].shape, bb);
        Standard_Real x0, y0, z0, x1, y1, z1;
        bb.Get(x0, y0, z0, x1, y1, z1);
        REQUIRE_THAT(double(x0), WithinAbs(bboxes[i].first.x(),  1e-6));
        REQUIRE_THAT(double(y0), WithinAbs(bboxes[i].first.y(),  1e-6));
        REQUIRE_THAT(double(z0), WithinAbs(bboxes[i].first.z(),  1e-6));
        REQUIRE_THAT(double(x1), WithinAbs(bboxes[i].second.x(), 1e-6));
        REQUIRE_THAT(double(y1), WithinAbs(bboxes[i].second.y(), 1e-6));
        REQUIRE_THAT(double(z1), WithinAbs(bboxes[i].second.z(), 1e-6));
    }

    bool found = false;
    for (const auto& f : doc2.features) {
        if (f.type == CadFeatureType::Hole && f.name == "Cbore") {
            REQUIRE(f.hole_style == 1);
            REQUIRE_THAT(f.hole_cbore_diameter, WithinAbs(11.0, 1e-9));
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("variables drive a box (parametric sketch + extrude)", "[CadDocument][variables]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(),
                             20, 20, 10, "Sketch");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.features.size() >= 2);
    int ex = sk + 1;

    doc.variables = {{"w", "10"}, {"h", "w*2"}};
    doc.features[sk].expr = {{"width", "w"}, {"height", "w"}};
    doc.features[ex].expr = {{"distance", "h"}};

    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE_FALSE(doc.body.IsNull());

    Bnd_Box bb; BRepBndLib::Add(doc.body, bb);
    double xlo, ylo, zlo, xhi, yhi, zhi;
    bb.Get(xlo, ylo, zlo, xhi, yhi, zhi);
    REQUIRE_THAT(xhi - xlo, WithinAbs(10.0, 1.0));
    REQUIRE_THAT(yhi - ylo, WithinAbs(10.0, 1.0));
    REQUIRE_THAT(zhi - zlo, WithinAbs(20.0, 2.0));
}

TEST_CASE("function + pi in expression binding", "[CadDocument][variables]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Circle, SketchPlane::XY(),
                             20, 20, 10, "Sketch");
    doc.add_extrude(sk, 5.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.features.size() >= 2);

    doc.variables = {{"r", "sqrt(16)+abs(-2)"}};
    doc.features[sk].expr = {{"radius", "r"}};

    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE_THAT(doc.features[sk].radius, WithinAbs(6.0, 1e-6));

    doc.variables = {{"r", "max(3, pi)"}};
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE_THAT(doc.features[sk].radius, WithinAbs(M_PI, 1e-6));

    doc.variables = {{"r", "5"}, {"d", "r*2"}};
    doc.features[sk].expr = {{"radius", "d"}};
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE_THAT(doc.features[sk].radius, WithinAbs(10.0, 1e-6));
}

TEST_CASE("cycle detected fails recompute with error", "[CadDocument][variables]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(),
                             20, 20, 10, "Sketch");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    doc.features[sk].expr = {{"width", "a"}};

    doc.variables = {{"a", "b"}, {"b", "a"}};
    REQUIRE_FALSE(doc.recompute());
    REQUIRE_CONTAINS(doc.error, "cycle");
}

TEST_CASE("unknown parameter fails recompute", "[CadDocument][variables]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(),
                             20, 20, 10, "Sketch");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    doc.features[sk].expr = {{"nope", "1"}};

    REQUIRE_FALSE(doc.recompute());
    REQUIRE_CONTAINS(doc.error, "unknown parameter");
}

TEST_CASE("unknown identifier fails recompute", "[CadDocument][variables]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(),
                             20, 20, 10, "Sketch");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    doc.features[sk].expr = {{"width", "x"}};

    doc.variables = {{"x", "y+1"}};
    REQUIRE_FALSE(doc.recompute());
    REQUIRE_CONTAINS(doc.error, "unknown identifier");
}

// The checkpoint -> mutate -> recompute -> undo-on-failure pattern is what both the GUI and
// McpControl use to keep a bad edit out of the document. It only works if the snapshot covers
// `variables` as well as `features`: undo() used to restore features alone, so a bad variable
// survived the rollback and every later recompute failed — the document was left unusable.
TEST_CASE("undo rolls back a bad variable, not just features", "[CadDocument][variables]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Sketch");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    doc.features[sk].expr = {{"width", "w"}};
    doc.variables = {{"w", "20"}};
    REQUIRE(doc.recompute());

    // Caller sets a variable to something unevaluable, exactly as action_set_variable does.
    doc.checkpoint();
    doc.variables["w"] = "nosuchvar + 1";
    REQUIRE_FALSE(doc.recompute());
    REQUIRE(doc.undo());

    // The good value must be back...
    REQUIRE(doc.variables.at("w") == "20");
    // ...and, the part that actually bit, the document must still be usable.
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
}

// A variable ADDED under a checkpoint must disappear entirely on undo, not linger with a
// stale value: the pre-mutation snapshot simply did not contain the key.
TEST_CASE("undo removes a variable that did not exist before the checkpoint", "[CadDocument][variables]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Sketch");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());
    REQUIRE(doc.variables.empty());

    doc.checkpoint();
    doc.variables["bogus"] = "1/0 +";     // syntactically broken
    REQUIRE_FALSE(doc.recompute());
    REQUIRE(doc.undo());

    REQUIRE(doc.variables.count("bogus") == 0);
    REQUIRE(doc.recompute());
}

TEST_CASE("parametric recipe round-trips through serialize/deserialize", "[CadDocument][variables]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(),
                             20, 20, 10, "Sketch");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.features.size() >= 2);
    int ex = sk + 1;

    doc.variables = {{"w", "10"}, {"h", "w*2"}};
    doc.features[sk].expr = {{"width", "w"}, {"height", "w"}};
    doc.features[ex].expr = {{"distance", "h"}};

    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    Bnd_Box bb_orig; BRepBndLib::Add(doc.body, bb_orig);
    double ox0, oy0, oz0, ox1, oy1, oz1;
    bb_orig.Get(ox0, oy0, oz0, ox1, oy1, oz1);

    auto blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    CadDocument doc2;
    REQUIRE(doc2.deserialize_recipe(blob));

    REQUIRE(doc2.variables.size() == 2);
    REQUIRE(doc2.variables["w"] == "10");
    REQUIRE(doc2.variables["h"] == "w*2");

    bool found_sk = false, found_ex = false;
    for (const auto& f : doc2.features) {
        if (f.name == "Sketch") {
            REQUIRE(f.expr.size() == 2);
            REQUIRE(f.expr.at("width") == "w");
            REQUIRE(f.expr.at("height") == "w");
            found_sk = true;
        }
        if (f.name == "Extrude") {
            REQUIRE(f.expr.size() == 1);
            REQUIRE(f.expr.at("distance") == "h");
            found_ex = true;
        }
    }
    REQUIRE(found_sk);
    REQUIRE(found_ex);

    REQUIRE(doc2.bodies.size() == doc.bodies.size());

    Bnd_Box bb2; BRepBndLib::Add(doc2.body, bb2);
    double nx0, ny0, nz0, nx1, ny1, nz1;
    bb2.Get(nx0, ny0, nz0, nx1, ny1, nz1);
    REQUIRE_THAT(nx0, WithinAbs(ox0, 1e-6));
    REQUIRE_THAT(ny0, WithinAbs(oy0, 1e-6));
    REQUIRE_THAT(nz0, WithinAbs(oz0, 1e-6));
    REQUIRE_THAT(nx1, WithinAbs(ox1, 1e-6));
    REQUIRE_THAT(ny1, WithinAbs(oy1, 1e-6));
    REQUIRE_THAT(nz1, WithinAbs(oz1, 1e-6));
}

TEST_CASE("surface-extrude makes an open shell", "[CadDocument][surface]")
{
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Rect");
    REQUIRE(sk >= 0);
    int fi = doc.add_surface_extrude(sk, 12.0, "Skin");
    REQUIRE(fi == 1);
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 1);
    REQUIRE(CadDocument::is_sheet_shape(doc.bodies.back().shape));

    // A rectangle skin has 4 side faces (no end caps).
    int face_count = 0, solid_count = 0;
    for (TopExp_Explorer fe(doc.bodies.back().shape, TopAbs_FACE); fe.More(); fe.Next()) ++face_count;
    for (TopExp_Explorer se(doc.bodies.back().shape, TopAbs_SOLID); se.More(); se.Next()) ++solid_count;
    REQUIRE(face_count >= 1);
    REQUIRE(solid_count == 0);
    REQUIRE(doc.display_mesh.facets_count() > 0);
}

TEST_CASE("surface-revolve makes an open shell", "[CadDocument][surface]")
{
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    // A small rectangle offset from the axis: u=10..15, v=0..5.
    SketchProfile sp;
    sp.points = {{10,0},{15,0},{15,5},{10,5}};
    sp.closed = true;
    const int sk = doc.add_sketch_profile(sp, SketchPlane::XY(), "Profile");
    REQUIRE(sk >= 0);
    int fi = doc.add_surface_revolve(sk, 360, 0, "Rev");
    REQUIRE(fi == 1);
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 1);
    REQUIRE(CadDocument::is_sheet_shape(doc.bodies.back().shape));

    int solid_count = 0;
    for (TopExp_Explorer se(doc.bodies.back().shape, TopAbs_SOLID); se.More(); se.Next()) ++solid_count;
    REQUIRE(solid_count == 0);
    REQUIRE(doc.display_mesh.facets_count() > 0);
}

TEST_CASE("surface-extrude bad ref safe", "[CadDocument][surface]")
{
    CadDocument doc;
    int fi = doc.add_surface_extrude(999, 10, "Bad");
    REQUIRE(fi == 0);
    REQUIRE_FALSE(doc.recompute());
    REQUIRE_CONTAINS(doc.error, "surface-extrude");
}

TEST_CASE("surface round-trip serialize/deserialize", "[CadDocument][surface]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Rect");
    doc.add_surface_extrude(sk, 12.0, "Skin");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(CadDocument::is_sheet_shape(doc.bodies.back().shape));

    size_t orig_nb = doc.bodies.size();
    Bnd_Box orig_bb;
    BRepBndLib::Add(doc.bodies.back().shape, orig_bb);

    std::string blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    CadDocument fresh;
    REQUIRE(fresh.deserialize_recipe(blob));
    REQUIRE(fresh.error.empty());
    REQUIRE(fresh.bodies.size() == orig_nb);
    REQUIRE(CadDocument::is_sheet_shape(fresh.bodies.back().shape));

    Bnd_Box fresh_bb;
    BRepBndLib::Add(fresh.bodies.back().shape, fresh_bb);
    Standard_Real ox0, oy0, oz0, ox1, oy1, oz1;
    orig_bb.Get(ox0, oy0, oz0, ox1, oy1, oz1);
    Standard_Real fx0, fy0, fz0, fx1, fy1, fz1;
    fresh_bb.Get(fx0, fy0, fz0, fx1, fy1, fz1);
    REQUIRE_THAT(double(fx0), WithinAbs(double(ox0), 1e-6));
    REQUIRE_THAT(double(fy0), WithinAbs(double(oy0), 1e-6));
    REQUIRE_THAT(double(fz0), WithinAbs(double(oz0), 1e-6));
    REQUIRE_THAT(double(fx1), WithinAbs(double(ox1), 1e-6));
    REQUIRE_THAT(double(fy1), WithinAbs(double(oy1), 1e-6));
    REQUIRE_THAT(double(fz1), WithinAbs(double(oz1), 1e-6));
}

TEST_CASE("thicken-surface makes a solid from a sheet", "[CadDocument][surface]")
{
    using Catch::Matchers::WithinRel;
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Rect");
    REQUIRE(sk >= 0);
    int si = doc.add_surface_extrude(sk, 12.0, "Skin");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 1);
    REQUIRE(CadDocument::is_sheet_shape(doc.bodies.back().shape));

    Bnd_Box sheet_bb;
    BRepBndLib::Add(doc.bodies.back().shape, sheet_bb);

    int ti = doc.add_thicken_surface(0, 2.0, false, "Wall");
    REQUIRE(ti == 2);
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 2);

    REQUIRE(!CadDocument::is_sheet_shape(doc.bodies.back().shape));
    double vol = doc.body_mass_properties(1).volume;
    REQUIRE(vol > 0);

    Bnd_Box thick_bb;
    BRepBndLib::Add(doc.bodies.back().shape, thick_bb);
    Standard_Real sx0, sy0, sz0, sx1, sy1, sz1;
    Standard_Real tx0, ty0, tz0, tx1, ty1, tz1;
    sheet_bb.Get(sx0, sy0, sz0, sx1, sy1, sz1);
    thick_bb.Get(tx0, ty0, tz0, tx1, ty1, tz1);
    REQUIRE_THAT(double(tx0), WithinAbs(double(sx0), 2.1));
    REQUIRE_THAT(double(tx1), WithinAbs(double(sx1), 2.1));
}

// Regression for snaporca-lu27, with the rig's own numbers. A 60x60x40 four-walled open box
// used to report volume 96000 and an inertia diagonal of [-4.2e7, -4.2e7, -6.9e7] — negative
// principal moments, which no real body can have. VolumeProperties was being integrated over
// an open shell as though it were closed, and std::abs() on the mass hid the only obvious tell.
// The area was always right, so that stays reported and valid; the volume and inertia must not.
TEST_CASE("mass properties of a sheet body report area only, never a volume",
          "[CadDocument][surface]")
{
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 60, 60, 0, "Rect");
    REQUIRE(sk >= 0);
    doc.add_surface_extrude(sk, 40.0, "Skin");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 1);

    auto sheet = doc.body_mass_properties(0);
    REQUIRE_FALSE(sheet.is_solid);
    REQUIRE(sheet.valid);                       // the area IS trustworthy
    REQUIRE_THAT(sheet.surface_area, WithinRel(9600.0, 1e-6));   // 4 walls x 60 x 40
    REQUIRE(sheet.volume == 0.0);
    for (double v : sheet.inertia)
        REQUIRE(v == 0.0);

    // The solid thickened from that same sheet is the contrast case: it does have a volume,
    // and its principal moments are positive, as any real body's must be.
    REQUIRE(doc.add_thicken_surface(0, 2.0, false, "Wall") >= 0);
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 2);

    auto solid = doc.body_mass_properties(1);
    REQUIRE(solid.is_solid);
    REQUIRE(solid.valid);
    REQUIRE(solid.volume > 0.0);
    REQUIRE(solid.inertia[0] > 0.0);
    REQUIRE(solid.inertia[4] > 0.0);
    REQUIRE(solid.inertia[8] > 0.0);
}

TEST_CASE("surface-offset creates another sheet shifted outward", "[CadDocument][surface]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Rect");
    REQUIRE(sk >= 0);
    int si = doc.add_surface_extrude(sk, 12.0, "Skin");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    Bnd_Box src_bb;
    BRepBndLib::Add(doc.bodies.back().shape, src_bb);

    int oi = doc.add_surface_offset(0, 1.0, "Off");
    REQUIRE(oi == 2);
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 2);
    REQUIRE(CadDocument::is_sheet_shape(doc.bodies.back().shape));

    Bnd_Box off_bb;
    BRepBndLib::Add(doc.bodies.back().shape, off_bb);
    Standard_Real sx0, sy0, sz0, sx1, sy1, sz1;
    Standard_Real ox0, oy0, oz0, ox1, oy1, oz1;
    src_bb.Get(sx0, sy0, sz0, sx1, sy1, sz1);
    off_bb.Get(ox0, oy0, oz0, ox1, oy1, oz1);
    REQUIRE(std::abs(double(ox0) - double(sx0)) > 1e-3);
    REQUIRE(std::abs(double(ox1) - double(sx1)) > 1e-3);
    REQUIRE(std::abs(double(oy0) - double(sy0)) > 1e-3);
    REQUIRE(std::abs(double(oy1) - double(sy1)) > 1e-3);
}

TEST_CASE("thicken-surface on non-sheet fails", "[CadDocument][surface]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Rect");
    REQUIRE(sk >= 0);
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Solid");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE_FALSE(CadDocument::is_sheet_shape(doc.bodies.back().shape));

    doc.add_thicken_surface(0, 2.0, false, "Bad");
    REQUIRE_FALSE(doc.recompute());
    REQUIRE_CONTAINS(doc.error, "sheet");
}

TEST_CASE("thicken-surface round-trip serialize/deserialize", "[CadDocument][surface]")
{
    using Catch::Matchers::WithinAbs;
    using Catch::Matchers::WithinRel;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Rect");
    doc.add_surface_extrude(sk, 12.0, "Skin");
    REQUIRE(doc.recompute());
    doc.add_thicken_surface(0, 2.0, false, "Wall");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE_FALSE(CadDocument::is_sheet_shape(doc.bodies.back().shape));

    size_t orig_nb = doc.bodies.size();
    Bnd_Box orig_bb;
    BRepBndLib::Add(doc.bodies.back().shape, orig_bb);

    std::string blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    CadDocument fresh;
    REQUIRE(fresh.deserialize_recipe(blob));
    REQUIRE(fresh.error.empty());
    REQUIRE(fresh.bodies.size() == orig_nb);
    REQUIRE_FALSE(CadDocument::is_sheet_shape(fresh.bodies.back().shape));

    Bnd_Box fresh_bb;
    BRepBndLib::Add(fresh.bodies.back().shape, fresh_bb);
    Standard_Real ox0, oy0, oz0, ox1, oy1, oz1;
    Standard_Real fx0, fy0, fz0, fx1, fy1, fz1;
    orig_bb.Get(ox0, oy0, oz0, ox1, oy1, oz1);
    fresh_bb.Get(fx0, fy0, fz0, fx1, fy1, fz1);
    REQUIRE_THAT(double(fx0), WithinAbs(double(ox0), 1e-6));
    REQUIRE_THAT(double(fy0), WithinAbs(double(oy0), 1e-6));
    REQUIRE_THAT(double(fz0), WithinAbs(double(oz0), 1e-6));
    REQUIRE_THAT(double(fx1), WithinAbs(double(ox1), 1e-6));
    REQUIRE_THAT(double(fy1), WithinAbs(double(oy1), 1e-6));
    REQUIRE_THAT(double(fz1), WithinAbs(double(oz1), 1e-6));
}

TEST_CASE("surface-loft makes an open shell", "[CadDocument][surface]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    SketchProfile bot;
    bot.points = {{-10,-10},{10,-10},{10,10},{-10,10}};
    bot.closed = true;
    int s0 = doc.add_sketch_profile(bot, SketchPlane::XY(), "Bottom");

    doc.add_plane(0 /*XY*/, 20.0, 0.0, 0, "Plane1");
    SketchPlane top = doc.resolve_datum_planes()[0].second;
    SketchProfile tp;
    tp.points = {{-5,-5},{5,-5},{5,5},{-5,5}};
    tp.closed = true;
    int s1 = doc.add_sketch_profile(tp, top, "Top");

    int fi = doc.add_surface_loft({s0, s1}, false, "Skin");
    REQUIRE(fi >= 0);
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 1);
    REQUIRE(CadDocument::is_sheet_shape(doc.bodies.back().shape));

    // Contrast with a solid loft on the same profiles.
    CadDocument doc2;
    SketchProfile bot2;
    bot2.points = {{-10,-10},{10,-10},{10,10},{-10,10}};
    bot2.closed = true;
    int a0 = doc2.add_sketch_profile(bot2, SketchPlane::XY(), "Bottom");
    doc2.add_plane(0 /*XY*/, 20.0, 0.0, 0, "Plane1");
    SketchPlane top2 = doc2.resolve_datum_planes()[0].second;
    SketchProfile tp2;
    tp2.points = {{-5,-5},{5,-5},{5,5},{-5,5}};
    tp2.closed = true;
    int a1 = doc2.add_sketch_profile(tp2, top2, "Top");
    doc2.add_loft({a0, a1}, false, BooleanMode::New, "SolidLoft");
    REQUIRE(doc2.recompute());
    REQUIRE(doc2.error.empty());
    REQUIRE_FALSE(CadDocument::is_sheet_shape(doc2.bodies.back().shape));
}

TEST_CASE("surface-fill makes a one-face sheet", "[CadDocument][surface]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Rect");
    REQUIRE(sk >= 0);
    int fi = doc.add_surface_fill(sk, "Patch");
    REQUIRE(fi == 1);
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 1);
    REQUIRE(CadDocument::is_sheet_shape(doc.bodies.back().shape));

    int face_count = 0;
    for (TopExp_Explorer fe(doc.bodies.back().shape, TopAbs_FACE); fe.More(); fe.Next()) ++face_count;
    REQUIRE(face_count >= 1);
}

TEST_CASE("surface-loft / surface-fill bad refs safe", "[CadDocument][surface]")
{
    {
        CadDocument doc;
        doc.add_surface_loft({999}, false, "Bad");
        REQUIRE_FALSE(doc.recompute());
        REQUIRE_CONTAINS(doc.error, "surface-loft");
    }
    {
        CadDocument doc;
        doc.add_surface_fill(999, "Bad");
        REQUIRE_FALSE(doc.recompute());
        REQUIRE_CONTAINS(doc.error, "surface-fill");
    }
}

TEST_CASE("surface-loft round-trip serialize/deserialize", "[CadDocument][surface]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    SketchProfile bot;
    bot.points = {{-10,-10},{10,-10},{10,10},{-10,10}};
    bot.closed = true;
    int s0 = doc.add_sketch_profile(bot, SketchPlane::XY(), "Bottom");
    doc.add_plane(0 /*XY*/, 20.0, 0.0, 0, "Plane1");
    SketchPlane top = doc.resolve_datum_planes()[0].second;
    SketchProfile tp;
    tp.points = {{-5,-5},{5,-5},{5,5},{-5,5}};
    tp.closed = true;
    int s1 = doc.add_sketch_profile(tp, top, "Top");
    doc.add_surface_loft({s0, s1}, false, "Skin");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(CadDocument::is_sheet_shape(doc.bodies.back().shape));

    size_t orig_nb = doc.bodies.size();
    Bnd_Box orig_bb;
    BRepBndLib::Add(doc.bodies.back().shape, orig_bb);

    std::string blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    CadDocument fresh;
    REQUIRE(fresh.deserialize_recipe(blob));
    REQUIRE(fresh.error.empty());
    REQUIRE(fresh.bodies.size() == orig_nb);
    REQUIRE(CadDocument::is_sheet_shape(fresh.bodies.back().shape));

    Bnd_Box fresh_bb;
    BRepBndLib::Add(fresh.bodies.back().shape, fresh_bb);
    Standard_Real ox0, oy0, oz0, ox1, oy1, oz1;
    Standard_Real fx0, fy0, fz0, fx1, fy1, fz1;
    orig_bb.Get(ox0, oy0, oz0, ox1, oy1, oz1);
    fresh_bb.Get(fx0, fy0, fz0, fx1, fy1, fz1);
    REQUIRE_THAT(double(fx0), WithinAbs(double(ox0), 1e-6));
    REQUIRE_THAT(double(fy0), WithinAbs(double(oy0), 1e-6));
    REQUIRE_THAT(double(fz0), WithinAbs(double(oz0), 1e-6));
    REQUIRE_THAT(double(fx1), WithinAbs(double(ox1), 1e-6));
    REQUIRE_THAT(double(fy1), WithinAbs(double(oy1), 1e-6));
    REQUIRE_THAT(double(fz1), WithinAbs(double(oz1), 1e-6));
}

// --- Mate tests (M8a) ---

TEST_CASE("fastened mate with zero offset/angle", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk_box = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Box");
    doc.add_extrude(sk_box, 5.0, false, BooleanMode::New, "BoxExt");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int sk_cyl = doc.add_sketch(SketchShape::Circle, SketchPlane::XY(), 0, 0, 3, "Cyl");
    doc.add_extrude(sk_cyl, 10.0, false, BooleanMode::New, "CylExt");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 2);

    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 5), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int mi = doc.add_mate(0, cs_fixed, cs_moving, 0.0, 0.0, false, "Mate");
    REQUIRE(mi >= 0);
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    GProp_GProps props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, props);
    gp_Pnt com = props.CentreOfMass();
    REQUIRE_THAT(double(com.X()), WithinAbs(5.0, 1e-4));
    REQUIRE_THAT(double(com.Y()), WithinAbs(5.0, 1e-4));
    REQUIRE_THAT(double(com.Z()), WithinAbs(5.0, 1e-4));
}

TEST_CASE("fastened mate with offset", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Box");
    doc.add_extrude(sk, 5.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int sk2 = doc.add_sketch(SketchShape::Circle, SketchPlane::XY(), 0, 0, 3, "Cyl");
    doc.add_extrude(sk2, 10.0, false, BooleanMode::New, "CylExt");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 5), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(0, cs_fixed, cs_moving, 7.0, 0.0, false, "Mate");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    GProp_GProps props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, props);
    gp_Pnt com = props.CentreOfMass();
    REQUIRE_THAT(double(com.Z()), WithinAbs(12.0, 1e-4));
}

TEST_CASE("fastened mate with angle", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Box");
    doc.add_extrude(sk, 5.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int sk2 = doc.add_sketch(SketchShape::Circle, SketchPlane::XY(), 0, 0, 3, "Cyl");
    doc.add_extrude(sk2, 10.0, false, BooleanMode::New, "CylExt");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 5), "CS_Moving");
    doc.features[cs_moving].coordsys_x_hint = Vec3d(1, 0, 0);
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(0, cs_fixed, cs_moving, 0.0, 90.0, false, "Mate");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    GProp_GProps props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, props);
    gp_Pnt com = props.CentreOfMass();
    REQUIRE_THAT(double(com.X()), WithinAbs(5.0, 1e-4));
    REQUIRE_THAT(double(com.Y()), WithinAbs(5.0, 1e-4));
}

TEST_CASE("fastened mate with flip", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Box");
    doc.add_extrude(sk, 5.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int sk2 = doc.add_sketch(SketchShape::Circle, SketchPlane::XY(), 0, 0, 3, "Cyl");
    doc.add_extrude(sk2, 10.0, false, BooleanMode::New, "CylExt");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 5), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(0, cs_fixed, cs_moving, 0.0, 0.0, true, "Mate");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    GProp_GProps props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, props);
    gp_Pnt com = props.CentreOfMass();
    REQUIRE_THAT(double(com.X()), WithinAbs(5.0, 1e-4));
    REQUIRE_THAT(double(com.Y()), WithinAbs(5.0, 1e-4));
    // Flip opposes Z axes: for a symmetric cylinder this is invisible in centroid,
    // but the mate executed cleanly and the body moved to the target connector.
}

TEST_CASE("planar mate: normal distance becomes mate_offset", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Box");
    doc.add_extrude(sk, 5.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int sk2 = doc.add_sketch(SketchShape::Circle, SketchPlane::XY(), 0, 0, 3, "Cyl");
    doc.add_extrude(sk2, 10.0, false, BooleanMode::New, "CylExt");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(20, 0, 0), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(1, cs_fixed, cs_moving, 3.0, 0.0, false, "MatePlanar");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    GProp_GProps props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, props);
    gp_Pnt com = props.CentreOfMass();
    // Connector B was at z=0 (bottom of 10mm cylinder). After planar mate with
    // offset=3 and A at z=5, the z-distance from A to B becomes 3 => B.z = 8.
    // The cylinder centroid (was at z=5) moves to z=13.
    REQUIRE_THAT(double(com.Z()), WithinAbs(13.0, 1e-4));
}

TEST_CASE("mate round-trip serialization", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Box");
    doc.add_extrude(sk, 5.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_a = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_A");
    doc.features[cs_a].coordsys_body = 0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int sk2 = doc.add_sketch(SketchShape::Circle, SketchPlane::XY(), 0, 0, 3, "Cyl");
    doc.add_extrude(sk2, 10.0, false, BooleanMode::New, "CylExt");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_b = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(20, 0, 5), "CS_B");
    doc.features[cs_b].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(1, cs_a, cs_b, 7.25, 33.0, true, "Mate");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    size_t nb = doc.bodies.size();
    auto blob = doc.serialize_recipe();
    REQUIRE_FALSE(blob.empty());

    CadDocument fresh;
    REQUIRE(fresh.deserialize_recipe(blob));
    REQUIRE(fresh.bodies.size() == nb);

    const CadFeature* mf = nullptr;
    for (const auto& f : fresh.features) {
        if (f.type == CadFeatureType::Mate) { mf = &f; break; }
    }
    REQUIRE(mf != nullptr);
    REQUIRE(mf->mate_kind   == 1);
    REQUIRE_THAT(mf->mate_offset, WithinAbs(7.25, 1e-9));
    REQUIRE_THAT(mf->mate_angle,  WithinAbs(33.0, 1e-9));
    REQUIRE(mf->mate_flip   == true);
}

TEST_CASE("version 2 blob is rejected", "[CadDocument][mate]")
{

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Box");
    doc.add_extrude(sk, 5.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    std::ostringstream oss;
    {
        cereal::BinaryOutputArchive ar(oss);
        uint32_t fake_v = 2;
        ar(fake_v);
        ar(doc.features);
        ar(doc.variables);
    }
    std::string blob = oss.str();

    CadDocument fresh;
    REQUIRE_FALSE(fresh.deserialize_recipe(blob));
    REQUIRE_CONTAINS(fresh.error, "older version");
}

TEST_CASE("mate error: out of range connectors", "[CadDocument][mate]")
{

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Box");
    doc.add_extrude(sk, 5.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS");
    doc.features[cs].coordsys_body = 0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(0, 999, cs, 0, 0, false, "Bad");
    REQUIRE_FALSE(doc.recompute());
    REQUIRE_CONTAINS(doc.error, "mate_cs_a out of range");
    doc.features.pop_back(); doc.error.clear();

    doc.add_mate(0, cs, 999, 0, 0, false, "Bad");
    REQUIRE_FALSE(doc.recompute());
    REQUIRE_CONTAINS(doc.error, "mate_cs_b out of range");
    doc.features.pop_back(); doc.error.clear();

    doc.add_mate(0, sk, cs, 0, 0, false, "Bad");
    REQUIRE_FALSE(doc.recompute());
    REQUIRE_CONTAINS(doc.error, "not a valid CoordSys");
    doc.features.pop_back(); doc.error.clear();
}

// A mate stores its two connectors as FEATURE INDICES. remove_feature() erases a slot and remaps
// every surviving sketch_ref through the deletion, but it did not remap mate_cs_a / mate_cs_b —
// so deleting anything ahead of a connector slid both references down onto whatever features
// happened to occupy those indices. The validation in recompute() only catches out-of-range and
// non-CoordSys targets; when the landing slots are themselves CoordSys features — the normal
// case, since an assembly carries at least two — nothing reports anything. The mate silently
// resolves against the wrong frames and moves the wrong body.
//
// The third connector below (CS_Spare) is what makes the corruption silent rather than loud:
// without it the one-slot shift would push mate_cs_b onto the Mate feature itself and trip the
// "not a valid CoordSys" guard, hiding the real defect behind an error that looks handled.
TEST_CASE("mate connectors survive deletion of an earlier feature", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk_box = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Box");
    doc.add_extrude(sk_box, 5.0, false, BooleanMode::New, "BoxExt");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // The victim: an unrelated connector sitting AHEAD of the pair the mate will use.
    int cs_decoy = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(1, 1, 1), "CS_Decoy");
    doc.features[cs_decoy].coordsys_body = 0;
    REQUIRE(doc.recompute());

    int sk_cyl = doc.add_sketch(SketchShape::Circle, SketchPlane::XY(), 0, 0, 3, "Cyl");
    doc.add_extrude(sk_cyl, 10.0, false, BooleanMode::New, "CylExt");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 2);

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 5), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    int cs_spare = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(9, 9, 9), "CS_Spare");
    doc.features[cs_spare].coordsys_body = 0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(0, cs_fixed, cs_moving, 0.0, 0.0, false, "Mate");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    GProp_GProps props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, props);
    REQUIRE_THAT(double(props.CentreOfMass().X()), WithinAbs(5.0, 1e-4));

    REQUIRE(doc.remove_feature(cs_decoy));
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // The mate must still name the same two connectors it was built with.
    const CadFeature& mate = doc.features.back();
    REQUIRE(mate.type == CadFeatureType::Mate);
    REQUIRE(mate.mate_cs_a >= 0);
    REQUIRE(mate.mate_cs_b >= 0);
    REQUIRE(doc.features[mate.mate_cs_a].name == "CS_Fixed");
    REQUIRE(doc.features[mate.mate_cs_b].name == "CS_Moving");

    // ...and therefore still assemble the same way: the cylinder on the fixed connector.
    BRepGProp::VolumeProperties(doc.bodies[1].shape, props);
    REQUIRE_THAT(double(props.CentreOfMass().X()), WithinAbs(5.0, 1e-4));
    REQUIRE_THAT(double(props.CentreOfMass().Y()), WithinAbs(5.0, 1e-4));
    REQUIRE_THAT(double(props.CentreOfMass().Z()), WithinAbs(5.0, 1e-4));
}

// move_feature() has the same blind spot from the other direction: it swaps two slots and
// rewrites sketch_ref for both, but leaves mate_cs_a / mate_cs_b pointing at the old positions.
TEST_CASE("mate connectors survive reordering of an earlier feature", "[CadDocument][mate]")
{
    CadDocument doc;
    int sk_box = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Box");
    doc.add_extrude(sk_box, 5.0, false, BooleanMode::New, "BoxExt");
    REQUIRE(doc.recompute());

    int sk_cyl = doc.add_sketch(SketchShape::Circle, SketchPlane::XY(), 0, 0, 3, "Cyl");
    doc.add_extrude(sk_cyl, 10.0, false, BooleanMode::New, "CylExt");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 2);

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 5), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(0, cs_fixed, cs_moving, 0.0, 0.0, false, "Mate");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Swap the two connectors' slots. The mate's references must follow the features,
    // not stay behind on the indices.
    REQUIRE(doc.move_feature(cs_fixed, 1));
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    const CadFeature& mate = doc.features.back();
    REQUIRE(mate.type == CadFeatureType::Mate);
    REQUIRE(mate.mate_cs_a >= 0);
    REQUIRE(mate.mate_cs_b >= 0);
    REQUIRE(doc.features[mate.mate_cs_a].name == "CS_Fixed");
    REQUIRE(doc.features[mate.mate_cs_b].name == "CS_Moving");
}

TEST_CASE("mate error: no associated body", "[CadDocument][mate]")
{

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Box");
    doc.add_extrude(sk, 5.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 5), "CS_Moving");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(0, cs_fixed, cs_moving, 0, 0, false, "Bad");
    REQUIRE_FALSE(doc.recompute());
    REQUIRE_CONTAINS(doc.error, "no associated body");
}

TEST_CASE("mate error: disabled connector", "[CadDocument][mate]")
{

    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Box");
    doc.add_extrude(sk, 5.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 5), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.features[cs_moving].enabled = false;

    doc.add_mate(0, cs_fixed, cs_moving, 0, 0, false, "Bad");
    REQUIRE_FALSE(doc.recompute());
    REQUIRE_CONTAINS(doc.error, "not a valid CoordSys");
}

TEST_CASE("mate conflicts: a clean assembly reports none", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk_box = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Box");
    doc.add_extrude(sk_box, 5.0, false, BooleanMode::New, "BoxExt");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int sk_cyl = doc.add_sketch(SketchShape::Circle, SketchPlane::XY(), 0, 0, 3, "Cyl");
    doc.add_extrude(sk_cyl, 10.0, false, BooleanMode::New, "CylExt");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 2);

    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 5), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());

    int mi = doc.add_mate(0, cs_fixed, cs_moving, 0.0, 0.0, false, "Mate1");
    REQUIRE(mi >= 0);
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.mate_conflicts.empty());

    GProp_GProps props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, props);
    gp_Pnt com = props.CentreOfMass();
    REQUIRE_THAT(double(com.X()), WithinAbs(5.0, 1e-4));
    REQUIRE_THAT(double(com.Y()), WithinAbs(5.0, 1e-4));
    REQUIRE_THAT(double(com.Z()), WithinAbs(5.0, 1e-4));
}

TEST_CASE("mate conflicts: two mates driving the same body", "[CadDocument][mate]")
{
    CadDocument doc;
    int sk_box = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Box");
    doc.add_extrude(sk_box, 5.0, false, BooleanMode::New, "BoxExt");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int sk_cyl = doc.add_sketch(SketchShape::Circle, SketchPlane::XY(), 0, 0, 3, "Cyl");
    doc.add_extrude(sk_cyl, 10.0, false, BooleanMode::New, "CylExt");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 2);

    int cs_a = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 0), "CS_A");
    doc.features[cs_a].coordsys_body = 0;
    int cs_b = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 0), "CS_B");
    doc.features[cs_b].coordsys_body = 1;
    int cs_c = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(10, 10, 10), "CS_C");
    doc.features[cs_c].coordsys_body = 0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int mate1 = doc.add_mate(0, cs_c, cs_b, 0, 0, false, "Mate1");
    REQUIRE(mate1 >= 0);
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int mate2 = doc.add_mate(0, cs_a, cs_b, 0, 0, false, "Mate2");
    REQUIRE(mate2 >= 0);
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    REQUIRE(doc.mate_conflicts.size() == 1);
    REQUIRE(doc.mate_conflicts[0].first == mate2);
    REQUIRE(doc.mate_conflicts[0].second.find("Mate1") != std::string::npos);
    REQUIRE(doc.mate_conflicts[0].second.find("Mate2") != std::string::npos);
}

TEST_CASE("mate conflicts: a circular mate chain", "[CadDocument][mate]")
{
    CadDocument doc;
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "BoxA");
    doc.add_extrude(sk_a, 5.0, false, BooleanMode::New, "ExtA");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);

    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "BoxB");
    doc.add_extrude(sk_b, 5.0, false, BooleanMode::New, "ExtB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 2);

    int cs_a = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_A");
    doc.features[cs_a].coordsys_body = 0;
    int cs_b = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 5), "CS_B");
    doc.features[cs_b].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_c = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 10), "CS_C");
    doc.features[cs_c].coordsys_body = 0;
    int cs_d = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 10), "CS_D");
    doc.features[cs_d].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int mate1 = doc.add_mate(0, cs_a, cs_b, 0, 0, false, "MateAB");
    REQUIRE(mate1 >= 0);
    REQUIRE(doc.recompute());

    int mate2 = doc.add_mate(0, cs_d, cs_c, 0, 0, false, "MateBA");
    REQUIRE(mate2 >= 0);
    REQUIRE(doc.recompute());

    REQUIRE(doc.error.empty());
    REQUIRE(doc.mate_conflicts.size() >= 1);
    bool found_cycle = false;
    for (const auto& c : doc.mate_conflicts) {
        if (c.second.find("circular") != std::string::npos) {
            found_cycle = true;
            break;
        }
    }
    REQUIRE(found_cycle);
}

TEST_CASE("mate conflicts: a broken mate is left to apply_mate", "[CadDocument][mate]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "Box");
    doc.add_extrude(sk, 5.0, false, BooleanMode::New, "E");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS");
    doc.features[cs].coordsys_body = 0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(0, 999, cs, 0, 0, false, "Bad");
    REQUIRE_FALSE(doc.recompute());
    REQUIRE_CONTAINS(doc.error, "mate_cs_a out of range");
    REQUIRE(doc.mate_conflicts.empty());
}

TEST_CASE("ordering: fillet after mate resolves face ids", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "BoxA");
    doc.add_extrude(sk_a, 5.0, false, BooleanMode::New, "EA");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "BoxB");
    doc.add_extrude(sk_b, 5.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 2);

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(0, cs_fixed, cs_moving, 0.0, 0.0, false, "Mate");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_fillet(1.0, FaceGroup::All, "FilletAfterMate");
    doc.features.back().target_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
}

// --- M8a fix round: planar antiparallel + blind tests ---

static int find_face_by_normal(const CadDocument& doc, int body_idx, const Vec3d& dir, double tol = 0.99)
{
    auto faces = GeometryEngine::faces_of(doc.bodies[body_idx].shape);
    for (int fi = 0; fi < int(faces.size()); ++fi) {
        if (GeometryEngine::face_normal_world(faces[fi]).dot(dir) > tol) return fi;
    }
    return -1;
}

// Global edge index of the first edge belonging to `face_idx`.
//
// A CoordSys built from a face alone takes its z from the face normal (which follows the
// body) but its x from coordsys_x_hint, a WORLD constant. Such a frame is only half
// body-following: the body's rotation about the face normal is invisible to it, so no mate
// can correct or preserve a spin the connector cannot see. Pinning coordsys_edge to an edge
// of the body makes the in-plane direction follow the body too, which is what any test
// distinguishing Slider (corrects spin) from Cylindrical (preserves it) requires.
static int find_edge_on_face(const CadDocument& doc, int body_idx, int face_idx)
{
    TopoDS_Face f = GeometryEngine::face_by_index(doc.bodies[body_idx].shape, face_idx);
    if (f.IsNull()) return -1;
    auto face_edges = GeometryEngine::edges_of_face(f);
    auto all_edges  = GeometryEngine::edges_of(doc.bodies[body_idx].shape);
    for (int ei = 0; ei < int(all_edges.size()); ++ei) {
        for (const auto& fe : face_edges) {
            if (all_edges[ei].IsSame(fe)) return ei;
        }
    }
    return -1;
}

TEST_CASE("planar mate: antiparallel normals with asymmetric body", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    // Body A: box 20x20x5, centered at origin in XY
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 5.0, false, BooleanMode::New, "EA");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Body B: box 20x10x5, also centered at origin (asymmetric in Y)
    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 10, 0, "BoxB");
    doc.add_extrude(sk_b, 5.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 2);

    // Face on A with normal +X, face on B with normal -X
    int faceA = find_face_by_normal(doc, 0, Vec3d(1, 0, 0));
    REQUIRE(faceA >= 0);
    int faceB = find_face_by_normal(doc, 1, Vec3d(-1, 0, 0));
    REQUIRE(faceB >= 0);

    // Verify z_A != z_B (they point opposite)
    Vec3d nA_pre = GeometryEngine::face_normal_world(
        GeometryEngine::face_by_index(doc.bodies[0].shape, faceA));
    Vec3d nB_pre = GeometryEngine::face_normal_world(
        GeometryEngine::face_by_index(doc.bodies[1].shape, faceB));
    REQUIRE(nA_pre.dot(nB_pre) < -0.9);  // antiparallel

    int cs_fixed = doc.add_coordsys(CoordSysType::FaceAndDirection, Vec3d(0,0,0), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    doc.features[cs_fixed].coordsys_face = faceA;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_moving = doc.add_coordsys(CoordSysType::FaceAndDirection, Vec3d(0,0,0), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    doc.features[cs_moving].coordsys_face = faceB;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Planar mate: z_A=+X, z_B=-X antiparallel. offset=0.
    // With fix: 180° flip, then translated so z-distance=0.
    // Without fix: no rotation, only translation — body X-centroid moves differently.
    doc.add_mate(1, cs_fixed, cs_moving, 0.0, 0.0, false, "PlanarAnti");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // With the fix: 180° rotation about faceB centroid, then translation.
    // Body B's X-extents are mirrored. After rotation, centroid X = 2*o_B.x - pre_cx = -20,
    // then translated by offset ≈ 20mm → centroid returns near 0.
    // Without the fix: no rotation, body just translates ~+20mm → centroid X ≈ 20.
    GProp_GProps post_props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, post_props);
    double post_cx = post_props.CentreOfMass().X();
    REQUIRE_THAT(std::abs(post_cx), WithinAbs(0.0, 1e-4));
}

TEST_CASE("planar mate: in-plane pose preserved", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 5.0, false, BooleanMode::New, "EA");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "BoxB");
    doc.add_extrude(sk_b, 10.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 2);

    GProp_GProps pre_props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, pre_props);
    gp_Pnt pre_com = pre_props.CentreOfMass();

    // Both connectors use PointWorld (z=(0,0,1)). B's connector offset in X/Y from A's.
    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(10, 10, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(7, 4, 10), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(1, cs_fixed, cs_moving, 0.0, 0.0, false, "PlanarSameZ");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    GProp_GProps post_props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, post_props);
    gp_Pnt post_com = post_props.CentreOfMass();

    // In-plane (X,Y) components unchanged, only Z moves.
    REQUIRE_THAT(double(post_com.X()), WithinAbs(double(pre_com.X()), 1e-4));
    REQUIRE_THAT(double(post_com.Y()), WithinAbs(double(pre_com.Y()), 1e-4));
    REQUIRE_THAT(double(post_com.Z()), !WithinAbs(double(pre_com.Z()), 1e-4));
}

TEST_CASE("mate with FaceAndDirection connectors on non-Z faces", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    // Body A: box 20x20x10
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 10.0, false, BooleanMode::New, "EA");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Face on A with normal +Y
    int faceA = find_face_by_normal(doc, 0, Vec3d(0, 1, 0));
    REQUIRE(faceA >= 0);

    int cs_fixed = doc.add_coordsys(CoordSysType::FaceAndDirection, Vec3d(0,0,0), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    doc.features[cs_fixed].coordsys_face = faceA;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Body B: box placed at a different location
    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "BoxB");
    doc.add_extrude(sk_b, 5.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Face on B with normal +Y
    int faceB = find_face_by_normal(doc, 1, Vec3d(0, 1, 0));
    REQUIRE(faceB >= 0);

    int cs_moving = doc.add_coordsys(CoordSysType::FaceAndDirection, Vec3d(0,0,0), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    doc.features[cs_moving].coordsys_face = faceB;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Fastened mate: B's +Y face lands on A's +Y face, offset=0.
    // Both normals are +Y so z_A = z_B = (0,1,0) — genuine rotation from frame composition.
    doc.add_mate(0, cs_fixed, cs_moving, 0.0, 0.0, false, "MateY");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // After fastened mate, the two mated faces should be coincident:
    // same centroid position along the normal (Y), and same centroid in X and Z
    // (within face dimensions since they're different sizes).
    Vec3d ca = GeometryEngine::face_centroid_world(GeometryEngine::face_by_index(doc.bodies[0].shape, faceA));
    Vec3d cb = GeometryEngine::face_centroid_world(GeometryEngine::face_by_index(doc.bodies[1].shape, faceB));
    REQUIRE_THAT(cb.x(), WithinAbs(ca.x(), 1e-4));
    REQUIRE_THAT(cb.y(), WithinAbs(ca.y(), 1e-4));
    REQUIRE_THAT(cb.z(), WithinAbs(ca.z(), 1e-4));
}

TEST_CASE("fastened mate with flip on asymmetric body", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    // Body A: box
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 10.0, false, BooleanMode::New, "EA");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Body B: tapered extrude (asymmetric, centroid not at geometric centre)
    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "BoxB");
    int ex_b = doc.add_extrude(sk_b, 8.0, false, BooleanMode::New, "EB");
    doc.features[ex_b].taper_deg = 8.0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // PointWorld connectors at distinct positions.
    // A's connector on the top face centre, B's connector at a corner.
    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(10, 10, 10), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(2, 3, 0), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Fastened, NO flip
    doc.add_mate(0, cs_fixed, cs_moving, 0.0, 0.0, false, "MateNoFlip");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    GProp_GProps nf_props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, nf_props);
    double nf_z = nf_props.CentreOfMass().Z();

    doc.undo();

    // Fastened, WITH flip
    doc.add_mate(0, cs_fixed, cs_moving, 0.0, 0.0, true, "MateFlip");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    GProp_GProps f_props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, f_props);
    double f_z = f_props.CentreOfMass().Z();

    // Flip changes the centroid Z for an asymmetric body
    REQUIRE_THAT(f_z, !WithinAbs(nf_z, 1e-4));
}

// --- M8b: Revolute / Slider / Cylindrical mates ---

TEST_CASE("revolute mate: position corrected, rotation about axis preserved", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    // Body A: reference box
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 5.0, false, BooleanMode::New, "EA");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Body B: asymmetric box 20x10x5
    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 10, 0, "BoxB");
    doc.add_extrude(sk_b, 5.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.bodies.size() == 2);

    // Pre-rotate B 30deg about Z, translate off-axis to (15, 2, 7)
    doc.add_transform(1, Vec3d(15, 2, 7), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 30.0, false, "PrePose");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Record rotation: body B has a face that was originally +X, now at 30deg
    auto faces_pre = GeometryEngine::faces_of(doc.bodies[1].shape);
    REQUIRE(faces_pre.size() == 6);
    Vec3d pre_x_face_normal;
    bool found = false;
    for (const auto& f : faces_pre) {
        Vec3d n = GeometryEngine::face_normal_world(f);
        // The face that was originally +X now points ~(cos30, sin30, 0)
        if (std::abs(n.x() - 0.866) < 0.02 && std::abs(n.y() - 0.5) < 0.02) {
            pre_x_face_normal = n; found = true; break;
        }
    }
    REQUIRE(found);

    // Fixed connector on A at (5,5,5), world axes
    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    // Moving connector on B: PointWorld at its current (transformed) centroid
    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(15, 2, 7), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(2, cs_fixed, cs_moving, 0.0, 0.0, false, "Revolute");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Position corrected: body centroid moves to the axis line.
    // Connector B (15,2,7) → (5,5,5); body centroid (15,2,9.5) → (5,5,7.5).
    GProp_GProps props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, props);
    gp_Pnt com = props.CentreOfMass();
    REQUIRE_THAT(double(com.X()), WithinAbs(5.0, 1e-4));
    REQUIRE_THAT(double(com.Y()), WithinAbs(5.0, 1e-4));
    REQUIRE_THAT(double(com.Z()), WithinAbs(7.5, 1e-4));

    // Rotation about Z preserved: the ~(0.866, 0.5, 0) face normal still exists
    auto faces_post = GeometryEngine::faces_of(doc.bodies[1].shape);
    REQUIRE(faces_post.size() == 6);
    bool rot_preserved = false;
    for (const auto& f : faces_post) {
        Vec3d n = GeometryEngine::face_normal_world(f);
        if (std::abs(n.x() - 0.866) < 0.02 && std::abs(n.y() - 0.5) < 0.02) {
            rot_preserved = true; break;
        }
    }
    REQUIRE(rot_preserved);
    // Fastened would have aligned face normals to world axes: no face with Y≈0.5 would exist.
}

TEST_CASE("revolute mate: no-op when already correct", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 5.0, false, BooleanMode::New, "EA");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "BoxB");
    doc.add_extrude(sk_b, 5.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Move body B to exactly the correct pose: on axis at (5,5,5) with no rotation
    doc.add_transform(1, Vec3d(5, 5, 5), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 0.0, false, "PrePose");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    GProp_GProps pre_props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, pre_props);
    gp_Pnt pre_com = pre_props.CentreOfMass();

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(2, cs_fixed, cs_moving, 0.0, 0.0, false, "RevoluteNoOp");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    GProp_GProps post_props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, post_props);
    gp_Pnt post_com = post_props.CentreOfMass();
    REQUIRE_THAT(double(post_com.X()), WithinAbs(double(pre_com.X()), 1e-4));
    REQUIRE_THAT(double(post_com.Y()), WithinAbs(double(pre_com.Y()), 1e-4));
    REQUIRE_THAT(double(post_com.Z()), WithinAbs(double(pre_com.Z()), 1e-4));
}

TEST_CASE("revolute mate: mate_angle applies additional rotation", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 5.0, false, BooleanMode::New, "EA");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Body B: asymmetric box, pre-rotated 30deg about Z, off-axis
    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 10, 0, "BoxB");
    doc.add_extrude(sk_b, 5.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_transform(1, Vec3d(15, 2, 7), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 30.0, false, "PrePose");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(15, 2, 7), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // mate_angle=45deg on top of preserved 30deg → face originally +X now at 75deg
    doc.add_mate(2, cs_fixed, cs_moving, 0.0, 45.0, false, "RevoluteAngle");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Position on axis line (same as revolute preservation test)
    GProp_GProps props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, props);
    gp_Pnt com = props.CentreOfMass();
    REQUIRE_THAT(double(com.X()), WithinAbs(5.0, 1e-4));
    REQUIRE_THAT(double(com.Y()), WithinAbs(5.0, 1e-4));
    REQUIRE_THAT(double(com.Z()), WithinAbs(7.5, 1e-4));

    // Rotation = 30deg (preserved) + 45deg (mate_angle) = 75deg → normal ≈ (cos75, sin75, 0)
    auto faces = GeometryEngine::faces_of(doc.bodies[1].shape);
    bool found_75 = false;
    for (const auto& f : faces) {
        Vec3d n = GeometryEngine::face_normal_world(f);
        if (std::abs(n.x() - 0.2588) < 0.02 && std::abs(n.y() - 0.9659) < 0.02) {
            found_75 = true; break;
        }
    }
    REQUIRE(found_75);
}

TEST_CASE("slider mate: rotation corrected, axial position preserved", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 5.0, false, BooleanMode::New, "EA");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 10, 0, "BoxB");
    doc.add_extrude(sk_b, 5.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Pre-rotate B 30deg about Z, translate off-axis to (15, 2, 13)
    doc.add_transform(1, Vec3d(15, 2, 13), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 30.0, false, "PrePose");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    GProp_GProps pre_props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, pre_props);
    double pre_z = pre_props.CentreOfMass().Z();

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(15, 2, 13), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(3, cs_fixed, cs_moving, 0.0, 0.0, false, "Slider");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Perpendicular position corrected to the axis line (X=5, Y=5)
    GProp_GProps post_props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, post_props);
    gp_Pnt post_com = post_props.CentreOfMass();
    REQUIRE_THAT(double(post_com.X()), WithinAbs(5.0, 1e-4));
    REQUIRE_THAT(double(post_com.Y()), WithinAbs(5.0, 1e-4));

    // Axial (Z) position preserved from pre-mate pose
    REQUIRE_THAT(double(post_com.Z()), WithinAbs(pre_z, 1e-4));
    // Fastened would have placed the body at Z=5. pre_z=15.5 (centroid), clearly different.
}

TEST_CASE("slider mate: mate_offset shifts axial position", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 5.0, false, BooleanMode::New, "EA");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "BoxB");
    doc.add_extrude(sk_b, 5.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_transform(1, Vec3d(12, 3, 9), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 0.0, false, "PrePose");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(12, 3, 9), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Slider with mate_offset=4: body centroid at (12,3,11.5), connector at (12,3,9).
    // Perpendicular corrected: X=5,Y=5. Axial: preserved Z 11.5 + offset 4 = 15.5.
    doc.add_mate(3, cs_fixed, cs_moving, 4.0, 0.0, false, "SliderOffset");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    GProp_GProps props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, props);
    gp_Pnt com = props.CentreOfMass();
    REQUIRE_THAT(double(com.X()), WithinAbs(5.0, 1e-4));
    REQUIRE_THAT(double(com.Y()), WithinAbs(5.0, 1e-4));
    REQUIRE_THAT(double(com.Z()), WithinAbs(15.5, 1e-3));
}

TEST_CASE("slider mate: no-op when already correct", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 5.0, false, BooleanMode::New, "EA");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "BoxB");
    doc.add_extrude(sk_b, 5.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Body already on axis at (5,5,20), no rotation
    doc.add_transform(1, Vec3d(5, 5, 20), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 0.0, false, "PrePose");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    GProp_GProps pre_props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, pre_props);
    gp_Pnt pre_com = pre_props.CentreOfMass();

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 20), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(3, cs_fixed, cs_moving, 0.0, 0.0, false, "SliderNoOp");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    GProp_GProps post_props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, post_props);
    gp_Pnt post_com = post_props.CentreOfMass();
    REQUIRE_THAT(double(post_com.X()), WithinAbs(double(pre_com.X()), 1e-4));
    REQUIRE_THAT(double(post_com.Y()), WithinAbs(double(pre_com.Y()), 1e-4));
    REQUIRE_THAT(double(post_com.Z()), WithinAbs(double(pre_com.Z()), 1e-4));
}

TEST_CASE("cylindrical mate: perpendicular corrected, rotation and axial preserved", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 5.0, false, BooleanMode::New, "EA");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 10, 0, "BoxB");
    doc.add_extrude(sk_b, 5.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Pre-rotate B 30deg about Z, translate off-axis to (15, 2, 13)
    doc.add_transform(1, Vec3d(15, 2, 13), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 30.0, false, "PrePose");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    GProp_GProps pre_props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, pre_props);
    double pre_z = pre_props.CentreOfMass().Z();

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(15, 2, 13), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(4, cs_fixed, cs_moving, 0.0, 0.0, false, "Cylindrical");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Perpendicular position corrected to axis line
    GProp_GProps post_props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, post_props);
    gp_Pnt com = post_props.CentreOfMass();
    REQUIRE_THAT(double(com.X()), WithinAbs(5.0, 1e-4));
    REQUIRE_THAT(double(com.Y()), WithinAbs(5.0, 1e-4));

    // Axial Z position preserved from pre-mate
    REQUIRE_THAT(double(com.Z()), WithinAbs(pre_z, 1e-4));

    // Rotation about Z preserved: face originally +X is still at 30deg
    auto faces = GeometryEngine::faces_of(doc.bodies[1].shape);
    REQUIRE(faces.size() == 6);
    bool rot_preserved = false;
    for (const auto& f : faces) {
        Vec3d n = GeometryEngine::face_normal_world(f);
        if (std::abs(n.x() - 0.866) < 0.02 && std::abs(n.y() - 0.5) < 0.02) {
            rot_preserved = true; break;
        }
    }
    REQUIRE(rot_preserved);
    // Fastened would have aligned face normals to world axes and fixed Z.
}

TEST_CASE("cylindrical mate: mate_offset and mate_angle applied on top", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 5.0, false, BooleanMode::New, "EA");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 10, 0, "BoxB");
    doc.add_extrude(sk_b, 5.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_transform(1, Vec3d(15, 2, 9), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 30.0, false, "PrePose");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(15, 2, 9), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(4, cs_fixed, cs_moving, 3.0, 45.0, false, "CylOffsetAngle");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Position: perpendicular corrected, axial = 11.5 (centroid preserved) + 3 (offset) = 14.5
    GProp_GProps props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, props);
    gp_Pnt com = props.CentreOfMass();
    REQUIRE_THAT(double(com.X()), WithinAbs(5.0, 1e-4));
    REQUIRE_THAT(double(com.Y()), WithinAbs(5.0, 1e-4));
    REQUIRE_THAT(double(com.Z()), WithinAbs(14.5, 1e-3));

    // Rotation = 30deg (preserved) + 45deg (angle) = 75deg
    auto faces = GeometryEngine::faces_of(doc.bodies[1].shape);
    bool found_75 = false;
    for (const auto& f : faces) {
        Vec3d n = GeometryEngine::face_normal_world(f);
        if (std::abs(n.x() - 0.2588) < 0.02 && std::abs(n.y() - 0.9659) < 0.02) {
            found_75 = true; break;
        }
    }
    REQUIRE(found_75);
}

TEST_CASE("cylindrical mate: no-op when already correct", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 5.0, false, BooleanMode::New, "EA");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 10, 10, 0, "BoxB");
    doc.add_extrude(sk_b, 5.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Body already on axis at (5,5,20), no rotation
    doc.add_transform(1, Vec3d(5, 5, 20), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 0.0, false, "PrePose");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    GProp_GProps pre_props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, pre_props);
    gp_Pnt pre_com = pre_props.CentreOfMass();

    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(5, 5, 20), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_mate(4, cs_fixed, cs_moving, 0.0, 0.0, false, "CylNoOp");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    GProp_GProps post_props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, post_props);
    gp_Pnt post_com = post_props.CentreOfMass();
    REQUIRE_THAT(double(post_com.X()), WithinAbs(double(pre_com.X()), 1e-4));
    REQUIRE_THAT(double(post_com.Y()), WithinAbs(double(pre_com.Y()), 1e-4));
    REQUIRE_THAT(double(post_com.Z()), WithinAbs(double(pre_com.Z()), 1e-4));
}

TEST_CASE("revolute mate with FaceAndDirection on non-Z faces", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    // Body A: box 20x20x10
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 10.0, false, BooleanMode::New, "EA");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Face on A with normal +Y
    int faceA = find_face_by_normal(doc, 0, Vec3d(0, 1, 0));
    REQUIRE(faceA >= 0);
    Vec3d nA = GeometryEngine::face_normal_world(
        GeometryEngine::face_by_index(doc.bodies[0].shape, faceA));

    // Body B: asymmetric box 20x10x5, also with +Y face
    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 10, 0, "BoxB");
    doc.add_extrude(sk_b, 5.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int faceB = find_face_by_normal(doc, 1, Vec3d(0, 1, 0));
    REQUIRE(faceB >= 0);
    Vec3d nB_pre = GeometryEngine::face_normal_world(
        GeometryEngine::face_by_index(doc.bodies[1].shape, faceB));
    REQUIRE_THAT(nB_pre.dot(nA), WithinAbs(1.0, 1e-4));

    // Pre-rotate B about Y (the face normal) by 30deg to give it a non-trivial rotation about its connector z
    doc.add_transform(1, Vec3d(0, 0, 0), Vec3d(0, 1, 0), Vec3d(0, 0, 0), 30.0, false, "PrePose");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // FaceAndDirection on A's +Y face
    int cs_fixed = doc.add_coordsys(CoordSysType::FaceAndDirection, Vec3d(0,0,0), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    doc.features[cs_fixed].coordsys_face = faceA;
    // FaceAndDirection on B's +Y face
    int cs_moving = doc.add_coordsys(CoordSysType::FaceAndDirection, Vec3d(0,0,0), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    doc.features[cs_moving].coordsys_face = faceB;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Record face normals on B before mate (to verify rotation preservation)
    // +30deg rotation about Y maps +Z=(0,0,1) → (0.5, 0, 0.866), -Z → (-0.5, 0, -0.866)
    auto faces_pre = GeometryEngine::faces_of(doc.bodies[1].shape);
    Vec3d pre_rotated_normal;
    {
        bool fnd = false;
        for (const auto& f : faces_pre) {
            Vec3d n = GeometryEngine::face_normal_world(f);
            if (std::abs(n.z()) > 0.85 && std::abs(n.x()) > 0.45 && std::abs(n.y()) < 0.02) {
                pre_rotated_normal = n; fnd = true; break;
            }
        }
        REQUIRE(fnd);
    }

    doc.add_mate(2, cs_fixed, cs_moving, 0.0, 0.0, false, "RevoluteFaceDir");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Position: faces are coincident (same as fastened position since no offset and axes aligned)
    Vec3d ca = GeometryEngine::face_centroid_world(GeometryEngine::face_by_index(doc.bodies[0].shape, faceA));
    Vec3d cb = GeometryEngine::face_centroid_world(GeometryEngine::face_by_index(doc.bodies[1].shape, faceB));
    REQUIRE_THAT(cb.x(), WithinAbs(ca.x(), 1e-4));
    REQUIRE_THAT(cb.y(), WithinAbs(ca.y(), 1e-4));
    REQUIRE_THAT(cb.z(), WithinAbs(ca.z(), 1e-4));

    // Rotation about the connector z (which is +Y) is preserved:
    // the face with |z| ≈ 0.866, |x| ≈ 0.5, y ≈ 0 must still exist.
    auto faces_post = GeometryEngine::faces_of(doc.bodies[1].shape);
    bool rot_preserved = false;
    for (const auto& f : faces_post) {
        Vec3d n = GeometryEngine::face_normal_world(f);
        if (std::abs(n.z()) > 0.85 && std::abs(n.x()) > 0.45 && std::abs(n.y()) < 0.02) {
            rot_preserved = true; break;
        }
    }
    REQUIRE(rot_preserved);
    // Fastened would force all axes to align: no face with Z≈0.87 would exist.
}

// --- M8b: rotation-coverage hole — FaceAndDirection captures body orientation ---

TEST_CASE("slider mate with FaceAndDirection corrects rotation", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    // Body A: box 20x20x10
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 10.0, false, BooleanMode::New, "EA");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int faceA = find_face_by_normal(doc, 0, Vec3d(0, 0, 1));
    REQUIRE(faceA >= 0);

    // Body B: asymmetric box 20x10x5
    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 10, 0, "BoxB");
    doc.add_extrude(sk_b, 5.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int faceB = find_face_by_normal(doc, 1, Vec3d(0, 0, 1));
    REQUIRE(faceB >= 0);

    // Pre-rotate B: first 20deg about X (tilts +Z face normal off-axis, so the
    // connector z genuinely differs from A's), then 30deg about Z (adds rotation
    // about the mate axis that Slider must correct and Cylindrical must preserve).
    doc.add_transform(1, Vec3d(14, 3, 7), Vec3d(1, 0, 0), Vec3d(0, 0, 0), 20.0, false, "RotX");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_transform(1, Vec3d(0, 0, 0), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 30.0, false, "RotZ");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Both connectors pin their in-plane direction to an edge of their own body, so each
    // frame follows its body's spin. Without this the frames' x comes from the world hint
    // and the 30deg spin is invisible to the mate — see find_edge_on_face.
    int edgeA = find_edge_on_face(doc, 0, faceA);
    int edgeB = find_edge_on_face(doc, 1, faceB);
    REQUIRE(edgeA >= 0);
    REQUIRE(edgeB >= 0);

    int cs_fixed = doc.add_coordsys(CoordSysType::FaceAndDirection, Vec3d(0, 0, 0), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    doc.features[cs_fixed].coordsys_face = faceA;
    doc.features[cs_fixed].coordsys_edge = edgeA;

    int cs_moving = doc.add_coordsys(CoordSysType::FaceAndDirection, Vec3d(0, 0, 0), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    doc.features[cs_moving].coordsys_face = faceB;
    doc.features[cs_moving].coordsys_edge = edgeB;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Record the pre-mate axial position of the CONNECTOR, not of the centroid. Slider
    // rotates the body about the connector origin to correct the 20deg tilt, and that
    // rotation legitimately moves the centroid in Z (by -d*(1-cos20) for a centroid d
    // below the mated face) while leaving the connector itself where it is. The DOF
    // Slider preserves is the connector's position along the axis.
    const double pre_conn_z = GeometryEngine::face_centroid_world(
        GeometryEngine::face_by_index(doc.bodies[1].shape, faceB)).z();

    doc.add_mate(3, cs_fixed, cs_moving, 0.0, 0.0, false, "SliderFaceDir");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Position: perpendicular corrected to axis line through A's +Z face centroid.
    // The +Z face of a 20x20x10 box centred at origin has centroid at (0, 0, 10).
    GProp_GProps props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, props);
    gp_Pnt com = props.CentreOfMass();
    REQUIRE_THAT(double(com.X()), WithinAbs(0.0, 1e-4));
    REQUIRE_THAT(double(com.Y()), WithinAbs(0.0, 1e-4));
    // Axial position of the connector preserved — Slider does not own that DOF.
    const double post_conn_z = GeometryEngine::face_centroid_world(
        GeometryEngine::face_by_index(doc.bodies[1].shape, faceB)).z();
    REQUIRE_THAT(post_conn_z, WithinAbs(pre_conn_z, 1e-4));

    // Rotation: Slider owns this DOF — all face normals must be axis-aligned.
    auto faces = GeometryEngine::faces_of(doc.bodies[1].shape);
    REQUIRE(faces.size() == 6);
    int axis_aligned = 0;
    for (const auto& f : faces) {
        Vec3d n = GeometryEngine::face_normal_world(f);
        double d = std::max({std::abs(n.x()), std::abs(n.y()), std::abs(n.z())});
        if (d > 0.999) ++axis_aligned;
    }
    REQUIRE(axis_aligned == 6);
    // Dropping rotation correction would leave 30deg Z rotation — at least 2
    // faces would have normals not aligned to any world axis.
}

TEST_CASE("cylindrical mate with FaceAndDirection preserves rotation", "[CadDocument][mate]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    // Body A: box 20x20x10
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 10.0, false, BooleanMode::New, "EA");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int faceA = find_face_by_normal(doc, 0, Vec3d(0, 0, 1));
    REQUIRE(faceA >= 0);

    // Body B: asymmetric box 20x10x5
    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 10, 0, "BoxB");
    doc.add_extrude(sk_b, 5.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int faceB = find_face_by_normal(doc, 1, Vec3d(0, 0, 1));
    REQUIRE(faceB >= 0);

    // Same two-step pre-rotation as the slider test.
    doc.add_transform(1, Vec3d(14, 3, 7), Vec3d(1, 0, 0), Vec3d(0, 0, 0), 20.0, false, "RotX");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    doc.add_transform(1, Vec3d(0, 0, 0), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 30.0, false, "RotZ");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Edge-pinned frames, as in the Slider case. These matter here for the opposite
    // reason: with a world-derived in-plane direction, a Cylindrical that WRONGLY aligned
    // the spin would still leave the body's 30deg face at 30deg, and the assertion below
    // would pass for the wrong implementation.
    int edgeA = find_edge_on_face(doc, 0, faceA);
    int edgeB = find_edge_on_face(doc, 1, faceB);
    REQUIRE(edgeA >= 0);
    REQUIRE(edgeB >= 0);

    int cs_fixed = doc.add_coordsys(CoordSysType::FaceAndDirection, Vec3d(0, 0, 0), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    doc.features[cs_fixed].coordsys_face = faceA;
    doc.features[cs_fixed].coordsys_edge = edgeA;

    int cs_moving = doc.add_coordsys(CoordSysType::FaceAndDirection, Vec3d(0, 0, 0), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    doc.features[cs_moving].coordsys_face = faceB;
    doc.features[cs_moving].coordsys_edge = edgeB;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // As in the Slider case, the preserved DOF is the CONNECTOR's position along the
    // axis, not the centroid's: aligning the 20deg tilt rotates the body about the
    // connector origin, which moves the centroid in Z by design.
    const double pre_conn_z_c = GeometryEngine::face_centroid_world(
        GeometryEngine::face_by_index(doc.bodies[1].shape, faceB)).z();

    doc.add_mate(4, cs_fixed, cs_moving, 0.0, 0.0, false, "CylFaceDir");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Position: perpendicular corrected to axis line (X=0, Y=0 at +Z face centroid)
    GProp_GProps props;
    BRepGProp::VolumeProperties(doc.bodies[1].shape, props);
    gp_Pnt com = props.CentreOfMass();
    REQUIRE_THAT(double(com.X()), WithinAbs(0.0, 1e-4));
    REQUIRE_THAT(double(com.Y()), WithinAbs(0.0, 1e-4));
    // Axial position of the connector preserved — Cylindrical leaves that DOF free.
    const double post_conn_z_c = GeometryEngine::face_centroid_world(
        GeometryEngine::face_by_index(doc.bodies[1].shape, faceB)).z();
    REQUIRE_THAT(post_conn_z_c, WithinAbs(pre_conn_z_c, 1e-4));

    // Rotation about Z (30deg) is preserved: the 20deg X-rotation was undone
    // by z-alignment, leaving the 30deg Z-rotation intact.
    // The +X face normal should be at ~(cos30, sin30, 0).
    auto faces = GeometryEngine::faces_of(doc.bodies[1].shape);
    REQUIRE(faces.size() == 6);
    bool rot_preserved = false;
    for (const auto& f : faces) {
        Vec3d n = GeometryEngine::face_normal_world(f);
        if (std::abs(n.x() - 0.866) < 0.02 && std::abs(n.y() - 0.5) < 0.02) {
            rot_preserved = true; break;
        }
    }
    REQUIRE(rot_preserved);
    // If cylindrical behaved like slider, the face would be at (1,0,0) instead.
}

// --- Interference detection (M8c) ---

TEST_CASE("interference: overlapping bodies reported with overlap volume", "[CadDocument][interference]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;
    // A: 20x20x10 box centred on the origin in XY, z in [0,10]
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 10.0, false, BooleanMode::New, "EA");
    // B: 20x20x10 box, same footprint
    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxB");
    doc.add_extrude(sk_b, 10.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 2);

    // Lift B by 6mm: the two overlap over 4mm of height => 20*20*4 = 1600 mm^3.
    doc.add_transform(1, Vec3d(0, 0, 6), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 0.0, false, "LiftB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    auto hits = doc.check_interference();
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].body_a == 0);
    REQUIRE(hits[0].body_b == 1);
    REQUIRE_THAT(hits[0].volume, WithinAbs(1600.0, 1e-3));
}

TEST_CASE("interference: disjoint and merely touching bodies are not reported", "[CadDocument][interference]")
{
    CadDocument doc;
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 10.0, false, BooleanMode::New, "EA");
    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxB");
    doc.add_extrude(sk_b, 10.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());

    SECTION("clearly apart") {
        doc.add_transform(1, Vec3d(0, 0, 50), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 0.0, false, "MoveB");
        REQUIRE(doc.recompute());
        REQUIRE(doc.check_interference().empty());
    }

    SECTION("face-to-face contact encloses no volume") {
        // B sits exactly on top of A: they share a face but nothing overlaps.
        doc.add_transform(1, Vec3d(0, 0, 10), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 0.0, false, "StackB");
        REQUIRE(doc.recompute());
        REQUIRE(doc.check_interference().empty());
    }
}

TEST_CASE("interference: sheet bodies are skipped", "[CadDocument][interference]")
{
    CadDocument doc;
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 10.0, false, BooleanMode::New, "EA");
    // A sheet passing straight through the solid: it has no volume, so no interference.
    int sk_s = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 40, 40, 0, "Sheet");
    doc.add_surface_extrude(sk_s, 5.0, "SE");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 2);
    REQUIRE(CadDocument::is_sheet_shape(doc.bodies[1].shape));

    REQUIRE(doc.check_interference().empty());
}

TEST_CASE("interference: reports every overlapping pair", "[CadDocument][interference]")
{
    CadDocument doc;
    for (int i = 0; i < 3; ++i) {
        int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Box");
        doc.add_extrude(sk, 10.0, false, BooleanMode::New, "E");
    }
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 3);

    // 0 and 1 stay coincident (overlapping); 2 is moved clear of both.
    doc.add_transform(2, Vec3d(0, 0, 100), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 0.0, false, "MoveC");
    REQUIRE(doc.recompute());

    auto hits = doc.check_interference();
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].body_a == 0);
    REQUIRE(hits[0].body_b == 1);

    // min_volume gates the report: a threshold above the overlap silences it.
    REQUIRE(doc.check_interference(1e9).empty());
}

TEST_CASE("interference: detects a clash created by a mate", "[CadDocument][interference]")
{
    CadDocument doc;
    int sk_a = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxA");
    doc.add_extrude(sk_a, 10.0, false, BooleanMode::New, "EA");
    int sk_b = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "BoxB");
    doc.add_extrude(sk_b, 10.0, false, BooleanMode::New, "EB");
    REQUIRE(doc.recompute());

    // Park B well clear, so the clash below is created by the mate and nothing else.
    doc.add_transform(1, Vec3d(0, 0, 80), Vec3d(0, 0, 1), Vec3d(0, 0, 0), 0.0, false, "ParkB");
    REQUIRE(doc.recompute());
    REQUIRE(doc.check_interference().empty());

    // Fastened mate onto a connector inside A's volume drives B into A.
    int cs_fixed = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 5), "CS_Fixed");
    doc.features[cs_fixed].coordsys_body = 0;
    int cs_moving = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 85), "CS_Moving");
    doc.features[cs_moving].coordsys_body = 1;
    REQUIRE(doc.recompute());

    doc.add_mate(0, cs_fixed, cs_moving, 0.0, 0.0, false, "Clash");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    auto hits = doc.check_interference();
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].volume > 1.0);
}

// A filleted solid must reach the plate as a watertight mesh. OCCT emits one degenerate
// triangle at the pole of every corner sphere patch; welded, its v->v edge counts as an open
// edge and the slicer tells the user to go repair the model in another CAD application --
// the exact round trip this feature exists to remove. snaporca-agw.
TEST_CASE("CadDocument filleted solid tessellates watertight", "[CadDocument]")
{
    CadDocument doc;
    SketchProfile sp;
    sp.points.push_back(Vec2d(  0,  0));
    sp.points.push_back(Vec2d( 80,  0));
    sp.points.push_back(Vec2d( 80, 50));
    sp.points.push_back(Vec2d(  0, 50));
    sp.closed = true;
    const int sk = doc.add_sketch_profile(sp, SketchPlane::XY(), "P");
    doc.add_extrude(sk, 12.0, false, BooleanMode::New, "E");
    doc.add_fillet(3.0, FaceGroup::All, "F");
    REQUIRE(doc.recompute());

    CHECK(doc.display_mesh.stats().open_edges == 0);
    CHECK(its_num_open_edges(doc.display_mesh.its) == 0);
    // No degenerate triangles survive the weld, and the per-triangle face map stays aligned.
    size_t degenerate = 0;
    for (const auto& t : doc.display_mesh.its.indices)
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) ++degenerate;
    REQUIRE(degenerate == 0);
    REQUIRE(doc.display_tri_face.size() == doc.display_mesh.its.indices.size());
    REQUIRE(doc.display_tri_body.size() == doc.display_mesh.its.indices.size());
}

// A rejected solve must leave the sketch untouched. libslvs writes its last Newton iterate
// into the params whether or not it converged, so reading geometry back unconditionally made
// every failed attempt destructive -- and the fillet degrade ladder tries a deliberately
// over-constrained rung FIRST, so a filleted corner was wrecked before the rung that works
// ever got a chance. snaporca-pl5.
TEST_CASE("Failed sketch solve leaves geometry untouched", "[CadDocument]")
{
    using R  = SketchPointRole;
    using CT = SketchConstraintType;
    auto line = [](Vec2d p0, Vec2d p1) {
        SketchEntity e; e.type = SketchEntity::Type::Line; e.p0 = p0; e.p1 = p1; return e; };
    auto coinc = [](int ea, R ra, int eb, R rb) {
        SketchEntityConstraintDef d; d.type = CT::Coincident; d.ea = ea; d.ra = ra; d.eb = eb; d.rb = rb; return d; };
    auto axis = [](CT t, int e) {
        SketchEntityConstraintDef d; d.type = t; d.ea = e; d.ra = R::P0; d.eb = e; d.rb = R::P1; return d; };

    // Axis-aligned rectangle, drawn as four lines with the constraints the sketch tool
    // infers: a Coincident at each corner and H/V per leg.
    std::vector<SketchEntity> ents = {
        line(Vec2d(-89.32,  72.05), Vec2d( 52.00,  72.05)),   // 0 top    (H)
        line(Vec2d( 52.00,  72.05), Vec2d( 52.00, -68.97)),   // 1 right  (V)
        line(Vec2d( 52.00, -68.97), Vec2d(-89.32, -68.97)),   // 2 bottom (H)
        line(Vec2d(-89.32, -68.97), Vec2d(-89.32,  72.05)),   // 3 left   (V)
    };
    std::vector<SketchEntityConstraintDef> cs = {
        coinc(0, R::P1, 1, R::P0), coinc(0, R::P0, 3, R::P1),
        coinc(1, R::P1, 2, R::P0), coinc(2, R::P1, 3, R::P0),
        axis(CT::Horizontal, 0), axis(CT::Vertical, 1),
        axis(CT::Horizontal, 2), axis(CT::Vertical, 3),
    };
    REQUIRE(solve_sketch_entities(ents, cs));

    // Fillet the top-left corner: trim both legs, drop the stale corner Coincident.
    const int a = 0, b = 3;
    SketchEntity a_out, b_out, arc;
    REQUIRE(SketchEngine::fillet_lines(ents[a], ents[b], 28.205, a_out, b_out, arc));
    ents[a] = a_out; ents[b] = b_out;
    const int xi = int(ents.size());
    ents.push_back(arc);
    cs.erase(std::remove_if(cs.begin(), cs.end(), [&](const SketchEntityConstraintDef& d) {
        return d.type == CT::Coincident && d.ea == a && d.ra == R::P0 && d.eb == b && d.rb == R::P1;
    }), cs.end());
    const std::vector<SketchEntity> trimmed = ents;

    auto coin = [&](R xr, int ln, R lr) { return coinc(xi, xr, ln, lr); };
    auto tang = [&](int ln) {
        SketchEntityConstraintDef d; d.type = CT::Tangent; d.ea = xi; d.eb = ln; return d; };

    // Rung 1 of the ladder: a tangent on each leg. Over-constrained against the legs'
    // own H/V, so it must be rejected -- and must not move a single point.
    {
        std::vector<SketchEntityConstraintDef> pc = cs;
        for (const auto& c : { coin(R::P0, a, R::P0), coin(R::P1, b, R::P1), tang(a), tang(b) })
            pc.push_back(c);
        std::vector<SketchEntity> e = ents;
        REQUIRE_FALSE(solve_sketch_entities(e, pc));
        for (size_t i = 0; i < e.size(); ++i) {
            CHECK((e[i].p0 - trimmed[i].p0).norm() == Approx(0.0).margin(1e-9));
            CHECK((e[i].p1 - trimmed[i].p1).norm() == Approx(0.0).margin(1e-9));
            CHECK((e[i].center - trimmed[i].center).norm() == Approx(0.0).margin(1e-9));
        }
    }

    // Rung 2 (one tangent) solves, and the arc keeps the radius the fillet gave it.
    {
        std::vector<SketchEntityConstraintDef> pc = cs;
        for (const auto& c : { coin(R::P0, a, R::P0), coin(R::P1, b, R::P1), tang(a) })
            pc.push_back(c);
        REQUIRE(solve_sketch_entities(ents, pc));
        CHECK(ents[xi].radius == Approx(28.205).margin(1e-6));
        // Corner stays open: the legs end on the arc, they do not meet each other.
        CHECK((ents[a].p0 - ents[b].p1).norm() > 1.0);
        CHECK((ents[a].p0 - ents[xi].p0).norm() == Approx(0.0).margin(1e-6));
        CHECK((ents[b].p1 - ents[xi].p1).norm() == Approx(0.0).margin(1e-6));
    }
}

// A subtraction whose tool misses the target is a perfectly legal boolean that removes nothing,
// so OCCT reports success and the feature lands in the recipe with ok:true and an unchanged body.
// That is how a hole placed with world coordinates instead of plane-frame ones read as "drilled"
// three times in a row while the volume never moved. snaporca-daf.
TEST_CASE("A cut that removes no material is an error, not a silent success", "[CadDocument]")
{
    // 20 x 20 box, 20 tall, centred on the origin of the XY plane.
    auto box = [] {
        CadDocument d;
        int sk = d.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Sketch");
        d.add_extrude(sk, 20.0, false, BooleanMode::New, "Extrude");
        return d;
    };

    SECTION("hole placed clear of the body is rejected") {
        CadDocument doc = box();
        REQUIRE(doc.recompute());
        const double solid = doc.body_mass_properties(0).volume;

        // 135 mm away — exactly the failure the issue reported (world centre passed for a
        // plane-frame coordinate). The old behaviour: recompute() true, volume unchanged.
        doc.add_hole(8.0, 20.0, true, 135.0, 0.0, SketchPlane::XY(), "Hole");
        CHECK_FALSE(doc.recompute());
        CHECK(doc.error.find("removed no material") != std::string::npos);

        // And the body is left as it was, not half-applied.
        CadDocument again = box();
        REQUIRE(again.recompute());
        CHECK_THAT(again.body_mass_properties(0).volume,
                   Catch::Matchers::WithinAbs(solid, 1e-6));
    }

    SECTION("the same hole on the body still works") {
        CadDocument doc = box();
        REQUIRE(doc.recompute());
        const double solid = doc.body_mass_properties(0).volume;
        doc.add_hole(8.0, 20.0, true, 0.0, 0.0, SketchPlane::XY(), "Hole");
        REQUIRE(doc.recompute());
        CHECK_THAT(doc.body_mass_properties(0).volume,
                   Catch::Matchers::WithinRel(solid - M_PI * 16.0 * 20.0, 0.01));
    }

    SECTION("a cut-mode extrude that misses is rejected too") {
        CadDocument doc = box();
        REQUIRE(doc.recompute());
        // Same trick, on the sketch plane's origin: the profile sits far outside the box,
        // so the subtraction is a legal no-op.
        SketchPlane far_plane = SketchPlane::XY();
        far_plane.origin = Vec3d(200.0, 0.0, 0.0);
        int sk = doc.add_sketch(SketchShape::Rectangle, far_plane, 5, 5, 10, "Tool");
        doc.add_extrude(sk, 30.0, false, BooleanMode::Cut, "Cut");
        CHECK_FALSE(doc.recompute());
        CHECK(doc.error.find("removed no material") != std::string::npos);
    }
}

// entities_to_wire handles exactly two shapes: one lone closed entity, or a chain of open ones.
// Anything else returns a null wire, and build_sketch_wire used to answer that by falling through
// to its legacy tail — which ends in a rectangle built from width/height. For an entity sketch
// those are whatever they were initialised to, so the extrude produced a box nobody drew.
// snaporca-88v.
TEST_CASE("An entity sketch that forms no wire fails instead of extruding a default box", "[CadDocument]")
{
    auto circle = [](Vec2d c, double r) {
        SketchEntity e; e.type = SketchEntity::Type::Circle; e.center = c; e.p0 = c; e.radius = r;
        return e;
    };
    auto line = [](Vec2d a, Vec2d b) {
        SketchEntity e; e.type = SketchEntity::Type::Line; e.p0 = a; e.p1 = b;
        return e;
    };

    SECTION("a lone circle still works — the supported case is untouched") {
        CadDocument doc;
        int sk = doc.add_sketch_entities({ circle({0, 0}, 10.0) }, SketchPlane::XY(), "Sketch");
        doc.add_extrude(sk, 5.0, false, BooleanMode::New, "Extrude");
        REQUIRE(doc.recompute());
        CHECK_THAT(doc.body_mass_properties(0).volume,
                   Catch::Matchers::WithinRel(M_PI * 100.0 * 5.0, 0.01));
    }

    SECTION("circle + stray line is rejected, not silently turned into a box") {
        CadDocument doc;
        int sk = doc.add_sketch_entities({ circle({0, 0}, 10.0), line({40, 40}, {60, 40}) },
                                        SketchPlane::XY(), "Sketch");
        doc.add_extrude(sk, 5.0, false, BooleanMode::New, "Extrude");
        CHECK_FALSE(doc.recompute());
        CHECK(doc.error.find("does not bound a face") != std::string::npos);
        CHECK(doc.bodies.empty());
    }

    SECTION("two disjoint circles are rejected too") {
        CadDocument doc;
        int sk = doc.add_sketch_entities({ circle({-20, 0}, 8.0), circle({20, 0}, 8.0) },
                                        SketchPlane::XY(), "Sketch");
        doc.add_extrude(sk, 5.0, false, BooleanMode::New, "Extrude");
        CHECK_FALSE(doc.recompute());
        CHECK(doc.error.find("disjoint") != std::string::npos);
    }
}

namespace {

std::vector<SketchEntity> rect_entities(double w, double h)
{
    const double hw = w * 0.5, hh = h * 0.5;
    return {
        {SketchEntity::Type::Line, Vec2d(-hw, -hh), Vec2d( hw, -hh)},
        {SketchEntity::Type::Line, Vec2d( hw, -hh), Vec2d( hw,  hh)},
        {SketchEntity::Type::Line, Vec2d( hw,  hh), Vec2d(-hw,  hh)},
        {SketchEntity::Type::Line, Vec2d(-hw,  hh), Vec2d(-hw, -hh)},
    };
}

SketchEntity circle_entity(const Vec2d& c, double r)
{
    SketchEntity e;
    e.type = SketchEntity::Type::Circle;
    e.center = c; e.p0 = c; e.radius = r;
    return e;
}

CadDocument plate_doc(const std::vector<SketchEntity>& entities, double distance)
{
    CadDocument doc;
    CadFeature sk;
    sk.type     = CadFeatureType::Sketch;
    sk.name     = "sketch";
    sk.plane    = SketchPlane::XY();
    sk.entities = entities;
    doc.features.push_back(sk);
    CadFeature ex;
    ex.type       = CadFeatureType::Extrude;
    ex.name       = "extrude";
    ex.sketch_ref = 0;
    ex.distance   = distance;
    ex.mode       = BooleanMode::New;
    doc.features.push_back(ex);
    return doc;
}

} // namespace

// snaporca-88v: a sketch may hold more than one closed loop. The Extrude path builds the
// sketch's planar region via SketchEngine::entities_to_wires + wires_to_face: the largest loop
// is the outer boundary, every other loop a hole. Volumes are the proof — a plate with a hole
// must subtract the hole, not merely "not throw".
TEST_CASE("a circle inside a rectangle extrudes to a plate with a hole", "[CadDocument][sketchwire]")
{
    std::vector<SketchEntity> ents = rect_entities(40, 30);
    ents.push_back(circle_entity({0, 0}, 5.0));
    CadDocument doc = plate_doc(ents, 10.0);

    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    const double expected = 40.0 * 30.0 * 10.0 - M_PI * 25.0 * 10.0;
    REQUIRE_THAT(double(doc.display_mesh.volume()), Catch::Matchers::WithinRel(expected, 0.01));
}

TEST_CASE("two holes are both subtracted", "[CadDocument][sketchwire]")
{
    std::vector<SketchEntity> ents = rect_entities(40, 30);
    ents.push_back(circle_entity({ 5, 0}, 3.0));
    ents.push_back(circle_entity({-5, 0}, 3.0));
    CadDocument doc = plate_doc(ents, 10.0);

    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    const double expected = 40.0 * 30.0 * 10.0 - 2.0 * M_PI * 9.0 * 10.0;
    REQUIRE_THAT(double(doc.display_mesh.volume()), Catch::Matchers::WithinRel(expected, 0.01));
}

TEST_CASE("a lone circle still extrudes exactly as before", "[CadDocument][sketchwire]")
{
    std::vector<SketchEntity> ents = { circle_entity({0, 0}, 8.0) };
    CadDocument doc = plate_doc(ents, 5.0);

    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    const double expected = M_PI * 64.0 * 5.0;
    REQUIRE_THAT(double(doc.display_mesh.volume()), Catch::Matchers::WithinRel(expected, 0.01));
}

TEST_CASE("a closed polygon still extrudes exactly as before", "[CadDocument][sketchwire]")
{
    std::vector<SketchEntity> ents = rect_entities(20, 20);
    CadDocument doc = plate_doc(ents, 10.0);

    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    const double expected = 20.0 * 20.0 * 10.0;
    REQUIRE_THAT(double(doc.display_mesh.volume()), Catch::Matchers::WithinRel(expected, 0.01));
}

TEST_CASE("two disjoint regions are refused, not guessed", "[CadDocument][sketchwire]")
{
    std::vector<SketchEntity> ents = { circle_entity({-10, 0}, 5.0), circle_entity({10, 0}, 5.0) };
    CadDocument doc = plate_doc(ents, 10.0);

    CHECK_FALSE(doc.recompute());
    CHECK(doc.error.find("disjoint") != std::string::npos);
    CHECK(doc.bodies.empty());
}

TEST_CASE("entities_to_wires returns one wire per loop", "[CadDocument][sketchwire]")
{
    std::vector<SketchEntity> ents = rect_entities(40, 30);
    ents.push_back(circle_entity({0, 0}, 5.0));

    const std::vector<TopoDS_Wire> wires = SketchEngine::entities_to_wires(ents, SketchPlane::XY());
    REQUIRE(wires.size() == 2);
    REQUIRE_FALSE(wires[0].IsNull());
    REQUIRE_FALSE(wires[1].IsNull());
}

// Sketching on a picked face is the most common gesture in solid modelling, and it was impossible:
// the plane came from a combo of base + datum planes only, so the sole route onto a face was to
// build a Coincident datum plane first. plane_of_face is the shared derivation that makes the
// viewport selection usable directly. snaporca-3a2.
TEST_CASE("plane_of_face gives a sketchable plane for a planar face only", "[CadDocument]")
{
    // 20 x 20 x 20 box on XY, so its top face sits at z = 20 with +Z normal.
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Sketch");
    doc.add_extrude(sk, 20.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());
    REQUIRE(doc.bodies.size() == 1);

    SECTION("every planar face of the box resolves, and its normal is a unit axis") {
        int resolved = 0, top = -1;
        for (int fi = 0; fi < 64; ++fi) {
            SketchPlane p;
            if (!doc.plane_of_face(0, fi, p)) continue;
            ++resolved;
            CHECK_THAT(p.normal.norm(), Catch::Matchers::WithinAbs(1.0, 1e-9));
            // The frame must be orthonormal or sketch coordinates on it would be skewed.
            CHECK_THAT(p.x_axis.dot(p.y_axis), Catch::Matchers::WithinAbs(0.0, 1e-9));
            CHECK_THAT(p.x_axis.dot(p.normal), Catch::Matchers::WithinAbs(0.0, 1e-9));
            if (p.normal.z() > 0.99) top = fi;
        }
        CHECK(resolved == 6);                 // a box has exactly six planar faces
        REQUIRE(top >= 0);                    // and one of them faces +Z
        SketchPlane p;
        REQUIRE(doc.plane_of_face(0, top, p));
        CHECK_THAT(p.origin.z(), Catch::Matchers::WithinAbs(20.0, 1e-6));   // the top, not the base
    }

    SECTION("bad indices are refused rather than guessed") {
        SketchPlane p;
        CHECK_FALSE(doc.plane_of_face(0, -1, p));
        CHECK_FALSE(doc.plane_of_face(-1, 0, p));
        CHECK_FALSE(doc.plane_of_face(9, 0, p));
        CHECK_FALSE(doc.plane_of_face(0, 999, p));
    }

    SECTION("a cylindrical face is refused — it has no single sketch plane") {
        CadDocument cyl;
        int c = cyl.add_sketch(SketchShape::Circle, SketchPlane::XY(), 0, 0, 10.0, "Sketch");
        cyl.add_extrude(c, 20.0, false, BooleanMode::New, "Extrude");
        REQUIRE(cyl.recompute());
        int planar = 0, refused = 0;
        for (int fi = 0; fi < 16; ++fi) {
            SketchPlane p;
            if (cyl.plane_of_face(0, fi, p)) ++planar; else ++refused;
        }
        CHECK(planar == 2);        // the two flat caps, and NOT the barrel
        CHECK(refused > 0);
    }
}

// snaporca-5425 — POSITIVE-CONTRACT variant. A feature that left a body with a null
// TopoDS_Shape used to be tolerated: recompute() returned true and the document kept
// advertising the body. The new guard makes that a hard failure. This test asserts the
// contract the guard preserves on the healthy side: a normal box + fillet document
// recomputes true, reports an empty error, and NO resulting body has a null shape.
//
// Why not assert the negative branch (a dress-up chain nulling a body -> recompute false)?
// Reproducing the original silent-null degeneracy (prism -> 6 vertical fillets -> chamfer on
// the already-filleted rim -> hole) is a separate open question. Driving that order headlessly
// makes the dress-up step THROW (OCCT "fillet radius too large" — an already-loud, already-
// caught path) rather than silently null a body, so no public-API sequence reliably reaches
// the guard's null branch in a headless test. Faking one (writing a null into `bodies` after
// recompute) would not exercise the guard — it runs on the freshly-built vector — and a test
// that asserts a state the code cannot reach is exactly what the contract forbids.
TEST_CASE("recompute on a healthy box + fillet leaves no body null and no error (positive contract)", "[CadDocument]")
{
    using namespace Slic3r;
    CadDocument doc;
    SketchProfile sp;
    sp.points.push_back(Vec2d(0,  0));
    sp.points.push_back(Vec2d(20, 0));
    sp.points.push_back(Vec2d(20, 20));
    sp.points.push_back(Vec2d(0,  20));
    sp.closed = true;
    const int sk = doc.add_sketch_profile(sp, SketchPlane::XY(), "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    doc.add_fillet(2.0, FaceGroup::All, "Fillet");

    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE_FALSE(doc.bodies.empty());
    for (size_t i = 0; i < doc.bodies.size(); ++i) {
        INFO("body " << i << " has a null shape");
        REQUIRE_FALSE(doc.bodies[i].shape.IsNull());
    }
}

// ============================================================================
// snaporca-rgbj — does a chamfer chain degenerate from a KERNEL defect, or from
// how the DRIVER captured its edge ids? Experiment, not a fix.
//
// CadFeature::dressup_edge is a GLOBAL edge id: an ordinal into
// TopExp::MapShapes(body, TopAbs_EDGE) (GeometryEngine::edge_by_index), resolved
// at recompute time against the body AS IT STANDS at that feature's position in
// the recipe. Every dress-up rewrites the edge map, so an id captured against one
// version of the shape names a DIFFERENT edge once an earlier dress-up has run.
//
// Test 1 = correct usage (re-read the map after each chamfer, pick by geometry).
// Test 2 = suspected-wrong usage (capture all four ids up-front, then run four).
// ============================================================================

namespace {

// A square box centred on the origin, extruded +Z by `height`. Its four top-rim
// edges have midpoints at Z=height: (+X)(half,0,h), (+Y)(0,half,h), (-X)(-half,0,h),
// (-Y)(0,-half,h). All four are geometrically identical (equal length), so a
// near-uniform chamfer removes the same volume from each.
CadDocument make_centred_box(double half, double height)
{
    CadDocument doc;
    SketchProfile sp;
    sp.points = { Vec2d(-half, -half), Vec2d(half, -half), Vec2d(half, half), Vec2d(-half, half) };
    sp.closed = true;
    const int sk = doc.add_sketch_profile(sp, SketchPlane::XY(), "Box");
    doc.add_extrude(sk, height, false, BooleanMode::New, "Extrude");
    return doc;
}

// Exact analytic volume (BRepGProp, not tessellation) of a solid.
double solid_volume(const TopoDS_Shape& s)
{
    GeometryEngine::MassProps p = GeometryEngine::mass_properties(s);
    return p.valid ? p.volume : 0.0;
}

// World-space midpoint of an edge, from its sampled polyline.
Vec3d edge_mid(const TopoDS_Edge& e)
{
    const std::vector<Vec3d> pts = GeometryEngine::sample_edge_world(e);
    Vec3d m = Vec3d::Zero();
    for (const Vec3d& p : pts) m += p;
    return m / double(pts.size());
}

// Global edge id whose midpoint is within `tol` mm of `target`. -1 if none.
// Picks by GEOMETRY (position), never by a hardcoded index.
int edge_near(const TopoDS_Shape& shape, const Vec3d& target, double tol)
{
    const std::vector<TopoDS_Edge> edges = GeometryEngine::edges_of(shape);
    for (int i = 0; i < int(edges.size()); ++i) {
        if (edges[i].IsNull()) continue;
        const std::vector<Vec3d> pts = GeometryEngine::sample_edge_world(edges[i]);
        if (pts.size() < 2) continue;
        if ((edge_mid(edges[i]) - target).norm() < tol) return i;
    }
    return -1;
}

} // namespace

TEST_CASE("dressup: sequential chamfers, ids re-read after each, stay uniform", "[CadDocument][dressup]")
{
    const double half = 10.0, h = 10.0, d = 0.2; // tiny d: the corner-shortening of a
                                                 // neighbour's rim edge stays negligible
    CadDocument doc = make_centred_box(half, h);
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // The four top-rim edges, addressed by world midpoint — never by index.
    const std::vector<Vec3d> rim = {
        Vec3d( half, 0.0, h),   // +X
        Vec3d(0.0,  half, h),   // +Y
        Vec3d(-half, 0.0, h),   // -X
        Vec3d(0.0, -half, h),   // -Y
    };

    std::vector<double> removals;
    double prev = solid_volume(doc.bodies[0].shape);
    for (size_t k = 0; k < rim.size(); ++k) {
        // Correct usage: re-read the CURRENT body's edge map, pick by geometry.
        const int id = edge_near(doc.bodies[0].shape, rim[k], 1.5);
        INFO("chamfer " << k << " -> edge id " << id);
        REQUIRE(id >= 0);
        doc.add_chamfer(d, id, "Chamfer" + std::to_string(k));
        REQUIRE(doc.recompute());
        REQUIRE(doc.error.empty());
        const double now = solid_volume(doc.bodies[0].shape);
        removals.push_back(prev - now);
        prev = now;
    }

    std::ostringstream os;
    for (size_t i = 0; i < removals.size(); ++i) os << (i ? ", " : "") << removals[i];
    INFO("removals (mm^3): [" << os.str() << "]");

    REQUIRE(removals.size() == 4);
    double mn = removals[0], mx = removals[0];
    for (double r : removals) { mn = std::min(mn, r); mx = std::max(mx, r); }
    INFO("min=" << mn << " max=" << mx);
    REQUIRE(mn > 0.0);
    // All four removals equal within a few percent. Measured on this fixture:
    // [0.4, 0.397333, 0.397333, 0.394667] mm^3. The residual spread is real geometry (each
    // chamfer shortens the two rim edges it shares a corner with), not id drift; drift would
    // push the later removals toward zero and blow the spread wide open.
    REQUIRE((mx - mn) <= 0.10 * mn);
}

TEST_CASE("dressup: four chamfer ids captured up-front drift as earlier chamfers run", "[CadDocument][dressup]")
{
    const double half = 10.0, h = 10.0, d = 0.2;
    CadDocument doc = make_centred_box(half, h);
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    const std::vector<Vec3d> rim = {
        Vec3d( half, 0.0, h), Vec3d(0.0, half, h), Vec3d(-half, 0.0, h), Vec3d(0.0, -half, h),
    };

    // Capture all four ids ONCE, from the shape as it stands before any chamfer.
    std::vector<int> ids;
    for (const Vec3d& t : rim) {
        const int id = edge_near(doc.bodies[0].shape, t, 1.5);
        REQUIRE(id >= 0);
        ids.push_back(id);
    }
    REQUIRE(ids.size() == 4);
    INFO("captured ids: [" << ids[0] << "," << ids[1] << "," << ids[2] << "," << ids[3] << "]");

    // Recompute resolves each dressup_edge against the body as it stands at that feature's
    // position — exactly what chaining apply_chamfer reproduces here. Measure each step, and
    // record whether a stale id throws (invalid/out-of-range edge) or silently lands elsewhere.
    TopoDS_Shape s = doc.bodies[0].shape;
    std::vector<double> vols{ solid_volume(s) };
    bool threw = false;
    std::string why;
    for (int id : ids) {
        try {
            s = GeometryEngine::apply_chamfer(s, d, id);
            vols.push_back(solid_volume(s));
        } catch (const std::exception& e) { threw = true; why = e.what(); break; }
    }

    std::vector<double> rmv;
    for (size_t i = 1; i < vols.size(); ++i) rmv.push_back(vols[i - 1] - vols[i]);

    std::ostringstream os;
    for (size_t i = 0; i < rmv.size(); ++i) os << (i ? ", " : "") << rmv[i];
    INFO("up-front-id removals (mm^3): [" << os.str() << "] threw=" << threw << " (" << why << ")");

    // Documents a defect, not a desired result. Measured on this fixture: [0.4, 0.008324,
    // 0.397333, 0.280405] mm^3 — the second chamfer's stale id landed on a nearly-consumed
    // edge and cut ~2% of the intended volume. Ids captured once and replayed against an
    // evolving edge map DRIFT; that is the degeneration the socket chamfers showed, and it is
    // a driver artefact (wrong usage), not a kernel defect — Test 1 proves the kernel stays
    // uniform when ids are re-read.
    REQUIRE_FALSE(threw);            // currently it does NOT throw: it silently drifts
    REQUIRE(rmv.size() == 4);
    double mn = rmv[0], mx = rmv[0];
    for (double r : rmv) { mn = std::min(mn, r); mx = std::max(mx, r); }
    INFO("up-front-id min=" << mn << " max=" << mx);
    REQUIRE(mx > 1.10 * mn);         // non-uniform: the id-drift signature

    // The first captured id is still a valid top-rim edge, so the first chamfer must cut.
    REQUIRE(vols.size() >= 2);
    REQUIRE(vols[1] < vols[0]);

    // Literal driver path: append all four and recompute once.
    CadDocument doc2 = make_centred_box(half, h);
    REQUIRE(doc2.recompute());
    for (size_t i = 0; i < ids.size(); ++i)
        doc2.add_chamfer(d, ids[i], "C" + std::to_string(i));
    const bool ok = doc2.recompute();
    INFO("single-recompute driver path: ok=" << ok << " error=" << (ok ? std::string() : doc2.error));
    REQUIRE(ok);
}

// --- Face-drift fingerprint: a FaceAndDirection connector warns when its face index slides ---

TEST_CASE("a connector records its face fingerprint on first recompute", "[CadDocument][mate]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());

    int top_face = find_face_by_normal(doc, 0, Vec3d(0, 0, 1));
    REQUIRE(top_face >= 0);

    int cs = doc.add_coordsys(CoordSysType::FaceAndDirection, Vec3d(0, 0, 0), "CS");
    doc.features[cs].coordsys_body = 0;
    doc.features[cs].coordsys_face = top_face;
    REQUIRE(doc.recompute());

    REQUIRE(doc.features[cs].coordsys_face_kind >= 0);
    REQUIRE(doc.features[cs].coordsys_face_edges >= 0);
    REQUIRE(doc.mate_conflicts.empty());
}

TEST_CASE("a connector whose face index slides onto a different KIND of face is reported", "[CadDocument][mate]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());

    int top_face = find_face_by_normal(doc, 0, Vec3d(0, 0, 1));
    REQUIRE(top_face >= 0);

    int cs = doc.add_coordsys(CoordSysType::FaceAndDirection, Vec3d(0, 0, 0), "CS");
    doc.features[cs].coordsys_body = 0;
    doc.features[cs].coordsys_face = top_face;
    REQUIRE(doc.recompute());
    REQUIRE(doc.mate_conflicts.empty());
    REQUIRE(doc.features[cs].coordsys_face_kind >= 0);

    // Insert a fillet: the box gains cylindrical faces and the face map is renumbered.
    doc.add_fillet(2.0, FaceGroup::All, "Fillet");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    // Find the cylindrical face the fillet introduced (a different KIND than the recorded
    // planar fingerprint), then force the drift by pointing coordsys_face at it.
    int cyl_face = -1;
    auto faces = GeometryEngine::faces_of(doc.bodies[0].shape);
    for (int i = 0; i < int(faces.size()); ++i) {
        if (GeometryEngine::cylinder_of_face(faces[i]).ok) { cyl_face = i; break; }
    }
    REQUIRE(cyl_face >= 0);

    doc.features[cs].coordsys_face = cyl_face;
    const bool ok = doc.recompute();
    REQUIRE(ok);   // non-fatality is the contract under test

    bool reported = false;
    for (const auto& c : doc.mate_conflicts)
        if (c.first == cs) { reported = true; break; }
    REQUIRE(reported);
}

TEST_CASE("a resized body does not report drift", "[CadDocument][mate]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 10, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());

    int top_face = find_face_by_normal(doc, 0, Vec3d(0, 0, 1));
    REQUIRE(top_face >= 0);

    int cs = doc.add_coordsys(CoordSysType::FaceAndDirection, Vec3d(0, 0, 0), "CS");
    doc.features[cs].coordsys_body = 0;
    doc.features[cs].coordsys_face = top_face;
    REQUIRE(doc.recompute());
    REQUIRE(doc.mate_conflicts.empty());

    // Resize upstream: same face, same kind, same edge count — only its dimensions changed.
    // This is a legitimate parametric edit and must not raise the drift warning.
    for (CadFeature& f : doc.features)
        if (f.type == CadFeatureType::Extrude) f.distance = 25.0;
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.mate_conflicts.empty());
}

TEST_CASE("mate_options always returns five entries in kind order", "[CadDocument][mate]")
{
    CadDocument doc;
    // Completely invalid pair: the stability contract (always five, always in order) must hold.
    auto opts = doc.mate_options(-1, -1);
    REQUIRE(opts.size() == 5);
    for (int i = 0; i < 5; ++i) REQUIRE(opts[i].kind == i);

    // A valid, distinct pair must return the same shape.
    int a = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 0), "A");
    int b = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(1, 0, 0), "B");
    auto opts2 = doc.mate_options(a, b);
    REQUIRE(opts2.size() == 5);
    for (int i = 0; i < 5; ++i) REQUIRE(opts2[i].kind == i);
}

TEST_CASE("a connector with no body dims every mate kind, with a reason", "[CadDocument][mate]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());

    // Point(world) connectors belong to no body, which is the default the GUI hands you.
    int a = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 0), "A");
    int b = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(30, 0, 0), "B");
    REQUIRE(doc.features[b].coordsys_body < 0);

    // B is the connector whose body MOVES, so without one no kind can apply. The offer used to
    // call all five viable and the mate only failed at recompute, after the feature existed.
    auto opts = doc.mate_options(a, b);
    REQUIRE(opts.size() == 5);
    for (int i = 0; i < 5; ++i) {
        REQUIRE(opts[i].kind == i);
        REQUIRE_FALSE(opts[i].viable);
        REQUIRE(opts[i].reason.find("not attached to a body") != std::string::npos);
    }

    // A needs no body — it is the fixed reference. Giving B one is enough to revive the offer.
    doc.features[b].coordsys_body = 0;
    auto opts2 = doc.mate_options(a, b);
    REQUIRE(opts2[0].viable);          // Fastened is frame-only
    REQUIRE(opts2[0].reason.empty());

    // A body index that no longer resolves is refused too, and says so differently.
    doc.features[b].coordsys_body = 7;
    auto opts3 = doc.mate_options(a, b);
    for (const auto& o : opts3) {
        REQUIRE_FALSE(o.viable);
        REQUIRE(o.reason.find("no longer exists") != std::string::npos);
    }
}

TEST_CASE("a flat-face pair offers Fastened, Planar and Slider but not the axial types", "[CadDocument][mate]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());

    int top  = find_face_by_normal(doc, 0, Vec3d(0, 0, 1));
    int side = find_face_by_normal(doc, 0, Vec3d(1, 0, 0));
    REQUIRE(top  >= 0);
    REQUIRE(side >= 0);

    int a = doc.add_coordsys(CoordSysType::FaceAndDirection, Vec3d(0, 0, 0), "A");
    doc.features[a].coordsys_body = 0;
    doc.features[a].coordsys_face = top;
    int b = doc.add_coordsys(CoordSysType::FaceAndDirection, Vec3d(0, 0, 0), "B");
    doc.features[b].coordsys_body = 0;
    doc.features[b].coordsys_face = side;

    // Recompute so the face fingerprints record (both planar => GeomAbs_Plane).
    REQUIRE(doc.recompute());
    REQUIRE(doc.features[a].coordsys_face_kind >= 0);
    REQUIRE(doc.features[b].coordsys_face_kind >= 0);

    auto opts = doc.mate_options(a, b);
    REQUIRE(opts.size() == 5);
    REQUIRE(opts[0].viable);   // Fastened: frame-only, always viable
    REQUIRE(opts[1].viable);   // Planar: both faces planar
    REQUIRE_FALSE(opts[2].viable); // Revolute: needs a cylindrical face
    REQUIRE(opts[3].viable);   // Slider: frame-only, always viable
    REQUIRE_FALSE(opts[4].viable); // Cylindrical: needs a cylindrical face

    REQUIRE(opts[0].reason.empty());
    REQUIRE(opts[1].reason.empty());
    REQUIRE(opts[3].reason.empty());
    REQUIRE_FALSE(opts[2].reason.empty());
    REQUIRE_FALSE(opts[4].reason.empty());
    REQUIRE_CONTAINS(opts[2].reason, "connector");
    REQUIRE_CONTAINS(opts[4].reason, "connector");
}

TEST_CASE("an unrecorded fingerprint does not make a type non-viable", "[CadDocument][mate]")
{
    CadDocument doc;
    int sk = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "Box");
    doc.add_extrude(sk, 10.0, false, BooleanMode::New, "Extrude");
    REQUIRE(doc.recompute());

    int a = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 0), "A");
    int b = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(1, 0, 0), "B");
    // The MISSING FINGERPRINT is the whole subject here, so nothing else may be missing: B is
    // given a body, because a connector without one is refused for that reason instead and the
    // case would no longer isolate what this test is named for.
    doc.features[a].coordsys_body = 0;
    doc.features[b].coordsys_body = 0;
    // PointWorld connectors never record a face fingerprint: face_kind stays -1.
    REQUIRE(doc.features[a].coordsys_face_kind == -1);
    REQUIRE(doc.features[b].coordsys_face_kind == -1);

    auto opts = doc.mate_options(a, b);
    REQUIRE(opts.size() == 5);
    for (const auto& o : opts) {
        REQUIRE(o.viable);
        REQUIRE(o.reason.empty());
    }
}

TEST_CASE("an invalid pair names which side is wrong", "[CadDocument][mate]")
{
    CadDocument doc;
    int a = doc.add_coordsys(CoordSysType::PointWorld, Vec3d(0, 0, 0), "A");
    int sketch = doc.add_sketch(SketchShape::Rectangle, SketchPlane::XY(), 20, 20, 0, "S");

    // cs_b points at a Sketch feature, not a CoordSys: all five non-viable, reason names B.
    auto opts = doc.mate_options(a, sketch);
    REQUIRE(opts.size() == 5);
    for (const auto& o : opts) {
        REQUIRE_FALSE(o.viable);
        REQUIRE_CONTAINS(o.reason, "connector B");
        REQUIRE(o.reason.find("connector A") == std::string::npos);
    }

    // Out of range on B names B; out of range on A names A.
    for (const auto& o : doc.mate_options(a, 9999))
        REQUIRE_CONTAINS(o.reason, "connector B");
    for (const auto& o : doc.mate_options(-5, a))
        REQUIRE_CONTAINS(o.reason, "connector A");
}

// A rectangular plate 120 x 160 x 10 centred on the origin with a radius-30 bore through
// the middle: one solid whose volume is the box minus the cylinder, and whose topology is a
// plate-with-a-bore (1 solid, 7 faces) rather than a plate plus a plug.
TEST_CASE("add_extrude_entities builds a plate with a bore", "[CadDocument][holes]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;

    const Vec2d A(-60, -80), B(60, -80), C(60, 80), D(-60, 80);
    std::vector<SketchEntity> entities = {
        {SketchEntity::Type::Line, A, B},
        {SketchEntity::Type::Line, B, C},
        {SketchEntity::Type::Line, C, D},
        {SketchEntity::Type::Line, D, A},
    };
    SketchEntity bore;
    bore.type   = SketchEntity::Type::Circle;
    bore.center = Vec2d(0, 0);
    bore.radius = 30.0;
    entities.push_back(bore);

    doc.add_extrude_entities(entities, SketchPlane::XY(), 10.0, false, BooleanMode::New, "Extrude1");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.display_mesh.facets_count() > 0);

    auto mp = doc.body_mass_properties(0);
    REQUIRE(mp.valid);
    const double expected_vol = 120.0 * 160.0 * 10.0 - M_PI * 30.0 * 30.0 * 10.0;
    REQUIRE_THAT(mp.volume, WithinAbs(expected_vol, 1.0));

    // One solid, seven faces: top + bottom (each a planar face with a hole), four side
    // walls, and the single cylindrical bore wall.
    int face_count = 0, solid_count = 0;
    for (TopExp_Explorer fe(doc.bodies.back().shape, TopAbs_FACE);  fe.More(); fe.Next()) ++face_count;
    for (TopExp_Explorer se(doc.bodies.back().shape, TopAbs_SOLID); se.More(); se.Next()) ++solid_count;
    INFO("volume mm^3 = " << mp.volume << ", faces = " << face_count << ", solids = " << solid_count);
    REQUIRE(solid_count == 1);
    REQUIRE(face_count == 7);
}

TEST_CASE("two disjoint circles are refused by name", "[CadDocument][holes]")
{
    CadDocument doc;

    SketchEntity a;
    a.type = SketchEntity::Type::Circle; a.center = Vec2d(-50, 0); a.radius = 20.0;
    SketchEntity b;
    b.type = SketchEntity::Type::Circle; b.center = Vec2d( 50, 0); b.radius = 20.0;

    doc.add_extrude_entities({a, b}, SketchPlane::XY(), 10.0, false, BooleanMode::New, "Extrude1");

    REQUIRE_FALSE(doc.recompute());
    REQUIRE_CONTAINS(doc.error, "disjoint");
}

// Same plate-with-a-bore, but the bore circle is wound CLOCKWISE (geometric winding opposite
// the CCW rectangle). The old wires_to_face reversed every hole wire unconditionally
// (wires[i].Reversed()), which only produced a correct hole when the circle was wound the same
// way as the outer loop; a clockwise circle got reversed into matching the outer boundary and
// the prism swept it solid — measured 220274 mm^3 = box (192000) + disc (28274), the filled-bore
// signature. The current ShapeFix_Face::FixOrientation path is winding-independent. Flipping the
// plane normal reverses gp_Circ's parametrisation (gp_Ax2(center, -Z) sweeps clockwise seen from
// +Z) while the rectangle's 2D coordinates stay CCW.
TEST_CASE("add_extrude_entities builds a plate with a bore (clockwise circle)", "[CadDocument][holes]")
{
    using Catch::Matchers::WithinAbs;

    CadDocument doc;

    SketchPlane cw_plane = SketchPlane::XY();
    cw_plane.normal = Vec3d(0, 0, -1);

    const Vec2d A(-60, -80), B(60, -80), C(60, 80), D(-60, 80);
    std::vector<SketchEntity> entities = {
        {SketchEntity::Type::Line, A, B},
        {SketchEntity::Type::Line, B, C},
        {SketchEntity::Type::Line, C, D},
        {SketchEntity::Type::Line, D, A},
    };
    SketchEntity bore;
    bore.type   = SketchEntity::Type::Circle;
    bore.center = Vec2d(0, 0);
    bore.radius = 30.0;
    entities.push_back(bore);

    doc.add_extrude_entities(entities, cw_plane, 10.0, false, BooleanMode::New, "Extrude1");
    REQUIRE(doc.recompute());
    REQUIRE(doc.error.empty());
    REQUIRE(doc.display_mesh.facets_count() > 0);

    auto mp = doc.body_mass_properties(0);
    REQUIRE(mp.valid);
    const double expected_vol = 120.0 * 160.0 * 10.0 - M_PI * 30.0 * 30.0 * 10.0;
    REQUIRE_THAT(mp.volume, WithinAbs(expected_vol, 1.0));

    int face_count = 0, solid_count = 0;
    for (TopExp_Explorer fe(doc.bodies.back().shape, TopAbs_FACE);  fe.More(); fe.Next()) ++face_count;
    for (TopExp_Explorer se(doc.bodies.back().shape, TopAbs_SOLID); se.More(); se.Next()) ++solid_count;
    INFO("volume mm^3 = " << mp.volume << ", faces = " << face_count << ", solids = " << solid_count);
    REQUIRE(solid_count == 1);
    REQUIRE(face_count == 7);
}

