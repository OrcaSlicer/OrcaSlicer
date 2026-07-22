#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Model.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Count of (non-overlapping) occurrences of `needle` in `haystack`.
int count_occurrences(const std::string &haystack, const std::string &needle)
{
    int count = 0;
    for (auto pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + needle.size()))
        ++count;
    return count;
}

// Builds a 20mm base cube plus an overlapping modifier volume (also cuboid, placed at the
// origin so its raw mesh coordinates directly define its footprint/extents within the object,
// per make_cube()'s (0,0,0)-(x,y,z) convention) carrying the given enter/exit G-code.
void slice_cube_with_modifier(Vec3d modifier_dims, Vec3d modifier_offset,
                               const std::string &enter_gcode, const std::string &exit_gcode,
                               std::string &out_gcode)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               2 },
        { "initial_layer_print_height", 2 },
    });

    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(cube(20), ModelVolumeType::MODEL_PART, false);

    ModelVolume *modifier = object->add_volume(make_cube(modifier_dims.x(), modifier_dims.y(), modifier_dims.z()),
                                                ModelVolumeType::PARAMETER_MODIFIER, false);
    modifier->set_offset(modifier_offset);
    modifier->config.set_key_value("modifier_enter_gcode", new ConfigOptionString(enter_gcode));
    modifier->config.set_key_value("modifier_exit_gcode", new ConfigOptionString(exit_gcode));

    object->add_instance();
    object->ensure_on_bed();

    Print print;
    print.auto_assign_extruders(object);
    print.apply(model, config);

    out_gcode = Slic3r::Test::gcode(print);
}

} // namespace

TEST_CASE("Modifier region emits balanced enter/exit G-code on every layer it spans", "[ModifierGCode]") {
    // 10mm modifier cube at the object's bottom corner, coincident with two of the base cube's
    // own edges: the (merged, continuous) outer wall loop dips into and out of the modifier's
    // footprint as it wraps the corner, crossing its boundary twice per layer, on each of the
    // layers 1..5 (print_z 2,4,6,8,10) the modifier spans. No other setting differs from the
    // base object, so this also regression-tests that a G-code-only modifier still gets its
    // G-code bracketed even though its perimeter generation is merged with its parent's.
    std::string gcode;
    slice_cube_with_modifier(Vec3d(10, 10, 10), Vec3d(0, 0, 0), "; ENTER_MOD\n", "; EXIT_MOD\n", gcode);

    const int enter_count = count_occurrences(gcode, "; ENTER_MOD");
    const int exit_count  = count_occurrences(gcode, "; EXIT_MOD");

    CHECK(enter_count > 1); // fires again each time the toolpath re-enters the region, not once for the whole volume
    CHECK(exit_count > 1);
    CHECK(enter_count == exit_count); // every enter is matched by an exit
}

TEST_CASE("Modifier still active at the end of the print still emits its exit G-code", "[ModifierGCode]") {
    // Modifier is a thin slab covering the object's full footprint for its top 5mm (15mm-20mm):
    // the wall loop is "inside" it for the entirety of every layer it spans, so no crossing
    // occurs within those layers (enter fires once, via the resync at the first wall path whose
    // start point is newly inside the boundary). Without a trailing flush at export end, that
    // single "enter" would have no matching "exit" appended after it.
    std::string gcode;
    slice_cube_with_modifier(Vec3d(20, 20, 5), Vec3d(0, 0, 15), "; ENTER_MOD\n", "; EXIT_MOD\n", gcode);

    const int enter_count = count_occurrences(gcode, "; ENTER_MOD");
    const int exit_count  = count_occurrences(gcode, "; EXIT_MOD");

    CHECK(enter_count > 0);
    CHECK(enter_count == exit_count);
}

TEST_CASE("Modifier exit G-code is optional", "[ModifierGCode]") {
    // Only enter G-code configured: exit should simply be skipped (empty template -> no emission),
    // not crash or emit a stray marker.
    std::string gcode;
    slice_cube_with_modifier(Vec3d(10, 10, 10), Vec3d(0, 0, 0), "; ENTER_MOD\n", "", gcode);

    CHECK(count_occurrences(gcode, "; ENTER_MOD") > 0);
    CHECK(count_occurrences(gcode, "; EXIT_MOD") == 0);
}
