#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Support/SupportDirection.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "test_helpers.hpp"

#include <algorithm>
#include <utility>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// Support direction: support columns leaning along a direction other than the layer normal.
//
// The fixture is a cantilever: a pillar on the bed with a beam sticking out of its top. Everything
// under the beam past the pillar needs support, and that support forms one column whose OUTER edge
// (the beam's free end) is trimmed by nothing, so where that edge sits at each height says exactly
// which way the column leans. The tests read the column off the support layers directly; G-code is
// only generated to prove the leaning support still emits.

namespace {

constexpr double PILLAR = 8.;    // pillar footprint and the beam's width
constexpr double BEAM   = 40.;   // beam length along +X
constexpr double TOP    = 20.;   // beam top; the beam is 2mm thick, so its underside is at 18

TriangleMesh cantilever()
{
    TriangleMesh pillar = make_cube(PILLAR, PILLAR, TOP);
    TriangleMesh beam   = make_cube(BEAM, PILLAR, 2.);
    beam.translate(0., 0., float(TOP - 2.));
    pillar.merge(beam);
    return pillar;
}

DynamicPrintConfig direction_config(std::initializer_list<ConfigBase::SetDeserializeItem> extra)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",          "1" },
        { "support_type",            "normal(auto)" },
        { "support_style",           "snug" },
        { "support_threshold_angle", "30" },
        { "layer_height",            "0.2" },
        { "skirt_loops",             "0" },
    });
    config.set_deserialize_strict(extra);
    return config;
}

// The +X extent of the support column at every support layer in a window that is strictly below
// the beam's underside and clear of the first layers (a raft-less first layer is padded, and a
// tilted pillar gets its own support there), as (print_z, max x in mm) pairs, in the object's slice
// frame. The window is measured as height ABOVE THE PLATE at the edge point, which is print_z for
// an unrotated object. With the layers tilted, the plate is a tilted plane in the slice frame: the
// column ends on it, and the beam runs parallel to it, so only a window in plate height stays
// clear of both.
std::vector<std::pair<double, double>> column_outer_edge(const PrintObject &object)
{
    // The plate as n . (x, y, print_z) + k = height above it (n is unit length).
    Vec3d  n_bed;
    double k_bed;
    object.bed_plane_in_slice_frame(n_bed, k_bed);
    std::vector<std::pair<double, double>> out;
    for (const SupportLayer *layer : object.support_layers()) {
        if (layer->support_islands.empty())
            continue;
        const BoundingBox bbox   = get_extents(layer->support_islands);
        const double      x      = unscale<double>(bbox.max.x());
        const double      height = n_bed.dot(Vec3d(x, unscale<double>(bbox.center().y()), layer->print_z)) + k_bed;
        if (height < 6. || height > TOP - 2. - 1.)
            continue;
        out.emplace_back(layer->print_z, x);
    }
    return out;
}

// Least-squares slope of x against z: the column's lean, in mm of drift per mm of height.
double lean_of(const std::vector<std::pair<double, double>> &edge)
{
    REQUIRE(edge.size() >= 10);
    double sz = 0., sx = 0., szz = 0., szx = 0.;
    for (const auto &[z, x] : edge) {
        sz += z; sx += x; szz += z * z; szx += z * x;
    }
    const double n = double(edge.size());
    return (n * szx - sz * sx) / (n * szz - sz * sz);
}

const PrintObject &only_object(const Slic3r::Print &print)
{
    REQUIRE(print.objects().size() == 1);
    return *print.objects().front();
}

} // namespace

TEST_CASE("Support follows the layers by default", "[SupportDirection]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ cantilever() }, print, direction_config({}));
    const PrintObject &object = only_object(print);
    REQUIRE(! object.support_layers().empty());
    CHECK(! object.support_lean().active());
    CHECK_THAT(lean_of(column_outer_edge(object)), WithinAbs(0., 0.02));
}

TEST_CASE("A custom support direction leans the column by the configured angle", "[SupportDirection]")
{
    // b = -30 tilts the support axis 30 degrees about Y. The object itself is not rotated, so the
    // axis is the same in the slice frame, and the column drifts by tan(30) mm per mm of height,
    // in whichever X sense the B/C convention puts the tilt.
    const double b = -30.;
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ cantilever() }, print, direction_config({
        { "support_direction",   "custom" },
        { "support_direction_b", b },
        { "support_direction_c", 0. },
    }));
    const PrintObject &object = only_object(print);
    REQUIRE(! object.support_layers().empty());
    CHECK(! object.support_lean().active());   // the shear is scoped to generation and is gone now

    const Vec3d  axis     = Geometry::build_normal_from_bc(b, 0.);
    const double expected = axis.x() / axis.z();
    CHECK_THAT(std::abs(expected), WithinAbs(std::tan(Geometry::deg2rad(30.)), 1e-6));
    CHECK_THAT(lean_of(column_outer_edge(object)), WithinAbs(expected, 0.05));

    SECTION("and the leaning support is emitted as support G-code") {
        const std::string g = Slic3r::Test::gcode(print);
        CHECK(! Slic3r::Test::layers_with_role(g, "support").empty());
    }
}

TEST_CASE("A custom direction equal to the layer normal changes nothing", "[SupportDirection]")
{
    Slic3r::Print plain, custom;
    Slic3r::Test::init_and_process_print({ cantilever() }, plain,  direction_config({}));
    Slic3r::Test::init_and_process_print({ cantilever() }, custom, direction_config({
        { "support_direction",   "custom" },
        { "support_direction_b", 0. },
        { "support_direction_c", 0. },
    }));
    const auto edge_plain  = column_outer_edge(only_object(plain));
    const auto edge_custom = column_outer_edge(only_object(custom));
    REQUIRE(edge_plain.size() == edge_custom.size());
    for (size_t i = 0; i < edge_plain.size(); ++ i) {
        INFO("layer " << i << " at z " << edge_plain[i].first);
        CHECK_THAT(edge_custom[i].second, WithinAbs(edge_plain[i].second, 1e-6));
    }
}

TEST_CASE("The support lean is limited by the configured maximum", "[SupportDirection]")
{
    // b = -80 asks for an 80 degree lean; the column gets the maximum instead, 45 by default.
    const double max_lean = GENERATE(45., 60.);
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ cantilever() }, print, direction_config({
        { "support_direction",          "custom" },
        { "support_direction_b",        -80. },
        { "support_direction_c",        0. },
        { "support_direction_max_lean", max_lean },
    }));
    const PrintObject &object = only_object(print);
    REQUIRE(! object.support_layers().empty());
    const SupportLean lean = support_lean(object);
    INFO("max lean " << max_lean);
    CHECK(lean.clamped);
    CHECK_THAT(lean.lean_deg(), WithinAbs(max_lean, 1e-6));
    CHECK_THAT(std::abs(lean_of(column_outer_edge(object))), WithinAbs(std::tan(Geometry::deg2rad(max_lean)), 0.05));
}

TEST_CASE("World-vertical support stays vertical in the world when the layers are rotated", "[SupportDirection]")
{
    // The object is sliced with its layers tilted 20 degrees (non-planar, layer_rotation_b), so
    // "follow layers" support tilts with them and "world vertical" support has to lean back by the
    // same angle inside the slice frame. Both are checked in the slice frame against the rotation
    // that was set, and the vertical one is also checked in the object's frame, where it must not
    // drift at all. The tilt is chosen so the beam's free end goes UP in the slice frame: then the
    // column's outer edge below the beam is the projection of that one corner, a clean line.
    const double tilt = 20.;
    const double b    = (Geometry::layer_rotation_matrix(- tilt, 0.) * Vec3d::UnitX()).z() > 0. ? - tilt : tilt;
    auto rotated = [b](const char *direction) {
        return direction_config({
            { "enable_nonplanar",  "1" },
            { "layer_rotation_b",  b },
            { "layer_rotation_c",  0. },
            { "support_direction", direction },
        });
    };
    const Eigen::Matrix3d R          = Geometry::layer_rotation_matrix(b, 0.);
    const Vec3d           up_in_slice = R * Vec3d::UnitZ();
    const double          vertical_lean = up_in_slice.x() / up_in_slice.z();
    REQUIRE(std::abs(vertical_lean) > 0.3);   // the rotation is actually in effect

    Slic3r::Print layers, vertical;
    Slic3r::Test::init_and_process_print({ cantilever() }, layers,   rotated("layers"));
    Slic3r::Test::init_and_process_print({ cantilever() }, vertical, rotated("vertical"));
    const PrintObject &object_layers   = only_object(layers);
    const PrintObject &object_vertical = only_object(vertical);
    REQUIRE(! object_layers.support_layers().empty());
    REQUIRE(! object_vertical.support_layers().empty());

    CHECK_THAT(lean_of(column_outer_edge(object_layers)),   WithinAbs(0., 0.05));
    CHECK_THAT(lean_of(column_outer_edge(object_vertical)), WithinAbs(vertical_lean, 0.05));

    // Back in the object's own frame the vertical column's outer edge must not move with height.
    std::vector<std::pair<double, double>> edge_world;
    for (const auto &[z, x] : column_outer_edge(object_vertical)) {
        const Vec3d p = object_vertical.slice_to_object() * Vec3d(x, 0., z);
        edge_world.emplace_back(p.z(), p.x());
    }
    CHECK_THAT(lean_of(edge_world), WithinAbs(0., 0.05));
}

TEST_CASE("Every support generator leaves the object layers exactly as sliced after a leaning run", "[SupportDirection]")
{
    // The lean is produced by shearing the object layers around the generator and shearing them
    // back. Whatever the generator, the layers a leaning run leaves behind must be the ones a plain
    // run has -- otherwise the perimeters and infill already generated would print displaced.
    const auto [type, style] = GENERATE(table<const char*, const char*>({
        { "normal(auto)", "snug" },
        { "tree(auto)",   "tree_slim" },
        { "tree(auto)",   "organic" },
    }));
    INFO(type << " / " << style);
    Slic3r::Print plain, leaning;
    Slic3r::Test::init_and_process_print({ cantilever() }, plain, direction_config({
        { "support_type",  type },
        { "support_style", style },
    }));
    Slic3r::Test::init_and_process_print({ cantilever() }, leaning, direction_config({
        { "support_type",        type },
        { "support_style",       style },
        { "support_direction",   "custom" },
        { "support_direction_b", -30. },
        { "support_direction_c", 0. },
    }));
    const PrintObject &a = only_object(plain);
    const PrintObject &b = only_object(leaning);
    REQUIRE(! b.support_layers().empty());
    REQUIRE(a.layer_count() == b.layer_count());
    for (size_t i = 0; i < a.layer_count(); ++ i) {
        const Layer *la = a.get_layer(int(i));
        const Layer *lb = b.get_layer(int(i));
        INFO("layer " << i << " at z " << la->print_z);
        REQUIRE(la->lslices.size() == lb->lslices.size());
        for (size_t k = 0; k < la->lslices.size(); ++ k)
            CHECK(la->lslices[k] == lb->lslices[k]);
        REQUIRE(la->region_count() == lb->region_count());
        for (size_t r = 0; r < la->region_count(); ++ r) {
            const ExtrusionEntityCollection &pa = la->get_region(int(r))->perimeters;
            const ExtrusionEntityCollection &pb = lb->get_region(int(r))->perimeters;
            REQUIRE(pa.entities.size() == pb.entities.size());
            for (size_t e = 0; e < pa.entities.size(); ++ e)
                CHECK(pa.entities[e]->first_point() == pb.entities[e]->first_point());
        }
    }
}

TEST_CASE("A world direction below the slice frame's horizon leans along the same column line", "[SupportDirection]")
{
    // The direction names a line, not a sense: support grows from the floor up in every frame. With
    // the layers tilted 120 degrees, world-up points 60 degrees below the slice frame's horizon;
    // the column line is 60 degrees from the layer normal, and the default 45 degree limit clamps
    // it -- not flips it to the opposite azimuth, and not "straight down, so no lean".
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ cantilever() }, print, direction_config({
        { "enable_nonplanar",  "1" },
        { "layer_rotation_b",  -120. },
        { "layer_rotation_c",  0. },
        { "support_direction", "vertical" },
    }));
    const PrintObject &object = only_object(print);
    const SupportLean  lean   = support_lean(object);
    CHECK(lean.active());
    CHECK(lean.clamped);
    CHECK_THAT(lean.lean_deg(), WithinAbs(45., 1e-6));
    // Same azimuth as the unclamped line: the slope points the way world-up projects in the frame,
    // whichever sense that vector came out with.
    const Vec3d up = Geometry::layer_rotation_matrix(-120., 0.) * Vec3d::UnitZ();
    const Vec3d line = up.z() < 0. ? Vec3d(- up) : up;
    CHECK(lean.slope.x() * line.x() > 0.);
}

TEST_CASE("A leaning run shifts an enforcer volume once", "[SupportDirection]")
{
    // The generators hand project_and_append_custom_facets() the enforcer volumes that
    // slice_support_volumes() has already shifted into the sheared frame. With nothing painted it
    // must leave them alone; shifting them again put the enforcer z * slope off at every layer.
    Slic3r::Print print;
    Slic3r::Model model;
    const DynamicPrintConfig config = direction_config({
        { "support_direction",   "custom" },
        { "support_direction_b", -30. },
        { "support_direction_c", 0. },
    });
    Slic3r::Test::init_print({ cantilever() }, print, model, config);
    TriangleMesh block = make_cube(4., 4., 4.);
    block.translate(20.f, 2.f, 8.f);
    model.objects.front()->add_volume(std::move(block), ModelVolumeType::SUPPORT_ENFORCER);
    print.apply(model, print.full_print_config());
    print.process();

    PrintObject &object = *print.get_object(0);
    ScopedSupportShear shear(object);
    REQUIRE(shear.active());
    std::vector<Polygons> enforcers = object.slice_support_enforcers();
    REQUIRE(std::any_of(enforcers.begin(), enforcers.end(), [](const Polygons &p) { return ! p.empty(); }));
    const std::vector<Polygons> sliced = enforcers;
    object.project_and_append_custom_facets(false, EnforcerBlockerType::ENFORCER, enforcers);
    CHECK(enforcers == sliced);
}
