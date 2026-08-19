#include <catch2/catch_all.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"

#include <algorithm>
#include <cmath>
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

// Turn angle, in degrees, at every interior vertex of the external perimeters on the layer at
// `print_z`. On a cylinder these are all the same angle if the contour was decimated evenly.
std::vector<double> external_perimeter_turns(const Print &print, double print_z)
{
    std::vector<double> turns;
    for (const Layer *layer : print.objects().front()->layers()) {
        if (std::abs(layer->print_z - print_z) > 1e-4)
            continue;
        for (const LayerRegion *region : layer->regions()) {
            Points pts;
            for (const ExtrusionEntity *entity : region->perimeters.entities)
                for (const ExtrusionEntity *path : static_cast<const ExtrusionEntityCollection *>(entity)->entities)
                    if (path->role() == erExternalPerimeter)
                        path->collect_points(pts);
            for (size_t i = 1; i + 1 < pts.size(); ++ i) {
                const Vec2d a = (pts[i] - pts[i - 1]).cast<double>();
                const Vec2d b = (pts[i + 1] - pts[i]).cast<double>();
                if (a.norm() < SCALED_EPSILON || b.norm() < SCALED_EPSILON)
                    continue;
                turns.push_back(Geometry::rad2deg(std::acos(std::clamp(a.dot(b) / (a.norm() * b.norm()), -1., 1.))));
            }
        }
    }
    return turns;
}

// Every setting the cylinder assertions depend on. The square corner velocity lives in the X/Y
// jerk slots, which is where Orca keeps it for Klipper.
DynamicPrintConfig cylinder_config(const char *flavor)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "gcode_flavor",                 flavor },
        { "resolution",                   0.012 },
        { "enable_arc_fitting",           "0" },
        { "layer_height",                 0.2 },
        { "initial_layer_print_height",   0.2 },
        { "wall_loops",                   2 },
        { "outer_wall_acceleration",      "5000" },
        { "default_acceleration",         "5000" },
        { "machine_max_jerk_x",           "9,9" },
        { "machine_max_jerk_y",           "9,9" },
        { "machine_max_junction_deviation", "0.01,0.01" },
    });
    return config;
}

} // namespace

// Simplifying a contour of radius R by a deviation t leaves turns of up to sqrt(8 t / R) at the
// vertices it keeps, and a junction deviation planner only stops charging for a turn once it drops
// below sqrt(8 * junction_deviation / R). The radius cancels, so a wall must not be decimated more
// coarsely than the junction deviation the firmware plans with, or the toolhead decelerates into
// every vertex Douglas-Peucker left behind. Klipper's junction deviation is
// square_corner_velocity^2 * (sqrt(2) - 1) / acceleration, so the faster the wall accelerates the
// finer its contour has to be kept, down to the quarter-of-resolution floor.
TEST_CASE("A curved wall is refined to what the firmware can corner through", "[Perimeters]")
{
    // Radius 8 mm, tessellated at the 2 degrees per facet that its_make_cylinder defaults to.
    const TriangleMesh cylinder = make_cylinder(8., 6.);

    auto wall_turns = [&cylinder](const DynamicPrintConfig &config) {
        Print print;
        init_and_process_print({cylinder}, print, config);
        return external_perimeter_turns(print, 3.0);
    };

    // Marlin legacy corners with classic jerk, which has neither a junction deviation nor a
    // curvature term, so there is nothing to align the decimation to and `resolution` stands.
    const std::vector<double> jerk = wall_turns(cylinder_config("marlin"));
    // Klipper at 5000 mm/s^2: 9^2 * (sqrt(2) - 1) / 5000 = 0.0067 mm, under the 0.012 mm resolution.
    const std::vector<double> klipper = wall_turns(cylinder_config("klipper"));
    // Klipper at 20000 mm/s^2: 0.0017 mm, so the floor at a quarter of `resolution` takes over.
    DynamicPrintConfig fast = cylinder_config("klipper");
    fast.set_deserialize_strict({{ "outer_wall_acceleration", "20000" }});
    const std::vector<double> klipper_fast = wall_turns(fast);

    REQUIRE(jerk.size() > 8);

    // The harder the firmware would brake for a turn, the less of the curve is decimated away.
    CHECK(klipper.size() > jerk.size());
    CHECK(klipper_fast.size() > klipper.size());
    CHECK(*std::max_element(klipper_fast.begin(), klipper_fast.end()) <
          *std::max_element(jerk.begin(), jerk.end()));
}

// The clamp only ever refines, so a flavor Orca models as classic jerk keeps exactly the contour
// `resolution` asks for, and no printer gets a coarser wall than before.
TEST_CASE("A classic jerk flavor keeps the contour resolution asks for", "[Perimeters]")
{
    const TriangleMesh cylinder = make_cylinder(8., 6.);

    Print jerk_print;
    init_and_process_print({cylinder}, jerk_print, cylinder_config("marlin"));

    DynamicPrintConfig zero_jd = cylinder_config("marlin");
    zero_jd.set_deserialize_strict({{ "machine_max_junction_deviation", "0,0" }});
    Print zero_jd_print;
    init_and_process_print({cylinder}, zero_jd_print, zero_jd);

    CHECK(external_perimeter_turns(jerk_print, 3.0).size() == external_perimeter_turns(zero_jd_print, 3.0).size());
}
