// End-to-end test for the outer-wall-filament feature: with wall_filament=1
// and surface_wall_override_filament=2, the slicer should route the outermost N perimeter
// loops through filament 2 and the rest through filament 1 within a single
// layer.
//
// Reproduces the user-reported bug where setting surface_wall_override_filament in the
// GUI had no visible effect — the G-code only used the base wall_filament.
//
// Coverage matrix (8 scenarios = 4 configurations × 2 BBL flags):
//
//   - SEMM/AMS (single nozzle, multi filament) × surface_wall_override_filament on/off
//   - Dual-nozzle/H2D (two nozzles) × surface_wall_override_filament on/off
//   - each of the above × is_BBL_printer = {true, false}
//
// We inspect Print's ToolOrdering + per-layer perimeter collections directly
// rather than the produced G-code: it isolates the routing decision (the same
// one the per-extruder bucket split in GCode.cpp consumes) from G-code
// emission concerns.

#include <catch2/catch_all.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

struct RoutingObservations {
    int  num_layer_tools                = 0;
    int  layers_with_both_filaments     = 0;
    bool any_layer_uses_filament_1      = false;
    bool any_layer_uses_filament_2      = false;
    // Counts of perimeter extrusion entities (loops + multipaths produced by
    // both classic and arachne wall generators) split by their per-entity
    // extruder_override value:
    //   - overridden_to_outer:  override matches the configured surface_wall_override_filament
    //   - overridden_other:     override is set but to some other filament (unexpected)
    //   - not_overridden:       no override (uses base wall_filament)
    int  perimeter_overridden_to_outer  = 0;
    int  perimeter_overridden_other     = 0;
    int  perimeter_not_overridden       = 0;
};

// Slice with the given config (no arrangement step — just place the cube at
// origin) and walk the resulting Print's ToolOrdering + per-island perimeter
// collections to count which extruder each wall loop is routed to.
//
// The `is_bbl` flag lets the test exercise both Bambu-flavored
// (BBL-only filament/nozzle group checks, WipeTower Type1, etc.) and the
// generic Marlin/Klipper code paths.
RoutingObservations slice_and_observe(std::initializer_list<TriangleMesh> meshes,
                                      const DynamicPrintConfig &config_in,
                                      bool is_bbl)
{
    Print  print;
    Model  model;
    // Print's m_isBBLPrinter is normally written by the GUI from preset
    // metadata; in libslic3r-only tests we set it explicitly so each scenario
    // is reproducible regardless of default-init.
    print.is_BBL_printer() = is_bbl;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.apply(config_in);

    for (TriangleMesh tri : meshes) {
        ModelObject *object = model.add_object();
        object->name += "object.stl";
        object->add_volume(std::move(tri));
        object->add_instance();
    }
    for (ModelObject *mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    RoutingObservations obs;

    const ToolOrdering &to = print.tool_ordering();
    for (auto it = to.begin(); it != to.end(); ++it) {
        ++obs.num_layer_tools;
        bool f1 = false, f2 = false;
        for (unsigned int eid : it->extruders) {
            if (eid == 0) f1 = true;   // 0-based: filament 1
            if (eid == 1) f2 = true;   // 0-based: filament 2
        }
        obs.any_layer_uses_filament_1 |= f1;
        obs.any_layer_uses_filament_2 |= f2;
        if (f1 && f2) ++obs.layers_with_both_filaments;
    }

    // Per-loop routing: walk every perimeter entity on every layer/region and
    // count by per-entity extruder_override. Both wall generators (classic,
    // arachne) set this on ExtrusionLoop and ExtrusionMultiPath; we don't
    // depend on inset_idx since Arachne leaves it at the default -1.
    const int8_t outer_filament_cfg = (int8_t)
        print.full_print_config().opt_int("surface_wall_override_filament");
    auto count_entity = [&](const ExtrusionEntity *ee) {
        const int8_t ov = ee->extruder_override;
        if (ov <= 0)
            ++obs.perimeter_not_overridden;
        else if (outer_filament_cfg > 0 && ov == outer_filament_cfg)
            ++obs.perimeter_overridden_to_outer;
        else
            ++obs.perimeter_overridden_other;
    };
    std::function<void(const ExtrusionEntityCollection&)> visit =
        [&](const ExtrusionEntityCollection &col) {
            for (const ExtrusionEntity *ee : col.entities) {
                if (const auto *sub = dynamic_cast<const ExtrusionEntityCollection*>(ee))
                    visit(*sub);
                else
                    count_entity(ee);
            }
        };
    for (const PrintObject *po : print.objects())
        for (const Layer *layer : po->layers())
            for (const LayerRegion *lr : layer->regions())
                visit(lr->perimeters);

    return obs;
}

// ---- Config builders ------------------------------------------------------

// Per-filament defaults that the multi-filament dispatch path needs sized to
// filament_count, regardless of nozzle topology.
//
//   filament_printable: bitmask, bit i = filament can print on extruder i.
//                       Default {3} (single entry) does not auto-extend.
//   filament_colour / filament_density: needed by reorder_extruders /
//                       calc_filament_change_info_by_toolorder; OOB on default.
//   flush_volumes_matrix / flush_multiplier: needed for flush volume calc.
//   filament_map: 1-based map filament -> extruder. {1,1} keeps both filaments
//                 on a single nozzle so the SEMM/AMS path doesn't try to send
//                 work to an extruder that doesn't exist.
static void apply_common_two_filament_defaults(DynamicPrintConfig &config) {
    config.set_deserialize_strict({
        { "filament_printable",                "3,3" },
        { "filament_colour",                   "#FF0000;#0000FF" },
        { "filament_density",                  "1.24;1.24" },
        { "flush_volumes_matrix",              "0,140,140,0" },
        { "flush_multiplier",                  "0.3" },
        { "filament_map",                      "1,1" },
    });
}

DynamicPrintConfig make_semm_config(int surface_wall_override_filament_value) {
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "nozzle_diameter",                   "0.4" },
        { "filament_diameter",                 "1.75;1.75" },
        { "filament_type",                     "PLA;PLA" },
        { "filament_settings_id",              "Generic PLA;Generic PLA" },
        { "single_extruder_multi_material",    true },

        { "wall_loops",                        2 },
        { "outer_wall_filament_id",            1 },
        { "inner_wall_filament_id",            1 },
        { "outer_wall_count",                  1 },

        { "sparse_infill_filament_id",         1 },
        { "internal_solid_filament_id",        1 },
        { "top_surface_filament_id",           1 },
        { "bottom_surface_filament_id",        1 },
        { "sparse_infill_density",             "20%" },
        { "wall_sequence",                     "inner wall/outer wall" },
        { "enable_prime_tower",                false },
    });
    apply_common_two_filament_defaults(config);
    config.set("surface_wall_override_filament", surface_wall_override_filament_value);
    return config;
}

DynamicPrintConfig make_dual_nozzle_config(int surface_wall_override_filament_value) {
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "nozzle_diameter",                   "0.4;0.4" },
        { "filament_diameter",                 "1.75;1.75" },
        { "filament_type",                     "PLA;PLA" },
        { "filament_settings_id",              "Generic PLA;Generic PLA" },
        { "single_extruder_multi_material",    false },
        { "extruder_offset",                   "0x0,18x0" },

        { "wall_loops",                        2 },
        { "outer_wall_filament_id",            1 },
        { "inner_wall_filament_id",            1 },
        { "outer_wall_count",                  1 },

        { "sparse_infill_filament_id",         1 },
        { "internal_solid_filament_id",        1 },
        { "top_surface_filament_id",           1 },
        { "bottom_surface_filament_id",        1 },
        { "sparse_infill_density",             "20%" },
        { "wall_sequence",                     "inner wall/outer wall" },
        { "enable_prime_tower",                false },
    });
    apply_common_two_filament_defaults(config);
    config.set("surface_wall_override_filament", surface_wall_override_filament_value);
    return config;
}

// ---- Shared assertions ----------------------------------------------------

void assert_routing_active(const RoutingObservations &obs)
{
    INFO("layer_tools="           << obs.num_layer_tools
        << " layers_with_both="   << obs.layers_with_both_filaments
        << " f1="                 << obs.any_layer_uses_filament_1
        << " f2="                 << obs.any_layer_uses_filament_2
        << " peri_to_outer="      << obs.perimeter_overridden_to_outer
        << " peri_other="         << obs.perimeter_overridden_other
        << " peri_no_override="   << obs.perimeter_not_overridden);

    REQUIRE(obs.num_layer_tools > 5);
    REQUIRE(obs.any_layer_uses_filament_1);
    REQUIRE(obs.any_layer_uses_filament_2);
    REQUIRE(obs.layers_with_both_filaments >= 5);
    REQUIRE(obs.perimeter_overridden_to_outer > 0);
    REQUIRE(obs.perimeter_overridden_other == 0);
    REQUIRE(obs.perimeter_not_overridden > 0);
}

void assert_routing_disabled(const RoutingObservations &obs)
{
    INFO("layer_tools="           << obs.num_layer_tools
        << " f1="                 << obs.any_layer_uses_filament_1
        << " f2="                 << obs.any_layer_uses_filament_2
        << " peri_to_outer="      << obs.perimeter_overridden_to_outer
        << " peri_other="         << obs.perimeter_overridden_other);

    REQUIRE(obs.any_layer_uses_filament_1);
    REQUIRE(!obs.any_layer_uses_filament_2);
    REQUIRE(obs.perimeter_overridden_to_outer == 0);
    REQUIRE(obs.perimeter_overridden_other == 0);
    REQUIRE(obs.layers_with_both_filaments == 0);
}

} // namespace

// ---------------------------------------------------------------------------
// SEMM / AMS-style: one physical nozzle, two filament slots in the AMS.
// This is the configuration the user originally reported broken.
// ---------------------------------------------------------------------------

TEST_CASE("SEMM/AMS: surface_wall_override_filament routes outermost loop to filament 2",
          "[surface_wall_override_filament][semm]")
{
    const bool is_bbl = GENERATE(false, true);
    DYNAMIC_SECTION("is_BBL_printer=" << (is_bbl ? "true" : "false")) {
        const auto obs = slice_and_observe(
            { cube(20) },
            make_semm_config(/*surface_wall_override_filament=*/2),
            is_bbl);
        assert_routing_active(obs);
    }
}

TEST_CASE("SEMM/AMS: surface_wall_override_filament=0 (disabled) keeps single-filament routing",
          "[surface_wall_override_filament][semm]")
{
    const bool is_bbl = GENERATE(false, true);
    DYNAMIC_SECTION("is_BBL_printer=" << (is_bbl ? "true" : "false")) {
        const auto obs = slice_and_observe(
            { cube(20) },
            make_semm_config(/*surface_wall_override_filament=*/0),
            is_bbl);
        assert_routing_disabled(obs);
    }
}

// ---------------------------------------------------------------------------
// Dual-nozzle (H2D-style): two physical nozzles. The routing decision must be
// the same as SEMM — the outer N loops carry an extruder_override pointing at
// surface_wall_override_filament regardless of nozzle topology.
// ---------------------------------------------------------------------------

TEST_CASE("Dual-nozzle/H2D: surface_wall_override_filament routes outermost loop to filament 2",
          "[surface_wall_override_filament][dual_nozzle]")
{
    const bool is_bbl = GENERATE(false, true);
    DYNAMIC_SECTION("is_BBL_printer=" << (is_bbl ? "true" : "false")) {
        const auto obs = slice_and_observe(
            { cube(20) },
            make_dual_nozzle_config(/*surface_wall_override_filament=*/2),
            is_bbl);
        assert_routing_active(obs);
    }
}

TEST_CASE("Dual-nozzle/H2D: surface_wall_override_filament=0 (disabled) keeps single-filament routing",
          "[surface_wall_override_filament][dual_nozzle]")
{
    const bool is_bbl = GENERATE(false, true);
    DYNAMIC_SECTION("is_BBL_printer=" << (is_bbl ? "true" : "false")) {
        const auto obs = slice_and_observe(
            { cube(20) },
            make_dual_nozzle_config(/*surface_wall_override_filament=*/0),
            is_bbl);
        assert_routing_disabled(obs);
    }
}
