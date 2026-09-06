#include <catch2/catch_all.hpp>

#include <numeric>
#include <sstream>

#include "test_helpers.hpp" // get access to init_print, etc

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r::Test;
using namespace Slic3r;

TEST_CASE("Filament wall flow inherits and overrides the process gate", "[Flow][Regression]")
{
    const bool process_gate = GENERATE(false, true);
    const std::string filament_gate = GENERATE(std::string("nil"), std::string("0"), std::string("1"));
    const bool override_ratio = GENERATE(false, true);
    auto config = multifilament_config(2, {
        {"wall_loops", "1"}, {"sparse_infill_density", "0%"},
        {"top_shell_layers", "0"}, {"bottom_shell_layers", "0"},
        {"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"},
        {"outer_wall_filament_id", "2"}, {"enable_arc_fitting", "0"},
        {"brim_type", "no_brim"}, {"skirt_loops", "0"},
        {"use_relative_e_distances", "1"}, {"seam_slope_type", "none"},
        {"first_layer_flow_ratio", "1"}, {"outer_wall_flow_ratio", "1"},
        {"set_other_flow_ratios", "0"},
        {"filament_self_index", "1,2"},
        {"filament_extruder_variant", "Direct Drive Standard;Direct Drive Standard"}
    });

    const auto wall_extrusion = [&](const DynamicPrintConfig& cfg) {
        Print print;
        Model model;
        init_print(std::vector<TriangleMesh>{cube(4)}, print, model, cfg, nullptr, false);
        const std::string output = gcode(print);
        GCodeReader reader;
        reader.apply_config(cfg);
        double extrusion = 0.;
        reader.parse_buffer(output, [&](GCodeReader& self, const GCodeReader::GCodeLine& line) {
            if (line.extruding(self) && line.dist_XY(self) > 0 &&
                line.comment().find("perimeter") != std::string_view::npos)
                extrusion += line.dist_E(self);
        });
        return extrusion;
    };

    // Leave defaults at one element: older and CLI configs need the get_at() fallback
    // even when the path uses the second filament.
    const double baseline = wall_extrusion(config);
    REQUIRE(baseline > 0.);
    config.set_deserialize_strict("set_other_flow_ratios", process_gate ? "1" : "0");
    config.set_deserialize_strict("outer_wall_flow_ratio", "0.8");
    config.set_deserialize_strict("filament_set_other_flow_ratios", filament_gate);
    config.set_deserialize_strict("filament_outer_wall_flow_ratio", override_ratio ? "0.9,1.2" : "nil");
    const bool effective_gate = filament_gate == "nil" ? process_gate : filament_gate == "1";
    const double expected_ratio = effective_gate ? (override_ratio ? 1.2 : 0.8) : 1.;
    REQUIRE_THAT(wall_extrusion(config) / baseline, Catch::Matchers::WithinRel(expected_ratio, 0.001));
}

TEST_CASE("Filament flow overrides follow nozzle variant expansion", "[Flow][H2C][Regression]")
{
    using namespace Slic3r::MultiNozzleUtils;
    auto config = multifilament_config(1);
    config.option<ConfigOptionFloats>("nozzle_diameter", true)->values = {0.4, 0.4};
    config.option<ConfigOptionStrings>("extruder_nozzle_stats", true)->values = {"Standard#1", "Standard#1|High Flow#2"};
    config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values = {etDirectDrive, etDirectDrive};
    config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = {nvtStandard, nvtHybrid};
    config.option<ConfigOptionInts>("filament_map", true)->values = {2};
    config.option<ConfigOptionInts>("filament_volume_map", true)->values = {int(nvtStandard)};
    config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = {
        "Direct Drive Standard,Direct Drive High Flow", "Direct Drive Standard,Direct Drive High Flow"};
    config.option<ConfigOptionInts>("print_extruder_id", true)->values = {1, 1, 2, 2};
    config.option<ConfigOptionStrings>("print_extruder_variant", true)->values = {
        "Direct Drive Standard", "Direct Drive High Flow", "Direct Drive Standard", "Direct Drive High Flow"};
    config.option<ConfigOptionInts>("filament_self_index", true)->values = {1, 1};
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = {"Direct Drive Standard", "Direct Drive High Flow"};

    const std::vector<std::string> flow_keys = {
        "filament_first_layer_flow_ratio", "filament_top_solid_infill_flow_ratio",
        "filament_bottom_solid_infill_flow_ratio", "filament_outer_wall_flow_ratio",
        "filament_inner_wall_flow_ratio", "filament_overhang_flow_ratio",
        "filament_sparse_infill_flow_ratio", "filament_internal_solid_infill_flow_ratio",
        "filament_gap_fill_flow_ratio", "filament_brim_flow_ratio",
        "filament_support_flow_ratio", "filament_support_interface_flow_ratio"
    };
    for (const auto& key : flow_keys)
        config.set_deserialize_strict(key, "0.8,1.2");
    config.set_deserialize_strict("filament_set_other_flow_ratios", "nil,1");

    Print print;
    Model model;
    init_print(std::vector<TriangleMesh>{cube(4)}, print, model, config, nullptr, false);
    std::vector<NozzleInfo> nozzles(2);
    for (auto& nozzle : nozzles) {
        nozzle.diameter = "0.4";
        nozzle.extruder_id = 1;
    }
    nozzles[0].volume_type = nvtStandard;
    nozzles[0].group_id = 0;
    nozzles[1].volume_type = nvtHighFlow;
    nozzles[1].group_id = 1;
    auto group = LayeredNozzleGroupResult::create({{0}, {1}}, nozzles, {0}, {{0}, {0}});
    REQUIRE(group.has_value());
    print.set_nozzle_group_result(std::make_shared<LayeredNozzleGroupResult>(*group));
    print.update_to_config_by_nozzle_group_result(*group);

    const size_t standard_slot = print.get_filament_config_indx(0, 0);
    const size_t high_flow_slot = print.get_filament_config_indx(0, 1);
    REQUIRE(standard_slot != high_flow_slot);
    for (const auto& key : flow_keys) {
        CAPTURE(key);
        const auto* values = print.config().option<ConfigOptionFloatsNullable>(key);
        REQUIRE(values != nullptr);
        REQUIRE(values->size() > high_flow_slot);
        REQUIRE_THAT(values->get_at(standard_slot), Catch::Matchers::WithinAbs(0.8, 1e-9));
        REQUIRE_THAT(values->get_at(high_flow_slot), Catch::Matchers::WithinAbs(1.2, 1e-9));
    }
    const auto* gate = print.config().option<ConfigOptionBoolsNullable>("filament_set_other_flow_ratios");
    REQUIRE(gate != nullptr);
    REQUIRE(gate->size() > high_flow_slot);
    REQUIRE(gate->is_nil(standard_slot));
    REQUIRE(gate->get_at(high_flow_slot));
}

/// Test the expected behavior for auto-width,
/// spacing, etc
SCENARIO("Flow math for non-bridges", "[Flow]") {
    GIVEN("Nozzle Diameter of 0.4, a desired width of 1mm and layer height of 0.5") {
        ConfigOptionFloatOrPercent	width(1.0, false);
        float nozzle_diameter	= 0.4f;
        float layer_height		= 0.4f;

        // Spacing for non-bridges is has some overlap
        THEN("External perimeter flow has spacing fixed to 1.125 * nozzle_diameter") {
            auto flow = Flow::new_from_config_width(frExternalPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
            REQUIRE(flow.spacing() == Catch::Approx(1.125 * nozzle_diameter - layer_height * (1.0 - PI / 4.0)));
        }

        THEN("Internal perimeter flow has spacing fixed to 1.125 * nozzle_diameter") {
            auto flow = Flow::new_from_config_width(frPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
            REQUIRE(flow.spacing() == Catch::Approx(1.125 *nozzle_diameter - layer_height * (1.0 - PI / 4.0)));
        }
        THEN("Spacing for supplied width is 0.8927f") {
            auto flow = Flow::new_from_config_width(frExternalPerimeter, width, nozzle_diameter, layer_height);
            REQUIRE(flow.spacing() == Catch::Approx(width.value - layer_height * (1.0 - PI / 4.0)));
            flow = Flow::new_from_config_width(frPerimeter, width, nozzle_diameter, layer_height);
            REQUIRE(flow.spacing() == Catch::Approx(width.value - layer_height * (1.0 - PI / 4.0)));
        }
    }
    /// Check the min/max
    GIVEN("Nozzle Diameter of 0.25") {
        float nozzle_diameter	= 0.25f;
        float layer_height		= 0.5f;
        WHEN("layer height is set to 0.2") {
            layer_height = 0.15f;
            THEN("Max width is set.") {
                auto flow = Flow::new_from_config_width(frPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
                REQUIRE(flow.width() == Catch::Approx(1.125 * nozzle_diameter));
            }
        }
        WHEN("Layer height is set to 0.25") {
            layer_height = 0.25f;
            THEN("Min width is set.") {
                auto flow = Flow::new_from_config_width(frPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
                REQUIRE(flow.width() == Catch::Approx(1.125 * nozzle_diameter));
            }
        }
    }

#if 0
    /// Check for an edge case in the maths where the spacing could be 0; original
    /// math is 0.99. Slic3r issue #4654
    GIVEN("Input spacing of 0.414159 and a total width of 2") {
        double in_spacing = 0.414159;
        double total_width = 2.0;
        auto flow = Flow::new_from_spacing(1.0, 0.4, 0.3);
        WHEN("solid_spacing() is called") {
            double result = flow.solid_spacing(total_width, in_spacing);
            THEN("Yielded spacing is greater than 0") {
                REQUIRE(result > 0);
            }
        }
    }
#endif    

}

/// Spacing, width calculation for bridge extrusions
SCENARIO("Flow math for bridges", "[Flow]") {
    GIVEN("Nozzle Diameter of 0.4, a desired width of 1mm and layer height of 0.5") {
		float nozzle_diameter	= 0.4f;
		float bridge_flow		= 1.0f;
        WHEN("Flow role is frExternalPerimeter") {
            auto flow = Flow::bridging_flow(nozzle_diameter * sqrt(bridge_flow), nozzle_diameter);
            THEN("Bridge width is same as nozzle diameter") {
                REQUIRE(flow.width() == Catch::Approx(nozzle_diameter));
            }
            THEN("Bridge spacing is same as nozzle diameter + BRIDGE_EXTRA_SPACING") {
                REQUIRE(flow.spacing() == Catch::Approx(nozzle_diameter + BRIDGE_EXTRA_SPACING));
            }
        }
    }
}
