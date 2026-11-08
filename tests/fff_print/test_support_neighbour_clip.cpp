#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Support/SupportNeighbourClip.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;

// Support never prints into ANOTHER object on the plate (Support/SupportNeighbourClip.hpp).
//
// Fixture: a tee -- a pillar with an arm reaching +X at the top -- and a flat plate standing under
// the arm, clear of the pillar. Every support generator drops the arm's support straight down to
// the bed, i.e. through the plate, because support only ever avoids its own object.

namespace {

TriangleMesh tee()
{
    TriangleMesh pillar = make_cube(6., 10., 30.);
    TriangleMesh arm    = make_cube(40., 10., 3.);
    arm.translate(0.f, 0.f, 27.f);
    pillar.merge(arm);
    return pillar;
}

// Model in plate coordinates: the tee at (100, 100), the plate 4 mm right of the pillar and
// 5 mm wider than the arm on either side, 6 mm tall, unless `plate_x` moves it.
DynamicPrintConfig build(Print &print, Model &model, const std::string &support_type, const std::string &style, double plate_x)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",          "1" },
        { "support_type",            support_type },
        { "support_style",           style },
        { "support_threshold_angle", "30" },
        { "layer_height",            "0.2" },
        { "skirt_loops",             "0" },
        { "printable_area",          "0x0,250x0,250x250,0x250" },
    });
    model.clear_objects();
    ModelObject *a = model.add_object();
    a->name = "tee";
    a->add_volume(tee());
    a->add_instance()->set_offset(Vec3d(100., 100., 0.));
    ModelObject *b = model.add_object();
    b->name = "plate";
    b->add_volume(make_cube(25., 20., 6.));
    b->add_instance()->set_offset(Vec3d(plate_x, 95., 0.));
    for (ModelObject *mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    return config;
}

const PrintObject *object_named(const Print &print, const std::string &name)
{
    for (const PrintObject *o : print.objects())
        if (o->model_object()->name == name)
            return o;
    return nullptr;
}

struct SupportAudit {
    double total_mm  = 0.;   // all support extrusion of the tee
    double inside_mm = 0.;   // of which inside the plate's material at that height
};

// The tee's support extrusions in plate coordinates against the plate's slices at the same height.
SupportAudit audit(const Print &print)
{
    const PrintObject *a = object_named(print, "tee");
    const PrintObject *b = object_named(print, "plate");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    const Point da = a->instances().front().shift;
    const Point db = b->instances().front().shift;
    SupportAudit out;
    for (const SupportLayer *sl : a->support_layers()) {
        Polylines lines;
        sl->support_fills.collect_polylines(lines);
        for (Polyline &pl : lines) {
            pl.translate(da);
            out.total_mm += unscale<double>(pl.length());
        }
        ExPolygons material;
        for (const Layer *l : b->layers())
            if (l->print_z - l->height < sl->print_z - EPSILON && l->print_z > sl->print_z - sl->height + EPSILON)
                for (ExPolygon ex : l->lslices) {
                    ex.translate(db);
                    material.emplace_back(std::move(ex));
                }
        if (material.empty())
            continue;
        for (const Polyline &pl : intersection_pl(lines, material))
            out.inside_mm += unscale<double>(pl.length());
    }
    return out;
}

} // namespace

TEST_CASE("Support stays out of a neighbouring object", "[SupportNeighbourClip]")
{
    const auto [type, style] = GENERATE(table<std::string, std::string>({
        { "normal(auto)", "default" },
        { "normal(auto)", "snug" },
        { "tree(auto)",   "organic" },
        { "tree(auto)",   "tree_hybrid" },
    }));
    CAPTURE(type, style);

    // Reference: the plate far away, so nothing is clipped. The tee's support is the same object
    // either way; only where it may print changes.
    Print far_print;  Model far_model;
    build(far_print, far_model, type, style, 180.);
    far_print.process();
    const SupportAudit far_audit = audit(far_print);
    REQUIRE(far_audit.total_mm > 50.);

    Print print; Model model;
    const DynamicPrintConfig config = build(print, model, type, style, 110.);
    print.process();
    const SupportAudit near_audit = audit(print);
    CHECK(near_audit.inside_mm < 0.5);
    // The support outside the plate is kept: the column still runs from the arm down to the plate.
    CHECK(near_audit.total_mm > 0.3 * far_audit.total_mm);
    CHECK(near_audit.total_mm < far_audit.total_mm);

    // Moving the plate away invalidates only the brim and G-code, not the support: the next
    // process() must give the clipped layers back.
    model.objects[1]->instances.front()->set_offset(Vec3d(180., 95., 0.));
    print.apply(model, config);
    print.process();
    const SupportAudit moved = audit(print);
    CHECK(moved.inside_mm == 0.);
    CHECK(std::abs(moved.total_mm - far_audit.total_mm) < 0.02 * far_audit.total_mm);
}
