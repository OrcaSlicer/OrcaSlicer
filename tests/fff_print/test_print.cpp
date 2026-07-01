#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"

#include "test_data.hpp"

#include <algorithm>

using namespace Slic3r;
using namespace Slic3r::Test;

SCENARIO("Print: Skirt generation", "[Print]") {
    GIVEN("20mm cube and default config") {
        WHEN("skirt_loops is set to 2")  {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
                { "skirt_height",   1 },
                { "skirt_distance", 1 },
                { "skirt_loops",    2 }
            });
            THEN("Skirt Extrusion collection has 2 loops in it") {
                REQUIRE(print.skirt().items_count() == 2);
                REQUIRE(print.skirt().flatten().entities.size() == 2);
            }
        }
    }
}

SCENARIO("Print: Changing number of solid shell layers does not cause all surfaces to become internal.", "[Print]") {
    GIVEN("sliced 20mm cube and config with top_shell_layers = 2 and bottom_shell_layers = 1") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
		config.set_deserialize_strict({
            { "top_shell_layers",           2 },
            { "bottom_shell_layers",        1 },
            { "layer_height",               0.25 }, // get a known number of layers
            { "initial_layer_print_height", 0.25 }
			});
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::Test::init_print({TestMesh::cube_20x20x20}, print, model, config);
        // Precondition: Ensure that the model has 2 solid top layers (79, 78)
        // and one solid bottom layer (0).
		auto test_is_solid_infill = [&print](size_t obj_id, size_t layer_id) {
		    const Layer &layer = *(print.objects().at(obj_id)->get_layer((int)layer_id));
		    // iterate over all of the regions in the layer
		    for (const LayerRegion *region : layer.regions()) {
		        // for each region, iterate over the fill surfaces
		        for (const Surface &surface : region->fill_surfaces.surfaces)
		            CHECK(surface.is_solid());
		    }
		};
        print.process();
        test_is_solid_infill(0,  0); // should be solid
        test_is_solid_infill(0, 79); // should be solid
        test_is_solid_infill(0, 78); // should be solid
        WHEN("Model is re-sliced with top_shell_layers == 3") {
			config.set("top_shell_layers", 3);
			print.apply(model, config);
            print.process();
            THEN("Print object does not have 0 solid bottom layers.") {
                test_is_solid_infill(0, 0);
            }
            AND_THEN("Print object has 3 top solid layers") {
                test_is_solid_infill(0, 79);
                test_is_solid_infill(0, 78);
                test_is_solid_infill(0, 77);
            }
        }
    }
}

SCENARIO("Print: Brim generation", "[Print]") {
    GIVEN("20mm cube and default config, 1mm first layer width") {
        WHEN("Brim is set to 6mm")  {
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
                    { "brim_type",                "outer_only" },
                    { "initial_layer_line_width", 1 },
                    { "brim_width",               6 }
	        });
            THEN("Brim Extrusion collection has 6 loops in it") {
                size_t total_items = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_items += pair.second.items_count();
                }
                REQUIRE(total_items == 6);
            }
        }
        WHEN("Brim is set to 6mm, extrusion width 0.5mm")  {
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {
                    { "brim_type",                "outer_only" },
                    { "brim_width",               6 },
                    { "initial_layer_line_width", 0.5 }
	        });
            THEN("Brim Extrusion collection has 12 loops in it") {
                size_t total_items = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_items += pair.second.items_count();
                }
                REQUIRE(total_items == 12);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Print::validate() warning collection
//
// validate() returns its warnings in a vector. The warning paths deliberately
// differ in how many entries they produce; these tests pin down each behaviour:
//   * independent checks    -> stack (one entry each)
//   * motion-ability        -> coalesce into one (mutually exclusive, gated)
//   * clumping detection    -> one independent warning
//   * layered clearance     -> many collisions concatenated into one entry
//   * null warnings pointer -> no-op, no crash, no blocking error
// ---------------------------------------------------------------------------
namespace {

// Build `n` 20mm cubes (spread apart, or stacked at the origin when `overlap`) into
// `model`/`print` and apply `config`, leaving the print ready to validate(). No slicing needed.
void build_cubes(Slic3r::Model& model, Slic3r::Print& print,
                 DynamicPrintConfig config, int n, bool overlap)
{
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0\n")); // validate() relative-E reset

    for (int i = 0; i < n; ++i) {
        ModelObject* object = model.add_object();
        object->add_volume(Slic3r::Test::mesh(TestMesh::cube_20x20x20));
        ModelInstance* inst = object->add_instance();
        inst->set_offset(Vec3d(overlap ? 0.0 : i * 60.0, 0.0, 0.0));
    }
    for (ModelObject* mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }
    print.apply(model, config);
}

// Build cubes and run validate(), collecting warnings; returns the blocking error.
StringObjectException validate_cubes(const DynamicPrintConfig& config,
                                     std::vector<StringObjectException>& warnings,
                                     int n = 1, bool overlap = false)
{
    Slic3r::Model model;
    Slic3r::Print print;
    build_cubes(model, print, config, n, overlap);
    return print.validate(&warnings);
}

size_t count_opt_key(const std::vector<StringObjectException>& warnings, const std::string& key)
{
    return std::count_if(warnings.begin(), warnings.end(),
        [&](const StringObjectException& w) { return w.opt_key == key; });
}

// Make `default_acceleration` exceed the machine's extruding-acceleration limit.
void trigger_acceleration_warning(DynamicPrintConfig& c)
{
    c.set_key_value("machine_max_acceleration_extruding", new ConfigOptionFloats{ 100. });
    c.set_key_value("default_acceleration", new ConfigOptionFloatsNullable{ 100000. });
}

// Make `default_jerk` exceed the machine's jerk limit (junction deviation off so
// the jerk check is not skipped).
void trigger_jerk_warning(DynamicPrintConfig& c)
{
    c.set_key_value("machine_max_junction_deviation", new ConfigOptionFloats{ 0. });
    c.set_key_value("machine_max_jerk_x", new ConfigOptionFloats{ 1. });
    c.set_key_value("machine_max_jerk_y", new ConfigOptionFloats{ 1. });
    c.set_key_value("default_jerk", new ConfigOptionFloatsNullable{ 9999. });
}

// Precise outer wall is ignored unless the wall sequence is inner-outer.
void trigger_precise_wall_warning(DynamicPrintConfig& c)
{
    c.set_key_value("precise_outer_wall", new ConfigOptionBool(true));
    c.set_key_value("wall_sequence", new ConfigOptionEnum<WallSequence>(WallSequence::OuterInner));
}

} // namespace

TEST_CASE("Print::validate stacks independent warnings", "[Print][validate]")
{
    // Two unrelated checks (region precise-wall + machine acceleration) must each
    // contribute their own entry.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    trigger_precise_wall_warning(config);
    trigger_acceleration_warning(config);

    std::vector<StringObjectException> warnings;
    StringObjectException err = validate_cubes(config, warnings);

    CHECK(err.string.empty());
    CHECK(warnings.size() >= 2);
    CHECK(count_opt_key(warnings, "precise_outer_wall") == 1);  // jump-to key is preserved
    for (const auto& w : warnings)
        CHECK(w.is_warning);                                   // every collected entry is a warning
}

TEST_CASE("Print::validate coalesces motion-ability warnings into one", "[Print][validate]")
{
    // The jerk/junction/acceleration checks are mutually exclusive (gated on a shared
    // key), so adding a second motion trigger must NOT add a second warning.
    DynamicPrintConfig accel_only = DynamicPrintConfig::full_print_config();
    trigger_acceleration_warning(accel_only);
    std::vector<StringObjectException> w_accel;
    CHECK(validate_cubes(accel_only, w_accel).string.empty());

    DynamicPrintConfig accel_and_jerk = DynamicPrintConfig::full_print_config();
    trigger_acceleration_warning(accel_and_jerk);
    trigger_jerk_warning(accel_and_jerk);
    std::vector<StringObjectException> w_both;
    CHECK(validate_cubes(accel_and_jerk, w_both).string.empty());

    CHECK(w_accel.size() >= 1);
    CHECK(w_both.size() == w_accel.size());  // the extra motion trigger collapses into the same warning
}

TEST_CASE("Print::validate reports the clumping-detection warning", "[Print][validate]")
{
    // A distinct single-shot path: clumping/wrapping detection without a prime tower warns
    // (and carries the enable_prime_tower jump-to key). enable_prime_tower must be off, as
    // the warning lives in the no-prime-tower branch.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    config.set_key_value("enable_wrapping_detection", new ConfigOptionBool(true));

    std::vector<StringObjectException> warnings;
    StringObjectException err = validate_cubes(config, warnings);

    CHECK(err.string.empty());
    CHECK(count_opt_key(warnings, "enable_prime_tower") == 1);
}

TEST_CASE("Print::validate concatenates layered-clearance collisions into one warning", "[Print][validate]")
{
    // In by-layer mode, layered_print_cleareance_valid folds every too-close pair into a
    // single warning entry (newline-joined), unlike the per-check stacking above. Isolate
    // that entry by type so unrelated default-config warnings don't affect the assertion.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    std::vector<StringObjectException> warnings;
    StringObjectException err = validate_cubes(config, warnings, /*n=*/3, /*overlap=*/true);

    CHECK(err.string.empty());
    auto is_layered = [](const StringObjectException& w) {
        return w.type == STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT; };
    REQUIRE(std::count_if(warnings.begin(), warnings.end(), is_layered) == 1);  // 3 objects, 2 collisions, 1 entry
    auto it = std::find_if(warnings.begin(), warnings.end(), is_layered);
    CHECK(it->string.find('\n') != std::string::npos);  // the collisions were concatenated
}

TEST_CASE("Print::validate tolerates a null warnings pointer", "[Print][validate]")
{
    // Callers may pass no warnings sink: a warning-producing config must not crash
    // and must still return without a blocking error.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    trigger_precise_wall_warning(config);
    trigger_acceleration_warning(config);

    Slic3r::Model model;
    Slic3r::Print print;
    build_cubes(model, print, config, /*n=*/1, /*overlap=*/false);

    StringObjectException err = print.validate();  // warnings == nullptr
    CHECK(err.string.empty());
}
