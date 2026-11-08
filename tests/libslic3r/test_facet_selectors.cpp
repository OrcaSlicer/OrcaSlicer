#include <catch2/catch_all.hpp>

#include <nlohmann/json.hpp>

#include "libslic3r/FacetSelectors.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using namespace Slic3r::FacetSelectors;
using json = nlohmann::json;
using Catch::Approx;

namespace {

// One 20 mm cube object with a single instance, optionally rotated / scaled / mirrored.
struct CubeModel
{
    Model        model;
    ModelObject *object = nullptr;

    explicit CubeModel(const Vec3d &rotation = Vec3d::Zero(), const Vec3d &scale = Vec3d::Ones(), const Vec3d &mirror = Vec3d::Ones())
    {
        object = model.add_object();
        object->add_volume(make_cube(20., 20., 20.));
        ModelInstance *inst = object->add_instance();
        inst->set_rotation(rotation);
        inst->set_scaling_factor(scale);
        inst->set_mirror(mirror);
    }

    ModelVolume &volume() { return *object->volumes.front(); }
    EvalContext  ctx() const { return object_geometry(*object, 0); }
};

std::vector<size_t> hits(const Selector &s, const EvalContext &ctx, size_t volume = 0)
{
    std::vector<size_t> out;
    auto                masks = evaluate(s, ctx);
    for (size_t f = 0; f < masks[volume].size(); ++f)
        if (masks[volume][f])
            out.push_back(f);
    return out;
}

// Facets of the volume whose MESH-frame normal points along `dir`.
std::vector<size_t> mesh_facets_facing(const ModelVolume &mv, const Vec3f &dir)
{
    std::vector<size_t> out;
    for (size_t f = 0; f < mv.mesh().its.indices.size(); ++f)
        if (its_face_normal(mv.mesh().its, int(f)).dot(dir) > 0.99f)
            out.push_back(f);
    return out;
}

SelectorPtr parse(const json &j)
{
    ParseError err;
    SelectorPtr s = parse_selector(j, &err);
    INFO(err.path << ": " << err.message);
    REQUIRE(s);
    return s;
}

// Copy an annotation through the 3MF per-triangle string form into `dst`.
void copy_through_strings(const FacetsAnnotation &src, FacetsAnnotation &dst, int n_triangles)
{
    dst.reset();
    dst.reserve(n_triangles);
    for (int i = 0; i < n_triangles; ++i) {
        std::string s = src.get_triangle_as_string(i);
        if (!s.empty())
            dst.set_triangle_from_string(i, s);
    }
    dst.shrink_to_fit();
}

} // namespace

TEST_CASE("Overhang selector on a cube rotated 45 degrees picks exactly the bottom-facing facets", "[facets]")
{
    CubeModel   cube(Vec3d(PI / 4., 0., 0.));
    EvalContext ctx = cube.ctx();
    REQUIRE(ctx.volumes.size() == 1);
    const VolumeGeometry &g = ctx.volumes.front();

    // Rotated about X, the original bottom and front faces both face 45 degrees below horizontal.
    std::vector<size_t> expected;
    for (size_t f = 0; f < g.size(); ++f)
        if (g.normal(f).z() < -0.5)
            expected.push_back(f);
    REQUIRE(expected.size() == 4);

    SECTION("a 50 degree limit takes both downward faces") {
        CHECK(hits(*select_overhang(50.), ctx) == expected);
    }
    SECTION("a 40 degree limit takes nothing (the faces sit at 45)") {
        CHECK(hits(*select_overhang(40.), ctx).empty());
    }
    SECTION("90 degrees = every downward-facing facet, never a side or top facet") {
        CHECK(hits(*select_overhang(90.), ctx) == expected);
    }
    SECTION("geometry_query overhangs reports their area") {
        json q = geometry_query(ctx, "overhangs", {{"maxAngleDeg", 50}});
        REQUIRE(q["ok"] == true);
        CHECK(q["facets"] == 4);
        CHECK(q["areaMm2"].get<double>() == Approx(800.));
        CHECK(q["regionCount"] == 1); // the two faces share an edge
    }
}

TEST_CASE("Side selectors pick the matching face of a cube", "[facets]")
{
    CubeModel   cube;
    EvalContext ctx = cube.ctx();

    SECTION("rear = +Y, 2 facets") {
        auto s = parse({{"normal", {{"side", "rear"}, {"withinDeg", 30}}}});
        CHECK(hits(*s, ctx) == mesh_facets_facing(cube.volume(), Vec3f::UnitY()));
        CHECK(hits(*s, ctx).size() == 2);
    }
    SECTION("each named side") {
        const std::pair<const char *, Vec3f> sides[] = {{"front", -Vec3f::UnitY()}, {"left", -Vec3f::UnitX()}, {"right", Vec3f::UnitX()},
                                                        {"top", Vec3f::UnitZ()},    {"bottom", -Vec3f::UnitZ()}};
        for (const auto &[name, dir] : sides) {
            INFO(name);
            CHECK(hits(*parse({{"normal", {{"side", name}}}}), ctx) == mesh_facets_facing(cube.volume(), dir));
        }
    }
    SECTION("explicit direction") {
        CHECK(hits(*parse({{"normal", {{"direction", {0, 5, 0}}, {"withinDeg", 1}}}}), ctx).size() == 2);
    }
    SECTION("rear but not top: combinators") {
        auto s = parse({{"all", {{{"normal", {{"side", "rear"}, {"withinDeg", 60}}}}, {{"not", {{"normal", {{"side", "top"}, {"withinDeg", 30}}}}}}}}});
        CHECK(hits(*s, ctx) == mesh_facets_facing(cube.volume(), Vec3f::UnitY()));
        auto any = parse({{"any", {{{"normal", {{"side", "rear"}}}}, {{"normal", {{"side", "front"}}}}}}});
        CHECK(hits(*any, ctx).size() == 4);
        CHECK(hits(*parse({{"all", json::object()}}), ctx).size() == 12);
        CHECK(hits(*parse({{"not", {{"all", json::object()}}}}), ctx).empty());
    }
}

TEST_CASE("Normals follow mirroring and non-uniform scale into the world frame", "[facets]")
{
    // Mirrored in X, the mesh's +X face now faces world -X.
    CubeModel mirrored(Vec3d::Zero(), Vec3d::Ones(), Vec3d(-1., 1., 1.));
    CHECK(hits(*select_normal(-Vec3d::UnitX(), 10.), mirrored.ctx()) == mesh_facets_facing(mirrored.volume(), Vec3f::UnitX()));

    // A volume rotated 45 degrees about X inside an instance stretched 3x in Z is sheared: its two
    // downward faces end up 71.6 degrees from straight down (inverse-transpose normals), while
    // naively transformed normals would put them at 18.4 degrees.
    CubeModel sheared(Vec3d::Zero(), Vec3d(1., 1., 3.));
    sheared.volume().set_rotation(Vec3d(PI / 4., 0., 0.));
    EvalContext ctx = sheared.ctx();
    for (size_t f = 0; f < ctx.volumes[0].size(); ++f)
        CHECK(ctx.volumes[0].normal(f).norm() == Approx(1.));
    CHECK(hits(*select_overhang(45.), ctx).empty());
    CHECK(hits(*select_overhang(70.), ctx).empty());
    CHECK(hits(*select_overhang(73.), ctx).size() == 4);
}

TEST_CASE("Spatial selectors test centroids by default, or all / any corners", "[facets]")
{
    CubeModel   cube;
    EvalContext ctx = cube.ctx();
    BoundingBoxf3 bb = ctx.volumes[0].bounding_box();
    REQUIRE(bb.size().x() == Approx(20.));
    const double z0 = bb.min.z();

    // Side facets have centroids at 1/3 or 2/3 of the height; the bottom face sits at z0.
    CHECK(hits(*select_z_range(z0 - 1., z0 + 1.), ctx) == mesh_facets_facing(cube.volume(), -Vec3f::UnitZ()));
    CHECK(hits(*select_z_range(z0 - 1., z0 + 1., PointTest::Any), ctx).size() == 10);  // all but the top
    CHECK(hits(*select_z_range(z0 - 1., z0 + 21., PointTest::All), ctx).size() == 12);
    CHECK(hits(*parse({{"zRange", {{"max", z0 + 1.}}}}), ctx).size() == 2);

    const Vec3d c = bb.center();
    CHECK(hits(*select_halfspace(c + Vec3d(0., 9., 0.), Vec3d::UnitY()), ctx) == mesh_facets_facing(cube.volume(), Vec3f::UnitY()));
    CHECK(hits(*select_box(bb.min - Vec3d::Ones(), bb.max + Vec3d::Ones()), ctx).size() == 12);
    CHECK(hits(*select_sphere(c, 5.), ctx).empty());
    CHECK(hits(*select_sphere(c, 50.), ctx).size() == 12);
}

TEST_CASE("Planar regions flood from a seed side or point", "[facets]")
{
    CubeModel   cube;
    EvalContext ctx = cube.ctx();
    CHECK(hits(*parse({{"planarRegion", {{"seedSide", "top"}}}}), ctx) == mesh_facets_facing(cube.volume(), Vec3f::UnitZ()));

    const BoundingBoxf3 bb = ctx.volumes[0].bounding_box();
    const Vec3d on_left(bb.min.x() - 0.5, bb.center().y(), bb.center().z());
    CHECK(hits(*select_planar_region_by_point(on_left, 5.), ctx) == mesh_facets_facing(cube.volume(), -Vec3f::UnitX()));

    // 90 degrees of tolerance floods from the top over every side wall, but not to the bottom.
    CHECK(hits(*select_planar_region_by_direction(Vec3d::UnitZ(), 90.), ctx).size() == 10);

    json q = geometry_query(ctx, "planarRegions", json::object());
    REQUIRE(q["ok"] == true);
    CHECK(q["regionCount"] == 6);
    CHECK(q["regions"][0]["areaMm2"].get<double>() == Approx(400.));
    CHECK(q["regions"][0]["indices"].size() == 2);
}

TEST_CASE("Smooth regions follow a curved wall where planar regions stop", "[facets]")
{
    // A 64-sided cylinder: neighbouring wall facets turn by 5.6 degrees.
    Model        model;
    ModelObject *object = model.add_object();
    object->add_volume(TriangleMesh(its_make_cylinder(10., 20., 2. * PI / 64.)));
    object->add_instance();
    const EvalContext ctx = object_geometry(*object, 0);

    const size_t planar = hits(*select_planar_region_by_direction(-Vec3d::UnitY(), 5.), ctx).size();
    const size_t smooth = hits(*select_smooth_region_by_direction(-Vec3d::UnitY(), 10.), ctx).size();
    CHECK(planar <= 4);
    // The whole wall (64 sectors, two triangles each), but neither cap: the cap edge is a 90 degree turn.
    CHECK(smooth == 2 * size_t(std::ceil(2. * PI / (2. * PI / 64.))));
    CHECK(hits(*select_all_of({select_smooth_region_by_direction(-Vec3d::UnitY(), 10.), select_normal(Vec3d::UnitZ(), 10.)}), ctx).empty());
    CHECK(hits(*parse({{"smoothRegion", {{"seedSide", "front"}, {"maxNeighbourAngleDeg", 10}}}}), ctx).size() == smooth);
}

TEST_CASE("Selector parse errors carry the JSON path", "[facets]")
{
    auto err_of = [](const json &j) {
        ParseError e;
        CHECK_FALSE(parse_selector(j, &e));
        return e;
    };
    CHECK(err_of({{"normal", {{"side", "up"}}}}).path == "where.normal.side");
    CHECK(err_of({{"all", {{{"overhang", json::object()}}}}}).path == "where.all[0].overhang.maxAngleDeg");
    CHECK(err_of({{"overhang", {{"maxAngleDeg", 120}}}}).path == "where.overhang.maxAngleDeg");
    CHECK(err_of({{"bogus", 1}}).path == "where.bogus");
    CHECK(err_of({{"zRange", {5, 1}}}).path == "where.zRange");
    CHECK(err_of({{"box", {{"min", {0, 0}}, {"max", {1, 1, 1}}}}}).path == "where.box.min");
    CHECK(err_of({{"normal", {{"side", "rear"}, {"extra", 1}}}}).path == "where.normal.extra");
    CHECK(err_of(json::array()).path == "where");

    json deep = {{"all", json::object()}};
    for (int i = 0; i < 20; ++i)
        deep = {{"not", deep}};
    CHECK_FALSE(err_of(deep).message.empty());
}

TEST_CASE("Paint states parse per layer", "[facets]")
{
    EnforcerBlockerType t;
    std::string         e;
    CHECK(parse_paint_state("enforce", PaintLayer::Support, t, e));
    CHECK(t == EnforcerBlockerType::ENFORCER);
    CHECK(parse_paint_state("block", PaintLayer::Seam, t, e));
    CHECK(t == EnforcerBlockerType::BLOCKER);
    CHECK_FALSE(parse_paint_state("block", PaintLayer::Fuzzy, t, e));
    CHECK(parse_paint_state("erase", PaintLayer::Color, t, e));
    CHECK(t == EnforcerBlockerType::NONE);
    CHECK_FALSE(parse_paint_state("enforce", PaintLayer::Color, t, e));
    CHECK(parse_paint_state({{"filament", 3}}, PaintLayer::Color, t, e));
    CHECK(int(t) == 3);
    CHECK_FALSE(parse_paint_state({{"filament", 0}}, PaintLayer::Color, t, e));
    CHECK_FALSE(parse_paint_state({{"filament", 33}}, PaintLayer::Color, t, e));
    CHECK_FALSE(parse_paint_state({{"filament", 2}}, PaintLayer::Seam, t, e));
}

TEST_CASE("Add, replace and clear round-trip through the FacetsAnnotation serialization", "[facets]")
{
    CubeModel    cube;
    ModelVolume &mv  = cube.volume();
    EvalContext  ctx = cube.ctx();
    const int    n   = int(mv.mesh().its.indices.size());
    auto rear  = select_normal(Vec3d::UnitY(), 10.);
    auto front = select_normal(-Vec3d::UnitY(), 10.);

    // A second volume to receive the 3MF-string copy.
    CubeModel copy;

    SECTION("dry run reports but does not write") {
        PaintResult r = paint_volume(mv, ctx, 0, PaintLayer::Seam, EnforcerBlockerType::ENFORCER, PaintMode::Add, *rear, true);
        CHECK(r.hits == 2);
        CHECK(r.hit_area_mm2 == Approx(400.));
        CHECK_FALSE(r.changed);
        CHECK(r.states[int(EnforcerBlockerType::ENFORCER)].facets == 2);
        CHECK(mv.seam_facets.empty());
    }

    SECTION("add, then replace, then clear") {
        PaintResult r = paint_volume(mv, ctx, 0, PaintLayer::Seam, EnforcerBlockerType::ENFORCER, PaintMode::Add, *rear, false);
        CHECK(r.changed);
        CHECK(mv.seam_facets.has_facets(mv, EnforcerBlockerType::ENFORCER));
        CHECK(stats_to_json(PaintLayer::Seam, r.states)["enforce"]["areaMm2"].get<double>() == Approx(400.));

        // Blockers on the front coexist with rear enforcers.
        paint_volume(mv, ctx, 0, PaintLayer::Seam, EnforcerBlockerType::BLOCKER, PaintMode::Add, *front, false);
        StateStatsMap s = annotation_stats(mv.mesh(), ctx.volumes[0].world(), mv.seam_facets.get_data());
        CHECK(s[int(EnforcerBlockerType::ENFORCER)].facets == 2);
        CHECK(s[int(EnforcerBlockerType::BLOCKER)].facets == 2);

        // Round trip through the 3MF string form keeps it identical.
        copy_through_strings(mv.seam_facets, copy.volume().seam_facets, n);
        CHECK(copy.volume().seam_facets.equals(mv.seam_facets));

        // Replace: enforcers move to the top, blockers stay.
        auto top = select_normal(Vec3d::UnitZ(), 10.);
        r = paint_volume(mv, ctx, 0, PaintLayer::Seam, EnforcerBlockerType::ENFORCER, PaintMode::Replace, *top, false);
        CHECK(r.changed);
        CHECK(r.states[int(EnforcerBlockerType::ENFORCER)].facets == 2);
        CHECK(r.states[int(EnforcerBlockerType::BLOCKER)].facets == 2);
        TriangleSelector sel(mv.mesh());
        sel.deserialize(mv.seam_facets.get_data());
        CHECK(sel.num_facets(EnforcerBlockerType::ENFORCER) == 2);
        CHECK(sel.num_facets(EnforcerBlockerType::BLOCKER) == 2);
        copy_through_strings(mv.seam_facets, copy.volume().seam_facets, n);
        CHECK(copy.volume().seam_facets.equals(mv.seam_facets));

        // Clear the blockers' face (state ignored), then everything.
        r = paint_volume(mv, ctx, 0, PaintLayer::Seam, EnforcerBlockerType::NONE, PaintMode::Clear, *front, false);
        CHECK(r.states.count(int(EnforcerBlockerType::BLOCKER)) == 0);
        r = paint_volume(mv, ctx, 0, PaintLayer::Seam, EnforcerBlockerType::NONE, PaintMode::Clear, *select_all(), false);
        CHECK(r.states.empty());
        CHECK(mv.seam_facets.empty());

        // Painting the same thing twice is not a change (no undo snapshot churn).
        paint_volume(mv, ctx, 0, PaintLayer::Seam, EnforcerBlockerType::ENFORCER, PaintMode::Add, *rear, false);
        r = paint_volume(mv, ctx, 0, PaintLayer::Seam, EnforcerBlockerType::ENFORCER, PaintMode::Add, *rear, false);
        CHECK_FALSE(r.changed);
    }
}

TEST_CASE("Colour painting stores the filament index as the state", "[facets]")
{
    CubeModel    cube;
    ModelVolume &mv  = cube.volume();
    EvalContext  ctx = cube.ctx();

    EnforcerBlockerType state;
    std::string         err;
    REQUIRE(parse_paint_state({{"filament", 3}}, PaintLayer::Color, state, err));
    PaintResult r = paint_volume(mv, ctx, 0, PaintLayer::Color, state, PaintMode::Add, *parse({{"normal", {{"side", "top"}}}}), false);
    CHECK(r.changed);
    CHECK(mv.mmu_segmentation_facets.has_facets(mv, EnforcerBlockerType(3)));
    CHECK_FALSE(mv.mmu_segmentation_facets.has_facets(mv, EnforcerBlockerType(2)));
    json j = stats_to_json(PaintLayer::Color, r.states);
    CHECK(j["filament 3"]["facets"] == 2);
    // Other layers untouched.
    CHECK(mv.seam_facets.empty());
    CHECK(mv.supported_facets.empty());
    CHECK(&annotation(mv, PaintLayer::Fuzzy) == &mv.fuzzy_skin_facets);
}

TEST_CASE("Clearing collapses brush-subdivided facets", "[facets]")
{
    CubeModel    cube;
    ModelVolume &mv  = cube.volume();
    EvalContext  ctx = cube.ctx();

    // Subdivide a facet the way a brush does, painting only part of it.
    TriangleSelector sel(mv.mesh(), 0.5f);
    const indexed_triangle_set &its = mv.mesh().its;
    const Vec3f centre = (its.vertices[its.indices[0][0]] + its.vertices[its.indices[0][1]] + its.vertices[its.indices[0][2]]) / 3.f;
    const Vec3f normal = its_face_normal(its, 0);
    sel.select_patch(0, TriangleSelector::SinglePointCursor::cursor_factory(centre, centre + normal * 100.f, 2.f, TriangleSelector::SPHERE,
                                                                          Transform3d::Identity(), TriangleSelector::ClippingPlane()),
                     EnforcerBlockerType::ENFORCER, Transform3d::Identity(), true);
    mv.supported_facets.set(sel);
    REQUIRE_FALSE(mv.supported_facets.empty());
    StateStatsMap before = annotation_stats(mv.mesh(), ctx.volumes[0].world(), mv.supported_facets.get_data());
    REQUIRE(before[int(EnforcerBlockerType::ENFORCER)].area_mm2 < 200.); // only part of a 200 mm2 facet

    PaintResult r = paint_volume(mv, ctx, 0, PaintLayer::Support, EnforcerBlockerType::NONE, PaintMode::Clear, *select_facets(0, -1, {0}), false);
    CHECK(r.changed);
    CHECK(mv.supported_facets.empty());
}

TEST_CASE("Facet index selectors match by volume id or index", "[facets]")
{
    CubeModel   cube;
    EvalContext ctx = cube.ctx();
    const size_t vid = cube.volume().id().id;
    CHECK(hits(*select_facets(vid, -1, {1, 3, 99}), ctx) == std::vector<size_t>{1, 3});
    CHECK(hits(*select_facets(vid + 1000, -1, {1}), ctx).empty());
    CHECK(hits(*select_facets(0, 0, {2}), ctx) == std::vector<size_t>{2});
    CHECK(hits(*parse({{"facets", {{"volume", vid}, {"indices", {4}}}}}), ctx) == std::vector<size_t>{4});
}

TEST_CASE("Geometry queries: bbox, sides, heightAt, measure", "[facets]")
{
    CubeModel   cube;
    EvalContext ctx = cube.ctx();

    json bb = geometry_query(ctx, "bbox", json::object());
    REQUIRE(bb["ok"] == true);
    CHECK(bb["size"][0].get<double>() == Approx(20.));
    CHECK(bb["facets"] == 12);

    json sides = geometry_query(ctx, "sides", json::object());
    CHECK(sides["sides"]["rear"]["areaMm2"].get<double>() == Approx(400.));
    CHECK(sides["sides"]["rear"]["extreme"].get<double>() == Approx(bb["max"][1].get<double>()));
    CHECK(sides["sides"]["front"]["extreme"].get<double>() == Approx(bb["min"][1].get<double>()));

    const double cx = (bb["min"][0].get<double>() + bb["max"][0].get<double>()) / 2.;
    const double cy = (bb["min"][1].get<double>() + bb["max"][1].get<double>()) / 2.;
    json h = geometry_query(ctx, "heightAt", {{"x", cx + 1.3}, {"y", cy + 2.7}});
    REQUIRE(h["ok"] == true);
    REQUIRE(h["hits"].size() == 2);
    CHECK(h["top"].get<double>() == Approx(bb["max"][2].get<double>()));
    CHECK(h["hits"][1]["facing"] == "up");
    CHECK(geometry_query(ctx, "heightAt", {{"x", cx + 50.}, {"y", cy}})["top"].is_null());

    CHECK(geometry_query(ctx, "heightAt", json::object())["ok"] == false);
    CHECK(geometry_query(ctx, "nope", json::object())["path"] == "query");

    // Two cubes 10 mm apart in X.
    CubeModel other;
    other.object->instances[0]->set_offset(cube.object->instances[0]->get_offset() + Vec3d(30., 0., 0.));
    json m = geometry_measure(ctx, other.ctx());
    REQUIRE(m["ok"] == true);
    CHECK(m["distanceMm"].get<double>() == Approx(10.));
    CHECK(m["bboxGapMm"][0].get<double>() == Approx(10.));
    CHECK(m["centroidDistanceMm"].get<double>() == Approx(30.));
}
