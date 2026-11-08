// Automation action tests. Owner: work package S6 (paint). Tag: [actions].
//
// The paint.* / geometry.* runners are thin GUI wrappers around pure cores (PaintActions.hpp);
// these tests drive the cores on a bare Model and check the descriptors against the dialect.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <nlohmann/json.hpp>

#include "libslic3r/BrimEarsAuto.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "slic3r/GUI/Actions/ActionCatalog.hpp"
#include "slic3r/GUI/Actions/ActionSchema.hpp"
#include "slic3r/GUI/Actions/PaintActions.hpp"
#include "slic3r/GUI/Actions/Register.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI::Actions;
using json = nlohmann::json;
using Catch::Approx;

namespace {

struct Plate
{
    Model model;
    explicit Plate(int n = 1)
    {
        for (int i = 0; i < n; ++i) {
            ModelObject* o = model.add_object();
            o->name        = "cube" + std::to_string(i);
            o->add_volume(make_cube(20., 20., 20.));
            o->add_instance()->set_offset(Vec3d(30. * i, 0., 0.));
        }
    }
};

json seam_rear()
{
    return {{"layer", "seam"},
            {"state", "enforce"},
            {"where", {{"all", json::array({{{"normal", {{"side", "rear"}, {"withinDeg", 60}}}},
                                            {{"not", {{"normal", {{"side", "top"}, {"withinDeg", 30}}}}}}})}}}};
}

} // namespace

TEST_CASE("paint and geometry descriptors are inside the dialect and their examples validate", "[actions]")
{
    Catalog c;
    REQUIRE_NOTHROW(register_paint_actions(c));
    REQUIRE_NOTHROW(register_geometry_actions(c));
    for (const char* id : {"paint.apply", "paint.clear", "paint.remapFilaments", "paint.stats", "geometry.query", "geometry.measure", "object.brimEars"})
        REQUIRE(c.find(id) != nullptr);
    for (const ActionEntry& e : c.entries()) {
        INFO(e.descriptor.id);
        SchemaError err;
        CHECK(check_schema(e.descriptor.params, err));
        INFO(err.path << ": " << err.message);
        CHECK_FALSE(e.descriptor.examples.empty());
        for (const json& ex : e.descriptor.examples) {
            SchemaError verr;
            const json  params = ex.contains("params") ? ex["params"] : json::object();
            CHECK(validate(e.descriptor.params, apply_defaults(e.descriptor.params, params), verr));
            INFO(verr.path << ": " << verr.message);
        }
        CHECK(bool(e.runner));
    }
}

TEST_CASE("the where schema rejects what the selector parser rejects", "[actions]")
{
    const json  schema = paint_apply_params_schema();
    SchemaError err;
    json        p = seam_rear();
    CHECK(validate(schema, p, err));
    p["where"] = {{"normal", {{"side", "up"}}}};
    CHECK_FALSE(validate(schema, p, err));
    p["where"] = {{"normal", {{"side", "rear"}}}, {"zRange", {0, 1}}}; // two selectors in one object
    CHECK_FALSE(validate(schema, p, err));
    p["where"] = {{"all", json::array()}}; // every facet
    CHECK(validate(schema, p, err));
}

TEST_CASE("paint.apply paints, snapshots once, and is a no-op the second time", "[actions]")
{
    Plate            plate;
    int              snapshots = 0;
    std::vector<int> changed;
    auto             snap = [&snapshots]() { ++snapshots; };

    InvokeResult r = paint_apply_model(plate.model, {0}, seam_rear(), snap, changed);
    REQUIRE(r.is_ok());
    CHECK(snapshots == 1);
    CHECK(changed == std::vector<int>{0});
    CHECK(r.result["hits"] == 2);
    CHECK(r.result["hitAreaMm2"].get<double>() == Approx(400.));
    CHECK(r.result["objects"][0]["volumes"][0]["states"]["enforce"]["facets"] == 2);
    CHECK_FALSE(plate.model.objects[0]->volumes[0]->seam_facets.empty());

    r = paint_apply_model(plate.model, {0}, seam_rear(), snap, changed);
    REQUIRE(r.is_ok());
    CHECK(snapshots == 1);
    CHECK(changed.empty());
    CHECK(r.result["changed"] == false);
    CHECK(r.effects_set);
    CHECK(r.effects.empty());

    SECTION("dry run changes nothing") {
        json p      = seam_rear();
        p["layer"]  = "support";
        p["dryRun"] = true;
        r           = paint_apply_model(plate.model, {0}, p, snap, changed);
        CHECK(snapshots == 1);
        CHECK(r.result["hits"] == 2);
        CHECK(r.result["objects"][0]["volumes"][0]["wouldChange"] == true);
        CHECK(plate.model.objects[0]->volumes[0]->supported_facets.empty());
    }
    SECTION("paint.clear erases, once") {
        r = paint_clear_model(plate.model, {0}, {{"layer", "seam"}}, snap, changed);
        CHECK(snapshots == 2);
        CHECK(changed == std::vector<int>{0});
        CHECK(plate.model.objects[0]->volumes[0]->seam_facets.empty());
        r = paint_clear_model(plate.model, {0}, json::object(), snap, changed);
        CHECK(snapshots == 2);
        CHECK(r.result["changed"] == false);
    }
    SECTION("paint.stats reads it back") {
        r = paint_stats_model(plate.model, {0}, {{"layer", "seam"}});
        REQUIRE(r.is_ok());
        CHECK(r.result["objects"][0]["volumes"][0]["layers"]["seam"]["enforce"]["areaMm2"].get<double>() == Approx(400.));
    }
    SECTION("replace moves the state") {
        json p     = seam_rear();
        p["mode"]  = "replace";
        p["where"] = {{"normal", {{"side", "front"}}}};
        r          = paint_apply_model(plate.model, {0}, p, snap, changed);
        CHECK(snapshots == 2);
        CHECK(r.result["objects"][0]["volumes"][0]["states"]["enforce"]["facets"] == 2);
        r = paint_stats_model(plate.model, {0}, {{"layer", "seam"}});
        CHECK(r.result["objects"][0]["volumes"][0]["layers"]["seam"]["enforce"]["facets"] == 2);
    }
}

TEST_CASE("paint.apply errors carry the param path and change nothing", "[actions]")
{
    Plate            plate;
    std::vector<int> changed;
    int              snapshots = 0;
    auto             snap      = [&snapshots]() { ++snapshots; };

    json p     = seam_rear();
    p["layer"] = "glitter";
    CHECK(paint_apply_model(plate.model, {0}, p, snap, changed).data["path"] == "/params/layer");

    p          = seam_rear();
    p["state"] = "filament";
    CHECK(paint_apply_model(plate.model, {0}, p, snap, changed).data["needs"] == "filament");

    p          = seam_rear();
    p["state"] = "block";
    p["layer"] = "fuzzy";
    CHECK(paint_apply_model(plate.model, {0}, p, snap, changed).data["path"] == "/params/state");

    p              = seam_rear();
    p["where"]     = {{"all", json::array({{{"normal", {{"side", "upwards"}}}}})}};
    InvokeResult r = paint_apply_model(plate.model, {0}, p, snap, changed);
    CHECK(r.code == 1002);
    CHECK(r.data["path"] == "/params/where/all/0/normal/side");

    CHECK(paint_apply_model(plate.model, {7}, seam_rear(), snap, changed).code == 1008);
    json q        = seam_rear();
    q["instance"] = 3;
    CHECK(paint_apply_model(plate.model, {0}, q, snap, changed).data["path"] == "/params/instance");
    CHECK(snapshots == 0);
    CHECK(plate.model.objects[0]->volumes[0]->seam_facets.empty());
}

TEST_CASE("colour paint stores the filament and hints at missing slots", "[actions]")
{
    Plate            plate;
    std::vector<int> changed;
    PaintHintContext hints;
    hints.filament_count = 2;
    json p = {{"layer", "color"}, {"state", "filament"}, {"filament", 3}, {"where", {{"planarRegion", {{"seedSide", "top"}}}}}};
    InvokeResult r = paint_apply_model(plate.model, {0}, p, {}, changed, hints);
    REQUIRE(r.is_ok());
    const ModelVolume& mv = *plate.model.objects[0]->volumes[0];
    CHECK(mv.mmu_segmentation_facets.has_facets(mv, EnforcerBlockerType(3)));
    REQUIRE(r.result["hints"].size() == 1);
    CHECK(r.result["hints"][0].get<std::string>().find("Filament 3") != std::string::npos);
}

TEST_CASE("paint.remapFilaments swaps painted filaments", "[actions]")
{
    Plate            plate;
    std::vector<int> changed;
    int              snapshots = 0;
    auto             snap      = [&snapshots]() { ++snapshots; };
    paint_apply_model(plate.model, {0}, {{"layer", "color"}, {"state", "filament"}, {"filament", 2}, {"where", {{"normal", {{"side", "top"}}}}}}, {},
                      changed);
    const ModelVolume& mv = *plate.model.objects[0]->volumes[0];
    REQUIRE(mv.mmu_segmentation_facets.has_facets(mv, EnforcerBlockerType(2)));
    InvokeResult r = paint_remap_model(plate.model, {0}, {{"pairs", json::array({{{"from", 2}, {"to", 5}}})}}, snap, changed);
    REQUIRE(r.is_ok());
    CHECK(snapshots == 1);
    CHECK(mv.mmu_segmentation_facets.has_facets(mv, EnforcerBlockerType(5)));
    CHECK_FALSE(mv.mmu_segmentation_facets.has_facets(mv, EnforcerBlockerType(2)));
    r = paint_remap_model(plate.model, {0}, {{"pairs", json::array({{{"from", 2}, {"to", 7}}})}}, snap, changed);
    CHECK(snapshots == 1);
    CHECK(r.result["changed"] == false);
}

TEST_CASE("geometry.query and geometry.measure", "[actions]")
{
    Plate        plate(2);
    InvokeResult q = geometry_query_model(plate.model, {0, 1}, {{"query", "sides"}});
    REQUIRE(q.is_ok());
    CHECK(q.result["objects"].size() == 2);
    CHECK(q.result["objects"][0]["result"]["sides"]["rear"]["areaMm2"].get<double>() == Approx(400.));

    InvokeResult bad = geometry_query_model(plate.model, {0}, {{"query", "heightAt"}});
    CHECK(bad.code == 1002);
    CHECK(bad.data["path"] == "/params/x");

    InvokeResult m = geometry_measure_model(plate.model, {0, 1}, json::object());
    REQUIRE(m.is_ok());
    CHECK(m.result["distanceMm"].get<double>() == Approx(10.));
    CHECK(m.result["b"]["name"] == "cube1");
    CHECK(geometry_measure_model(plate.model, {0}, json::object()).code == 1002);
}

TEST_CASE("object.brimEars adds ears in object coordinates and clears them", "[actions]")
{
    Plate plate;
    plate.model.objects[0]->instances[0]->set_offset(Vec3d(100., 50., 0.));
    int  snapshots = 0;
    bool changed   = false;
    auto snap      = [&snapshots]() { ++snapshots; };

    json         p = {{"mode", "points"}, {"points", json::array({{{"x", 110}, {"y", 60}}, {{"x", 90}, {"y", 40}}})}};
    InvokeResult r = brim_ears_model(plate.model, 0, p, 3.f, snap, changed);
    REQUIRE(r.is_ok());
    CHECK(changed);
    CHECK(snapshots == 1);
    const ModelObject& mo = *plate.model.objects[0];
    REQUIRE(mo.brim_points.size() == 2);
    const Vec3d world = mo.instances[0]->get_matrix() * mo.brim_points[0].pos.cast<double>();
    CHECK(world.x() == Approx(110.).margin(1e-4));
    CHECK(world.y() == Approx(60.).margin(1e-4));
    CHECK(mo.brim_points[0].head_front_radius == Approx(3.f));

    r = brim_ears_model(plate.model, 0, p, 3.f, snap, changed); // same points again
    CHECK_FALSE(changed);
    CHECK(snapshots == 1);

    r = brim_ears_model(plate.model, 0, {{"mode", "clear"}}, 3.f, snap, changed);
    CHECK(changed);
    CHECK(plate.model.objects[0]->brim_points.empty());
    CHECK(brim_ears_model(plate.model, 0, {{"mode", "points"}}, 3.f, snap, changed).data["needs"] == "points");

    // auto: a cube's footprint has four 90 degree corners.
    r = brim_ears_model(plate.model, 0, {{"mode", "auto"}}, 3.f, snap, changed);
    REQUIRE(r.is_ok());
    CHECK(r.result["added"] == 4);
    CHECK(plate.model.objects[0]->brim_points.size() == 4);
    r = brim_ears_model(plate.model, 0, {{"mode", "auto"}, {"maxAngleDeg", 60}, {"replace", true}}, 3.f, snap, changed);
    CHECK(r.result["added"] == 0);

    // remove: the ear under a point goes, the others stay.
    const size_t before = plate.model.objects[0]->brim_points.size();
    const Vec3d  first  = plate.model.objects[0]->instances[0]->get_matrix() * plate.model.objects[0]->brim_points[0].pos.cast<double>();
    r = brim_ears_model(plate.model, 0, {{"mode", "remove"}, {"points", json::array({{{"x", first.x() + 1.}, {"y", first.y()}}})}}, 3.f, snap,
                        changed);
    REQUIRE(r.is_ok());
    CHECK(changed);
    CHECK(r.result["removed"] == 1);
    CHECK(plate.model.objects[0]->brim_points.size() == before - 1);
    r = brim_ears_model(plate.model, 0, {{"mode", "remove"}, {"points", json::array({{{"x", -500}, {"y", -500}}})}}, 3.f, snap, changed);
    CHECK_FALSE(changed);
}

TEST_CASE("Brim ear corners: convex corners of outlines, concave corners of holes", "[actions]")
{
    // A 20 mm square with a 6 mm square hole: the four outer corners get ears; the hole's corners are
    // reflex seen from the material, so none there (the tool reverses holes and takes concave points).
    Slic3r::Polygon outer({{0, 0}, {scaled(20.), 0}, {scaled(20.), scaled(20.)}, {0, scaled(20.)}});
    Slic3r::Polygon hole({{scaled(7.), scaled(7.)}, {scaled(7.), scaled(13.)}, {scaled(13.), scaled(13.)}, {scaled(13.), scaled(7.)}});
    ExPolygon ex;
    ex.contour = outer;
    ex.holes   = {hole};
    CHECK(brim_ears_auto_points({ex}, 1.f, 125.f).size() == 4);
    CHECK(brim_ears_auto_points({ex}, 1.f, 60.f).empty()); // nothing sharper than 60 degrees
    CHECK(brim_ears_corner_points(outer, 1.f, 125.f, true).size() == 4);
}
