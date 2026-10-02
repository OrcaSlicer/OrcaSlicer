#include <catch2/catch_all.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// The layer at this Z is the last one of the base, so its top surface is the ledge.
const double ledge_z = 5.0;

// The first layer, at initial_layer_print_height.
const double first_layer_z = 0.2;

// TestMesh::step scaled 3x in X/Y: a 60x60x5 base carrying a 54x54 column up to z=10, leaving a 3mm
// top ledge around a feature that keeps rising. That is the geometry both only_one_wall_top and the
// top surface expansion act on. The ledge has to stay wider than the wall band plus two top-infill
// lines, or the expansion discards it as a sliver and the tests below assert nothing.
TriangleMesh step_with_ledge()
{
    TriangleMesh m = Slic3r::Test::mesh(TestMesh::step);
    m.scale(Vec3f(3.f, 3.f, 1.f));
    return m;
}

// Every setting the assertions depend on, so none of them rests on a default.
DynamicPrintConfig base_config(const char *wall_generator)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "wall_generator",             wall_generator },
        { "layer_height",               0.2 },  // puts a layer boundary exactly on ledge_z
        { "initial_layer_print_height", 0.2 },
        { "wall_loops",                 3 },
        { "sparse_infill_density",      "15%" },
        { "top_shell_layers",           3 },
        { "bottom_shell_layers",        3 },
        { "top_surface_density",        "100%" },
        { "top_surface_expansion",      0.0 },
        { "only_one_wall_top",          false },
        { "only_one_wall_first_layer",  false },
        // Do not let the one-wall threshold discard the 3mm ledge before the feature sees it.
        { "min_width_top_surface",      0.0 },
    });
    return config;
}

double collection_length(const ExtrusionEntityCollection &coll)
{
    double len = 0.;
    for (const ExtrusionEntity *entity : coll.flatten().entities)
        if (! entity->is_collection())
            len += entity->length();
    return len;
}

// Extruded length per layer. Two slices are compared through this rather than through their G-code,
// because the G-code carries a config block that differs whenever any setting differs.
struct SliceLengths {
    std::vector<double> perimeters;
    std::vector<double> fills;
};

SliceLengths slice_lengths(const Print &print)
{
    SliceLengths out;
    for (const Layer *layer : print.objects().front()->layers()) {
        double perimeters = 0., fills = 0.;
        for (const LayerRegion *region : layer->regions()) {
            perimeters += collection_length(region->perimeters);
            fills      += collection_length(region->fills);
        }
        out.perimeters.push_back(perimeters);
        out.fills.push_back(fills);
    }
    return out;
}

double perimeter_length_at(const Print &print, double print_z)
{
    for (const Layer *layer : print.objects().front()->layers())
        if (std::abs(layer->print_z - print_z) < 1e-4) {
            double len = 0.;
            for (const LayerRegion *region : layer->regions())
                len += collection_length(region->perimeters);
            return len;
        }
    return 0.;
}

// Largest per-layer difference between two series; a negative result means they are not comparable.
double max_difference(const std::vector<double> &a, const std::vector<double> &b)
{
    if (a.size() != b.size() || a.empty())
        return -1.;
    double worst = 0.;
    for (size_t i = 0; i < a.size(); ++ i)
        worst = std::max(worst, std::abs(a[i] - b[i]));
    return worst;
}

} // namespace

// The expansion only retypes area as top solid infill, so it can do nothing where there is no top
// fill to begin with: zero top shell layers retypes the top surfaces as internal, and a top surface
// density of 0% leaves the top layer with walls only. The last section is the control - the same
// expansion on the same model does change the slice once a top fill exists - without which the two
// equality checks above it would hold for an unrelated reason.
TEST_CASE("Top surface expansion only acts where there is a top fill", "[Perimeters]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    auto lengths_for = [wall_generator](int top_shell_layers, const char *top_surface_density, double expansion) {
        DynamicPrintConfig config = base_config(wall_generator);
        config.set_deserialize_strict({
            { "top_shell_layers",      top_shell_layers },
            { "top_surface_density",   top_surface_density },
            { "top_surface_expansion", expansion },
        });
        Print print;
        init_and_process_print({ step_with_ledge() }, print, config);
        REQUIRE_FALSE(print.objects().empty());
        return slice_lengths(print);
    };

    SECTION("no top shell layers") {
        const SliceLengths off = lengths_for(0, "100%", 0.0);
        const SliceLengths on  = lengths_for(0, "100%", 2.0);
        REQUIRE(off.perimeters.size() == on.perimeters.size());
        CHECK_THAT(max_difference(off.perimeters, on.perimeters), Catch::Matchers::WithinAbs(0., 1.0));
        CHECK_THAT(max_difference(off.fills,      on.fills),      Catch::Matchers::WithinAbs(0., 1.0));
    }

    SECTION("zero top surface density") {
        const SliceLengths off = lengths_for(3, "0%", 0.0);
        const SliceLengths on  = lengths_for(3, "0%", 2.0);
        REQUIRE(off.perimeters.size() == on.perimeters.size());
        CHECK_THAT(max_difference(off.perimeters, on.perimeters), Catch::Matchers::WithinAbs(0., 1.0));
        CHECK_THAT(max_difference(off.fills,      on.fills),      Catch::Matchers::WithinAbs(0., 1.0));
    }

    SECTION("with a top fill the same expansion does change the slice") {
        const SliceLengths off = lengths_for(3, "100%", 0.0);
        const SliceLengths on  = lengths_for(3, "100%", 2.0);
        REQUIRE(off.fills.size() == on.fills.size());
        CHECK(max_difference(off.fills, on.fills) > scale_(0.5));
    }
}

// With no top shell the top surfaces are retyped as internal, so the top surface density has nothing
// left to control: there is no top fill, and only_one_wall_top - the one route from the density to the
// perimeters - is itself switched off for want of a top surface to act on.
TEST_CASE("Top surface density does not affect a slice without a top shell", "[Perimeters]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    auto lengths_for = [wall_generator](const char *top_surface_density) {
        DynamicPrintConfig config = base_config(wall_generator);
        config.set_deserialize_strict({
            { "top_shell_layers",    0 },
            { "only_one_wall_top",   true },
            { "top_surface_density", top_surface_density },
        });
        Print print;
        init_and_process_print({ step_with_ledge() }, print, config);
        REQUIRE_FALSE(print.objects().empty());
        return slice_lengths(print);
    };

    const SliceLengths solid = lengths_for("100%");
    const SliceLengths none  = lengths_for("0%");
    REQUIRE(solid.perimeters.size() == none.perimeters.size());
    CHECK_THAT(max_difference(solid.perimeters, none.perimeters), Catch::Matchers::WithinAbs(0., 1.0));
    CHECK_THAT(max_difference(solid.fills,      none.fills),      Catch::Matchers::WithinAbs(0., 1.0));
}

// On the ledge layer the inner walls are given up to the top fill, so that layer loses wall length.
// The handover needs a top fill that reaches the freed space: at a top surface density of 0% there is
// no top fill at all, and without top_surface_expansion the fill never grows over the walls. Either
// way the feature still runs, through the original generation, which keeps the inner walls up to the
// top boundary - putting that layer back between the plain and the one-wall slice.
TEST_CASE("Only one wall on top surfaces drops inner walls only where a top fill replaces them", "[Perimeters]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    auto ledge_perimeters_for = [wall_generator](bool only_one_wall_top, const char *top_surface_density, double expansion) {
        DynamicPrintConfig config = base_config(wall_generator);
        config.set_deserialize_strict({
            { "only_one_wall_top",     only_one_wall_top },
            { "top_surface_density",   top_surface_density },
            { "top_surface_expansion", expansion },
        });
        Print print;
        init_and_process_print({ step_with_ledge() }, print, config);
        REQUIRE_FALSE(print.objects().empty());
        return perimeter_length_at(print, ledge_z);
    };

    const double plain              = ledge_perimeters_for(false, "100%", 2.0);
    const double one_wall           = ledge_perimeters_for(true,  "100%", 2.0);
    const double one_wall_no_fill   = ledge_perimeters_for(true,  "0%",   2.0);
    const double one_wall_no_expand = ledge_perimeters_for(true,  "100%", 0.0);

    REQUIRE(plain > 0.);
    CHECK(one_wall < plain);
    // Both fall back to the original generation, which cuts the walls back to the top boundary but not past it.
    CHECK(one_wall_no_fill > one_wall);
    CHECK(one_wall_no_fill < plain);
    CHECK(one_wall_no_expand > one_wall);
    CHECK(one_wall_no_expand < plain);
}

// The bottom counterpart: the first layer is thinned to a single wall only where a bottom shell fills the
// space behind it. With no bottom shell layers the bottom surfaces are retyped as internal, so that wall
// would ring sparse infill on the bed - the option is switched off instead, and the GUI hides it in that
// state so a profile that left it enabled cannot act behind a hidden checkbox.
TEST_CASE("Only one wall on the first layer needs a bottom shell", "[Perimeters]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    auto first_layer_perimeters_for = [wall_generator](bool only_one_wall_first_layer, int bottom_shell_layers) {
        DynamicPrintConfig config = base_config(wall_generator);
        config.set_deserialize_strict({
            { "only_one_wall_first_layer", only_one_wall_first_layer },
            { "bottom_shell_layers",       bottom_shell_layers },
        });
        Print print;
        init_and_process_print({ step_with_ledge() }, print, config);
        REQUIRE_FALSE(print.objects().empty());
        return perimeter_length_at(print, first_layer_z);
    };

    const double plain             = first_layer_perimeters_for(false, 3);
    const double one_wall          = first_layer_perimeters_for(true,  3);
    // Both at zero bottom shell layers, so everything else that setting changes cancels out between them.
    const double plain_no_shell    = first_layer_perimeters_for(false, 0);
    const double one_wall_no_shell = first_layer_perimeters_for(true,  0);

    REQUIRE(plain > 0.);
    CHECK(one_wall < plain);
    // No bottom shell: the option is inert, down to the same walls an unchecked box gives.
    CHECK_THAT(one_wall_no_shell, Catch::Matchers::WithinAbs(plain_no_shell, 1.0));
}

namespace {

// The layer that closes the cavity of box_over_cavity(), the first one printed over air.
const double cavity_ceiling_z = 6.2;

// A cone standing on its tip, flaring by 5mm of radius per mm of height: at a layer height of 0.2 every
// wall of a layer lands a full millimetre outside the one below, entirely off the layer below but right
// alongside the walls printed with it.
TriangleMesh flared_cone()
{
    TriangleMesh cone = make_cone(20., 4.);
    cone.mirror(Z);
    cone.translate(0., 0., 4.);
    return cone;
}

// A 30mm box holding a 20mm cavity from z=2 to z=6, with a 4mm hole punched down through the ceiling
// of that cavity. The layer at cavity_ceiling_z bridges the cavity, and the walls of the hole sit in
// the middle of that bridge, 15mm clear of anything the layer below supports.
Print &box_over_cavity(Print &print, Model &model, const DynamicPrintConfig &config)
{
    ModelObject *object = model.add_object();
    object->name = "box_over_cavity.stl";
    object->add_volume(make_cube(30., 30., 8.), ModelVolumeType::MODEL_PART, false);
    TriangleMesh cavity = make_cube(20., 20., 4.);
    cavity.translate(5.f, 5.f, 2.f);
    object->add_volume(std::move(cavity), ModelVolumeType::NEGATIVE_VOLUME, false);
    TriangleMesh hole = make_cube(4., 4., 6.);
    hole.translate(13.f, 13.f, 5.f);
    object->add_volume(std::move(hole), ModelVolumeType::NEGATIVE_VOLUME, false);
    object->add_instance();
    object->ensure_on_bed();

    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    return print;
}

// Every setting the assertions below depend on, so none of them rests on a default.
DynamicPrintConfig unsupported_walls_config(const char *wall_generator, bool unsupported_wall_last)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "wall_generator",             wall_generator },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "wall_loops",                 3 },
        { "detect_overhang_wall",       true },
        // Outer wall first, so an unsupported loop only ends up last if the feature puts it there.
        { "wall_sequence",              "outer wall/inner wall" },
        { "is_infill_first",            false },
        { "sparse_infill_density",      "15%" },
        { "unsupported_wall_last",      unsupported_wall_last },
        { "gcode_comments",             true },
    });
    return config;
}

// A loop extruded entirely in mid air: every one of its paths is an overhang.
bool unsupported_loop(const ExtrusionEntity *entity)
{
    if (! entity->is_loop())
        return false;
    const ExtrusionPaths &paths = static_cast<const ExtrusionLoop *>(entity)->paths;
    return ! paths.empty() && std::all_of(paths.begin(), paths.end(),
                                          [](const ExtrusionPath &path) { return path.role() == erOverhangPerimeter; });
}

// The loops of every wall island of the print, island by island, in extrusion order.
std::vector<std::vector<const ExtrusionLoop*>> wall_islands(const Print &print)
{
    std::vector<std::vector<const ExtrusionLoop*>> islands;
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *region : layer->regions())
            for (const ExtrusionEntity *island : region->perimeters.entities) {
                std::vector<const ExtrusionLoop*> loops;
                for (const ExtrusionEntity *entity : static_cast<const ExtrusionEntityCollection*>(island)->entities)
                    if (entity->is_loop())
                        loops.push_back(static_cast<const ExtrusionLoop*>(entity));
                islands.push_back(std::move(loops));
            }
    return islands;
}

// Islands where a loop that is anchored is extruded after one that is not.
int islands_with_a_supported_loop_last(const Print &print)
{
    int count = 0;
    for (const std::vector<const ExtrusionLoop*> &loops : wall_islands(print)) {
        bool seen_unsupported = false;
        for (const ExtrusionLoop *loop : loops) {
            if (unsupported_loop(loop))
                seen_unsupported = true;
            else if (seen_unsupported) {
                ++ count;
                break;
            }
        }
    }
    return count;
}

// The unsupported loops of the print, and those of them held back for the infill.
std::vector<const ExtrusionLoop*> unsupported_loops(const Print &print, double print_z = -1.)
{
    std::vector<const ExtrusionLoop*> loops;
    for (const Layer *layer : print.objects().front()->layers()) {
        if (print_z >= 0. && std::abs(layer->print_z - print_z) > EPSILON)
            continue;
        for (const LayerRegion *region : layer->regions())
            for (const ExtrusionEntity *island : region->perimeters.entities)
                for (const ExtrusionEntity *entity : static_cast<const ExtrusionEntityCollection*>(island)->entities)
                    if (unsupported_loop(entity))
                        loops.push_back(static_cast<const ExtrusionLoop*>(entity));
    }
    return loops;
}

int loops_held_back_for_infill(const std::vector<const ExtrusionLoop*> &loops)
{
    return int(std::count_if(loops.begin(), loops.end(), [](const ExtrusionLoop *loop) { return loop->print_after_infill; }));
}

// The G-code emitted at `print_z`, so the order of one layer can be read on its own.
std::string layer_gcode(const std::string &gcode, double print_z)
{
    std::string out;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&out, print_z](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (std::abs(self.z() - print_z) < EPSILON)
            out += line.raw() + "\n";
    });
    return out;
}

} // namespace

// Whatever the wall order asks for, a loop with nothing under it cannot be extruded before the loops it
// leans on. The flared cone gives every layer an outer wall that lands completely off the one below, and
// the outer wall first sequence would otherwise put it down before any of them.
TEST_CASE("Unsupported wall loops are extruded after the walls that anchor them", "[Perimeters]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    auto slice_cone = [wall_generator](bool unsupported_wall_last, Print &print) {
        init_and_process_print({ flared_cone() }, print, unsupported_walls_config(wall_generator, unsupported_wall_last));
        REQUIRE_FALSE(print.objects().empty());
    };

    Print on;
    slice_cone(true, on);
    // Without unsupported loops to reorder the rest of the test would pass on an empty print.
    REQUIRE(unsupported_loops(on).size() > 0);
    CHECK(islands_with_a_supported_loop_last(on) == 0);

    SECTION("the held back loops run innermost first") {
        for (const std::vector<const ExtrusionLoop*> &loops : wall_islands(on)) {
            int previous_inset = std::numeric_limits<int>::max();
            for (const ExtrusionLoop *loop : loops)
                if (unsupported_loop(loop)) {
                    CHECK(loop->inset_idx <= previous_inset);
                    previous_inset = loop->inset_idx;
                }
        }
    }

    SECTION("switched off, the configured wall order is left alone") {
        Print off;
        slice_cone(false, off);
        REQUIRE(unsupported_loops(off).size() == unsupported_loops(on).size());
        // Outer wall first puts the unsupported outer wall ahead of the walls behind it.
        CHECK(islands_with_a_supported_loop_last(off) > 0);
    }
}

// A loop the walls cannot reach is a different case: only the bridges of its own layer will ever hold it,
// so it has to wait for them - while a loop that runs alongside a wall keeps its place, because the
// bridges anchor on it instead.
TEST_CASE("A wall loop out of reach of the layer below waits for the infill", "[Perimeters]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    Print print;
    Model model;
    box_over_cavity(print, model, unsupported_walls_config(wall_generator, true));
    print.process();

    const std::vector<const ExtrusionLoop*> hole_loops = unsupported_loops(print, cavity_ceiling_z);
    REQUIRE(hole_loops.size() > 0);
    CHECK(loops_held_back_for_infill(hole_loops) == int(hole_loops.size()));

    SECTION("a loop alongside a supported wall is not held back") {
        Print cone;
        init_and_process_print({ flared_cone() }, cone, unsupported_walls_config(wall_generator, true));
        const std::vector<const ExtrusionLoop*> loops = unsupported_loops(cone);
        REQUIRE(loops.size() > 0);
        CHECK(loops_held_back_for_infill(loops) == 0);
    }

    SECTION("switched off, no loop is held back") {
        Print off;
        Model off_model;
        box_over_cavity(off, off_model, unsupported_walls_config(wall_generator, false));
        off.process();
        const std::vector<const ExtrusionLoop*> loops = unsupported_loops(off, cavity_ceiling_z);
        REQUIRE(loops.size() == hole_loops.size());
        CHECK(loops_held_back_for_infill(loops) == 0);
    }
}

// The held back loops reach the G-code in a second pass, after the infill of their layer: on the layer
// that closes the cavity the walls of the hole are extruded once the bridge is down, so the layer emits
// perimeters, then infill, then the perimeters that were waiting for it.
TEST_CASE("Loops waiting for the infill are extruded after it", "[Perimeters]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    auto ceiling_roles = [wall_generator](bool unsupported_wall_last) {
        Print print;
        Model model;
        box_over_cavity(print, model, unsupported_walls_config(wall_generator, unsupported_wall_last));
        const std::string layer = layer_gcode(gcode(print), cavity_ceiling_z);
        REQUIRE_FALSE(layer.empty());
        return role_sequence(layer, { "perimeter", "infill" });
    };

    CHECK(ceiling_roles(true)  == std::vector<std::string>{ "perimeter", "infill", "perimeter" });
    CHECK(ceiling_roles(false) == std::vector<std::string>{ "perimeter", "infill" });
}

namespace {

// The rib spans z=[0,5] and the slab z=[5,6], so this is the slab's first layer - the only one whose
// support comes from the rib rather than from the slab below it.
const double slab_first_layer_z = 5.2;

// Rib widths either side of what the wall generators can print. At a 0.4mm nozzle the classic generator
// builds nothing thinner than nozzle/3 = 0.133mm and Arachne drops anything below min_feature_size, 25%
// of the nozzle = 0.1mm. 0.08mm is under both thresholds, 0.3mm over both.
const double unprintable_rib = 0.08;
const double printable_rib   = 0.3;

// A 4x5mm anchor tower carrying a 20x5mm slab at z=[5,6], with a rib `rib_width` wide running the whole
// length of the slab beneath its y=0 edge; a `rib_width` of 0 leaves the rib out. Nothing else is under
// that edge, so whether the wall along it is an overhang rests entirely on the rib. Overhang detection
// grows the lower slices by half the nozzle diameter before it asks, which carries either rib past the
// 0.21mm from the slab edge to that wall - the unprintable one only fails to reach it once it is filtered
// out for being unprintable.
Print &slab_over_rib(Print &print, Model &model, double rib_width, const DynamicPrintConfig &config)
{
    ModelObject *object = model.add_object();
    object->name = "slab_over_rib.stl";
    object->add_volume(make_cube(4., 5., 6.), ModelVolumeType::MODEL_PART, false);
    if (rib_width > 0.) {
        TriangleMesh rib = make_cube(20., rib_width, 5.);
        rib.translate(4.f, 0.f, 0.f);
        object->add_volume(std::move(rib), ModelVolumeType::MODEL_PART, false);
    }
    TriangleMesh slab = make_cube(20., 5., 1.);
    slab.translate(4.f, 0.f, 5.f);
    object->add_volume(std::move(slab), ModelVolumeType::MODEL_PART, false);
    object->add_instance();
    object->ensure_on_bed();

    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    return print;
}

// Every setting the assertions below depend on, so none of them rests on a default. The wall line widths
// are pinned because the rib widths above are chosen against the distance from the slab edge to its outer
// wall, and min_feature_size because it is one of the two thresholds under test.
DynamicPrintConfig printable_rib_config(const char *wall_generator, bool detect_thin_wall)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "wall_generator",                wall_generator },
        { "layer_height",                  0.2 },  // puts a layer boundary exactly on the top of the rib
        { "initial_layer_print_height",    0.2 },
        { "nozzle_diameter",               "0.4" },
        { "outer_wall_line_width",         0.42 },
        { "inner_wall_line_width",         0.45 },
        { "wall_loops",                    2 },
        { "detect_overhang_wall",          true },
        { "detect_thin_wall",              detect_thin_wall },
        { "min_feature_size",              "25%" },
        { "raft_layers",                   0 },
        // Anything that adds, drops or reorders walls would move length between the roles being counted.
        { "extra_perimeters_on_overhangs", false },
        { "overhang_reverse",              false },
        { "only_one_wall_top",             false },
        { "only_one_wall_first_layer",     false },
        { "unsupported_wall_last",         false },
        { "sparse_infill_density",         "15%" },
    });
    return config;
}

// Length of every overhang perimeter path on the layer at `print_z`, loops and open extrusions alike.
double overhang_length_at(const Print &print, double print_z)
{
    double len = 0.;
    const auto add_entity = [&len](const ExtrusionEntity *entity, auto &&self) -> void {
        const auto add_paths = [&len](const ExtrusionPaths &paths) {
            for (const ExtrusionPath &path : paths)
                if (path.role() == erOverhangPerimeter)
                    len += path.length();
        };
        if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection*>(entity)) {
            for (const ExtrusionEntity *child : coll->entities)
                self(child, self);
        } else if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(entity)) {
            add_paths(loop->paths);
        } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath*>(entity)) {
            add_paths(multi->paths);
        } else if (const auto *path = dynamic_cast<const ExtrusionPath*>(entity)) {
            if (path->role() == erOverhangPerimeter)
                len += path->length();
        }
    };

    for (const Layer *layer : print.objects().front()->layers()) {
        if (std::abs(layer->print_z - print_z) > EPSILON)
            continue;
        for (const LayerRegion *region : layer->regions())
            add_entity(&region->perimeters, add_entity);
    }
    return len;
}

} // namespace

// A sliver the wall generator prints nothing for holds nothing up, so it cannot be what decides that the
// wall above it is not an overhang. The rib under the slab is the only thing that edge of the slab could
// rest on: below the threshold of the active generator the slab has to come out exactly as it does with
// no rib at all, and the last check is the control - a rib the generator does print anchors that wall,
// without which the first check would hold for want of any sensitivity to the rib.
TEST_CASE("A lower layer sliver too thin to print does not support the wall above it", "[Perimeters]")
{
    const char *wall_generator   = GENERATE("classic", "arachne");
    const bool  detect_thin_wall = GENERATE(true, false);
    CAPTURE(wall_generator, detect_thin_wall);

    auto overhang_for = [wall_generator, detect_thin_wall](double rib_width) {
        Print print;
        Model model;
        slab_over_rib(print, model, rib_width, printable_rib_config(wall_generator, detect_thin_wall));
        print.process();
        REQUIRE_FALSE(print.objects().empty());
        return overhang_length_at(print, slab_first_layer_z);
    };

    const double no_rib      = overhang_for(0.);
    const double unprintable = overhang_for(unprintable_rib);
    const double printable   = overhang_for(printable_rib);

    // Only where the slab meets the tower is it held up from below, so both of its 20mm walls overhang.
    REQUIRE(no_rib > scale_(30.));
    CHECK_THAT(unprintable, Catch::Matchers::WithinAbs(no_rib, scale_(1.)));
    // A rib that does get printed takes the 20mm outer wall running along it out of the overhangs.
    CHECK(printable < no_rib - scale_(15.));
}

namespace {

// Every setting the fuzzy skin assertions below depend on.
DynamicPrintConfig fuzzy_skin_config(const char *wall_generator)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "wall_generator",             wall_generator },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        // One wall, so every wall point along the long sides belongs to the fuzzed outer wall.
        { "wall_loops",                 1 },
        { "fuzzy_skin",                 "external" },
        { "fuzzy_skin_noise_type",      "classic" },
        { "fuzzy_skin_thickness",       0.3 },
        { "fuzzy_skin_point_distance",  0.8 },
    });
    return config;
}

// How far the wall points over the middle 60% of the layer's length stray across its width, worst side.
// A negative result means there is no layer at `print_z`.
double mid_span_wall_spread(const Print &print, double print_z)
{
    for (const Layer *layer : print.objects().front()->layers()) {
        if (std::abs(layer->print_z - print_z) > 1e-4)
            continue;
        const BoundingBox bbox  = get_extents(layer->lslices);
        const coord_t     x_min = bbox.min.x() + bbox.size().x() / 5;
        const coord_t     x_max = bbox.max.x() - bbox.size().x() / 5;
        Points            points;
        for (const LayerRegion *region : layer->regions())
            region->perimeters.collect_points(points);
        coord_t spread = 0;
        for (const bool south : { true, false }) {
            coord_t lo = bbox.max.y(), hi = bbox.min.y();
            for (const Point &p : points)
                if (p.x() > x_min && p.x() < x_max && (p.y() < bbox.center().y()) == south) {
                    lo = std::min(lo, p.y());
                    hi = std::max(hi, p.y());
                }
            spread = std::max(spread, hi - lo);
        }
        return unscale<double>(spread);
    }
    return -1.;
}

} // namespace

// TestMesh::bridge is a 50x10mm deck from z=5 to z=8 on two 5mm-wide pillars, leaving a 40mm span. The deck's
// first layer (print_z 5.2) crosses the span unsupported; the layers above it rest on the deck.
TEST_CASE("Fuzzy skin leaves the walls of a bridge smooth", "[Perimeters]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    Print print;
    init_and_process_print({ TestMesh::bridge }, print, fuzzy_skin_config(wall_generator));
    REQUIRE_FALSE(print.objects().empty());

    // Control: one deck layer up the same walls rest on the deck, so they are fuzzed.
    CHECK(mid_span_wall_spread(print, 5.6) > 0.1);
    // Over the unsupported span the walls stay straight.
    const double bridged = mid_span_wall_spread(print, 5.2);
    CHECK(bridged >= 0.);
    CHECK(bridged < 0.001);
}

// One object: a 20x20x3mm block on the bed and a second one floating above it from z=5 to z=8. The layers in
// the gap are empty, so the floating block's first layer (print_z 5.2) has a layer below it with nothing
// printed on it; the layers above rest on the floating block.
TEST_CASE("Fuzzy skin leaves the walls over an empty layer smooth", "[Perimeters]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    TriangleMesh mesh     = make_cube(20., 20., 3.);
    TriangleMesh floating = make_cube(20., 20., 3.);
    floating.translate(0.f, 0.f, 5.f);
    mesh.merge(floating);

    Print print;
    init_and_process_print({ mesh }, print, fuzzy_skin_config(wall_generator));
    REQUIRE_FALSE(print.objects().empty());

    // Control: one layer up the walls rest on the floating block, so they are fuzzed.
    CHECK(mid_span_wall_spread(print, 5.6) > 0.1);
    // Nothing is printed under the first floating layer, so its walls stay straight.
    const double floating_first_layer = mid_span_wall_spread(print, 5.2);
    CHECK(floating_first_layer >= 0.);
    CHECK(floating_first_layer < 0.001);
}
