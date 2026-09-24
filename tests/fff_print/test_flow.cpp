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

static double extrusion_with_comment(const std::string &output, const DynamicPrintConfig &config, std::string_view comment)
{
    GCodeReader reader;
    reader.apply_config(config);
    double extrusion = 0.;
    reader.parse_buffer(output, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.extruding(self) && line.dist_XY(self) > 0 && line.comment().find(comment) != std::string_view::npos)
            extrusion += line.dist_E(self);
    });
    return extrusion;
}

TEST_CASE("Filament wall flow inherits and overrides the process gate", "[Flow][Regression]")
{
    const bool process_gate = GENERATE(false, true);
    const std::string filament_gate = GENERATE(std::string("nil"), std::string("0"), std::string("1"),
                                               std::string("0,1"), std::string("1,0"));
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
        return extrusion_with_comment(gcode(print), cfg, "perimeter");
    };

    // Leave defaults at one element: older and CLI configs need the get_at() fallback
    // even when the path uses the second filament.
    const double baseline = wall_extrusion(config);
    REQUIRE(baseline > 0.);
    config.set_deserialize_strict("set_other_flow_ratios", process_gate ? "1" : "0");
    config.set_deserialize_strict("outer_wall_flow_ratio", "0.8");
    config.set_deserialize_strict("filament_set_other_flow_ratios", filament_gate);
    config.set_deserialize_strict("filament_outer_wall_flow_ratio", override_ratio ? "0.9,1.2" : "nil");
    const bool effective_gate = filament_gate == "nil" ? process_gate : filament_gate.back() == '1';
    const double expected_ratio = effective_gate ? (override_ratio ? 1.2 : 0.8) : 1.;
    REQUIRE_THAT(wall_extrusion(config) / baseline, Catch::Matchers::WithinRel(expected_ratio, 0.001));
}

TEST_CASE("Explicit local top flow takes precedence over filament overrides", "[Flow][Regression]")
{
    const std::string scope = GENERATE(std::string("object"), std::string("part"),
                                      std::string("modifier"), std::string("height range"));
    auto config = multifilament_config(1, {
        {"wall_loops", "1"}, {"sparse_infill_density", "0%"},
        {"top_shell_layers", "1"}, {"bottom_shell_layers", "0"},
        {"top_shell_thickness", "0"}, {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"}, {"brim_type", "no_brim"},
        {"skirt_loops", "0"}, {"enable_arc_fitting", "0"}, {"gcode_comments", "1"},
        {"use_relative_e_distances", "1"}, {"seam_slope_type", "none"},
        {"set_other_flow_ratios", "0"}, {"top_solid_infill_flow_ratio", "1"}
    });
    auto top_extrusion = [&](bool local_override) {
        Print print;
        Model model;
        init_print(std::vector<TriangleMesh>{cube(4)}, print, model, config, nullptr, false);
        if (local_override) {
            ModelObject &object = *model.objects.front();
            ModelConfig *local = &object.config;
            if (scope == "part")
                local = &object.volumes.front()->config;
            else if (scope == "modifier") {
                ModelVolume *modifier = object.add_volume(cube(4));
                modifier->set_type(ModelVolumeType::PARAMETER_MODIFIER);
                local = &modifier->config;
            } else if (scope == "height range") {
                local = &object.layer_config_ranges[{0., 10.}];
                local->set_key_value("layer_height", new ConfigOptionFloat(0.2));
            }
            // Calibration sets exactly this object override. Equality to the process
            // default must not make it disappear during region deduplication.
            local->set_key_value("top_solid_infill_flow_ratio", new ConfigOptionFloat(1.));
            print.apply(model, config);
        }
        return extrusion_with_comment(gcode(print), config, "infill");
    };

    const double baseline = top_extrusion(false);
    REQUIRE(baseline > 0.);
    config.set_deserialize_strict("filament_top_solid_infill_flow_ratio", "0.8");
    CHECK_THAT(top_extrusion(false) / baseline, Catch::Matchers::WithinRel(0.8, 0.001));
    CHECK_THAT(top_extrusion(true) / baseline, Catch::Matchers::WithinRel(1., 0.001));
}

TEST_CASE("An explicit object flow gate overrides the filament gate", "[Flow][Regression]")
{
    const bool object_gate = GENERATE(false, true);
    auto config = multifilament_config(1, {
        {"wall_loops", "1"}, {"sparse_infill_density", "0%"},
        {"top_shell_layers", "0"}, {"bottom_shell_layers", "0"},
        {"brim_type", "no_brim"}, {"skirt_loops", "0"},
        {"use_relative_e_distances", "1"}, {"seam_slope_type", "none"},
        {"enable_arc_fitting", "0"}, {"outer_wall_flow_ratio", "1"},
        {"filament_outer_wall_flow_ratio", "0.8"}, {"first_layer_flow_ratio", "1"}
    });
    // The local gate intentionally equals the process default.
    config.set_deserialize_strict("set_other_flow_ratios", object_gate ? "1" : "0");
    config.set_deserialize_strict("filament_set_other_flow_ratios", object_gate ? "0" : "1");
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides = {
        {{"set_other_flow_ratios", object_gate ? "1" : "0"}}
    };
    auto walls = [&](bool local_override) {
        Print print;
        Model model;
        init_print(std::vector<TriangleMesh>{cube(4)}, print, model, config,
                   local_override ? &overrides : nullptr, false);
        return extrusion_with_comment(gcode(print), config, "perimeter");
    };
    const double inherited = walls(false);
    REQUIRE(inherited > 0.);
    const double expected = object_gate ? 0.8 : 1. / 0.8;
    CHECK_THAT(walls(true) / inherited, Catch::Matchers::WithinRel(expected, 0.001));
}

TEST_CASE("Empty and short filament override vectors safely inherit", "[Flow][Regression]")
{
    const size_t slot = GENERATE(size_t(0), size_t(1), size_t(7));
    ConfigOptionFloatsNullable ratios;
    ConfigOptionBoolsNullable gates;
    CHECK_FALSE(has_filament_override(&ratios, slot));
    CHECK_FALSE(has_filament_override(&gates, slot));
    CHECK_THAT(resolve_filament_override(ratios, slot, 0.8), Catch::Matchers::WithinAbs(0.8, 1e-9));
    CHECK(resolve_filament_override(gates, slot, true));
    ratios.values = {ConfigOptionFloatsNullable::nil_value()};
    gates.values = {ConfigOptionBoolsNullable::nil_value()};
    CHECK_FALSE(has_filament_override(&ratios, slot));
    CHECK_FALSE(has_filament_override(&gates, slot));
    ratios.values = {1.2};
    gates.values = {0};
    CHECK_THAT(resolve_filament_override(ratios, slot, 0.8), Catch::Matchers::WithinAbs(1.2, 1e-9));
    CHECK_FALSE(resolve_filament_override(gates, slot, true));
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
    config.set_num_extruders(2);
    // Flush matrices contain one block per physical extruder, even with one filament.
    config.set_deserialize_strict("flush_volumes_matrix", "0,0");
    config.option<ConfigOptionInts>("print_extruder_id", true)->values = {1, 1, 2, 2};
    config.option<ConfigOptionStrings>("print_extruder_variant", true)->values = {
        "Direct Drive Standard", "Direct Drive High Flow", "Direct Drive Standard", "Direct Drive High Flow"};
    config.option<ConfigOptionInts>("filament_self_index", true)->values = {1, 1};
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = {"Direct Drive Standard", "Direct Drive High Flow"};

    for (const auto &override : flow_ratio_overrides)
        if (std::string(override.key) != "set_other_flow_ratios")
            config.set_deserialize_strict(std::string("filament_") + override.key, "0.8,1.2");
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
    for (const auto &override : flow_ratio_overrides) {
        if (std::string(override.key) == "set_other_flow_ratios")
            continue;
        const std::string key = std::string("filament_") + override.key;
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

    config.set_deserialize_strict({
        {"wall_loops", "1"}, {"sparse_infill_density", "0%"},
        {"top_shell_layers", "0"}, {"bottom_shell_layers", "0"},
        {"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"},
        {"brim_type", "no_brim"}, {"skirt_loops", "0"},
        {"use_relative_e_distances", "1"}, {"seam_slope_type", "none"},
        {"enable_arc_fitting", "0"}, {"set_other_flow_ratios", "0"},
        {"filament_first_layer_flow_ratio", "nil"},
        {"filament_outer_wall_flow_ratio", "0.8,1.2"}
    });
    auto emitted_wall_flow = [&](const char *gate_values) {
        config.set_deserialize_strict("filament_set_other_flow_ratios", gate_values);
        Print sliced_print;
        Model sliced_model;
        init_print(std::vector<TriangleMesh>{cube(4)}, sliced_print, sliced_model, config, nullptr, false);
        sliced_print.process();
        const size_t layers = sliced_print.objects().front()->layer_count();
        REQUIRE(layers > 1);
        std::vector<std::vector<int>> layer_maps(layers, {1});
        layer_maps.front() = {0};
        auto layer_group = LayeredNozzleGroupResult::create(
            layer_maps, nozzles, {0}, std::vector<std::vector<unsigned int>>(layers, {0}));
        REQUIRE(layer_group.has_value());
        sliced_print.set_nozzle_group_result(std::make_shared<LayeredNozzleGroupResult>(*layer_group));
        sliced_print.update_to_config_by_nozzle_group_result(*layer_group);
        REQUIRE(sliced_print.get_filament_config_indx(0, 1) != 0);

        const std::string output = gcode(sliced_print);
        GCodeReader reader;
        reader.apply_config(config);
        std::array<double, 2> extrusion{};
        reader.parse_buffer(output, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
            if (line.extruding(self) && line.dist_XY(self) > 0 &&
                line.comment().find("perimeter") != std::string_view::npos)
                extrusion[self.z() < 0.3 ? 0 : 1] += line.dist_E(self);
        });
        return extrusion;
    };
    const auto baseline = emitted_wall_flow("nil,nil");
    REQUIRE(baseline[0] > 0.);
    REQUIRE(baseline[1] > 0.);
    const auto overridden = emitted_wall_flow("nil,1");
    CHECK_THAT(overridden[0] / baseline[0], Catch::Matchers::WithinRel(1., 0.001));
    CHECK_THAT(overridden[1] / baseline[1], Catch::Matchers::WithinRel(1.2, 0.001));

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


TEST_CASE("Explicit default flow keeps a separate print region", "[Flow][Regression]")
{
    auto config = multifilament_config(1, {
        {"top_solid_infill_flow_ratio", "1"}, {"filament_top_solid_infill_flow_ratio", "0.8"}
    });
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides = {
        {}, {{"top_solid_infill_flow_ratio", "1"}}
    };
    Print print;
    Model model;
    init_print(std::vector<TriangleMesh>{cube(4), cube(4)}, print, model, config, &overrides);
    REQUIRE(print.objects().size() == 2);
    CHECK(print.objects()[0]->printing_region(0).config() != print.objects()[1]->printing_region(0).config());
    model.objects.back()->config.erase("top_solid_infill_flow_ratio");
    print.apply(model, config);
    REQUIRE(print.objects().size() == 2);
    CHECK(print.objects()[0]->printing_region(0).config() == print.objects()[1]->printing_region(0).config());
}


TEST_CASE("Each object brim honors its local flow override", "[Flow][Regression]")
{
    const size_t overridden_object = GENERATE(size_t(0), size_t(1));
    auto config = multifilament_config(1, {
        {"brim_type", "outer_only"}, {"brim_width", "2"}, {"brim_object_gap", "0.1"},
        {"brim_flow_ratio", "1"}, {"skirt_loops", "0"}, {"combine_brims", "0"},
        {"set_other_flow_ratios", "0"}, {"use_relative_e_distances", "1"},
        {"enable_arc_fitting", "0"}, {"seam_slope_type", "none"}, {"gcode_comments", "1"}
    });
    auto brims = [&](bool local_override) {
        Print print;
        Model model;
        std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides(2);
        if (local_override)
            overrides[overridden_object] = {{"brim_flow_ratio", "1.2"}};
        init_print(std::vector<TriangleMesh>{cube(4), cube(4)}, print, model, config, &overrides, false);
        model.objects[0]->instances.front()->set_offset(Vec3d(50., 50., 0.));
        model.objects[1]->instances.front()->set_offset(Vec3d(100., 50., 0.));
        print.apply(model, config);
        return extrusion_with_comment(gcode(print), config, "brim");
    };
    const double baseline = brims(false);
    REQUIRE(baseline > 0.);
    config.set_deserialize_strict("filament_brim_flow_ratio", "0.8");
    CHECK_THAT(brims(false) / baseline, Catch::Matchers::WithinRel(0.8, 0.001));
    CHECK_THAT(brims(true) / baseline, Catch::Matchers::WithinRel((0.8 + 1.2) / 2., 0.001));
}
