#include <algorithm>
#include <cstddef>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"
#include "libslic3r/AdaptiveLayerMerge.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <cmath>
#include <functional>
#include <limits>
#include <vector>
#include <string>
#include <utility>
#include "libslic3r/Flow.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include <set>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

namespace {
void collect_heights(const ExtrusionEntityCollection &collection, std::vector<double> &heights)
{
    for (const ExtrusionEntity *entity : collection.entities) {
        if (const auto *nested = dynamic_cast<const ExtrusionEntityCollection *>(entity))
            collect_heights(*nested, heights);
        else if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity))
            heights.push_back(path->height);
        else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity))
            for (const auto &path : loop->paths)
                heights.push_back(path.height);
        else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity))
            for (const auto &path : multi->paths)
                heights.push_back(path.height);
    }
}

DynamicPrintConfig layer_config(unsigned int filaments = 2)
{
    // Construct at the final count: growing set_num_filaments() later leaves variant
    // metadata and the per-nozzle flush blocks at their original size.
    auto config = multifilament_config(filaments);
    config.set_deserialize_strict({
        {"nozzle_diameter", "0.4,0.4"}, {"extruder_layer_height", "0.1,0.2"},
        {"min_layer_height", "0.05,0.05"}, {"max_layer_height", "0.3,0.3"},
        {"layer_height", "0.2"}, {"initial_layer_print_height", "0.1"},
        {"filament_map", "1,2"}, {"enable_prime_tower", "0"},
        {"brim_type", "no_brim"}, {"skirt_loops", "0"},
        {"thick_bridges", "0"}, {"thick_internal_bridges", "0"},
        {"wall_loops", "2"}, {"sparse_infill_density", "20%"},
        {"top_shell_layers", "2"}, {"bottom_shell_layers", "2"},
        {"top_shell_thickness", "0"}, {"bottom_shell_thickness", "0"}
    });
    config.set_num_extruders(filaments);
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode")->value = fmmManual;
    size_flush_to_nozzles(config);
    return config;
}
}

TEST_CASE("Equal and mixed nozzle diameters print their own layer heights", "[ExtruderLayerHeight]")
{
    const double scale = GENERATE(1., 1.5);
    auto config = layer_config();
    if (scale == 1.5)
        config.set_deserialize_strict({{"extruder_layer_height", "0.15,0.3"}, {"initial_layer_print_height", "0.15"}});
    const bool mixed_nozzles = GENERATE(false, true);
    if (mixed_nozzles) {
        // Exercise the reported 0.2 mm nozzle / 0.15 mm layer combination without
        // changing the shipped printer profile's 0.14 mm maximum.
        config.set_deserialize_strict({{"nozzle_diameter", "0.2,0.6"}, {"max_layer_height", "0.15,0.42"}});
        for (const char *key : {"line_width", "outer_wall_line_width", "inner_wall_line_width",
                               "sparse_infill_line_width", "internal_solid_infill_line_width",
                               "top_surface_line_width", "initial_layer_line_width"})
            config.set_deserialize_strict(key, "100%");
    }
    CAPTURE(scale, mixed_nozzles);
    config.set_deserialize_strict("wall_generator", GENERATE("classic", "arachne"));
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        {{"extruder", "1"}}, {{"extruder", "2"}}
    };
    init_print({make_cube(8., 8., 1.1 * scale), make_cube(8., 8., 1.1 * scale)}, print, model, config, &overrides);
    print.set_status_silent();
    print.process();
    REQUIRE(print.objects().size() == 2);
    for (size_t object_id = 0; object_id < 2; ++object_id) {
        const double expected = (object_id == 0 ? 0.1 : 0.2) * scale;
        size_t extruding_layers = 0;
        for (const Layer *layer : print.objects()[object_id]->layers()) {
            std::vector<double> wall_heights;
            std::vector<double> fill_heights;
            for (const LayerRegion *region : layer->regions()) {
                collect_heights(region->perimeters, wall_heights);
                collect_heights(region->fills, fill_heights);
            }
            CAPTURE(object_id, layer->id(), layer->print_z, wall_heights, fill_heights);
            for (const LayerRegion *region : layer->regions()) {
                INFO("wall=" << region->region().extruder(frExternalPerimeter) << " sparse=" << region->region().extruder(frInfill) << " solid=" << region->region().extruder(frSolidInfill));
            }
            if (wall_heights.empty()) {
                REQUIRE(fill_heights.empty());
                continue;
            }
            ++extruding_layers;
            for (double height : wall_heights)
                REQUIRE_THAT(height, WithinAbs(layer->id() == 0 ? 0.1 * scale : expected, 1e-5));
            for (double height : fill_heights)
                REQUIRE_THAT(height, WithinAbs(layer->id() == 0 ? 0.1 * scale : expected, 1e-5));
        }
        REQUIRE(extruding_layers == (object_id == 0 ? 11 : 6));
    }
}

TEST_CASE("Extruder layer heights follow manual filament assignments", "[ExtruderLayerHeight]")
{
    PrintConfig config;
    config.nozzle_diameter.values = {0.4, 0.6};
    config.extruder_layer_height.values = {0.12, 0.3};
    config.filament_map_mode.value = fmmManual;
    config.filament_map.values = {2, 1, 2};
    REQUIRE_THAT(extruder_layer_height_for_filament(config, 1, 0.2), WithinAbs(0.3, 1e-6));
    REQUIRE_THAT(extruder_layer_height_for_filament(config, 2, 0.2), WithinAbs(0.12, 1e-6));
    REQUIRE_THAT(extruder_layer_height_for_filament(config, 3, 0.2), WithinAbs(0.3, 1e-6));
    REQUIRE_THAT(extruder_layer_height_grid(config, 0.2, {0, 1, 2}), WithinAbs(0.06, 1e-6));
    config.extruder_layer_height.values = {0.};
    REQUIRE_FALSE(has_extruder_layer_heights(config));
    REQUIRE_THAT(extruder_layer_height_for_filament(config, 1, 0.2), WithinAbs(0.2, 1e-6));
}

TEST_CASE("Adjoining materials retain separate layer schedules in one object", "[ExtruderLayerHeight]")
{
    auto config = layer_config();
    Print print;
    Model model;
    init_print({make_cube(8., 8., 1.1)}, print, model, config);
    ModelObject *object = model.objects.front();
    object->volumes.front()->config.set_key_value("extruder", new ConfigOptionInt(1));
    TriangleMesh second = make_cube(8., 8., 1.1);
    second.translate(8., 0., 0.);
    ModelVolume *volume = object->add_volume(std::move(second));
    volume->config.set_key_value("extruder", new ConfigOptionInt(2));
    print.apply(model, config);
    print.set_status_silent();
    print.process();
    REQUIRE(print.objects().size() == 1);
    size_t printed[2] = {0, 0};
    for (const Layer *layer : print.objects().front()->layers()) {
        for (const LayerRegion *region : layer->regions()) {
            const unsigned int filament = region->region().extruder(frExternalPerimeter);
            REQUIRE(filament >= 1);
            REQUIRE(filament <= 2);
            std::vector<double> heights;
            collect_heights(region->perimeters, heights);
            if (heights.empty())
                continue;
            ++printed[filament - 1];
            for (double height : heights)
                REQUIRE_THAT(height, WithinAbs(layer->id() == 0 || filament == 1 ? 0.1 : 0.2, 1e-5));
        }
    }
    REQUIRE(printed[0] == 11);
    REQUIRE(printed[1] == 6);
    const std::string output = gcode(print);
    REQUIRE(output.find("T0") != std::string::npos);
    REQUIRE(output.find("T1") != std::string::npos);
}

TEST_CASE("Layer heights with a nonintegral ratio share an exact slicing grid", "[ExtruderLayerHeight]")
{
    auto config = layer_config();
    config.set_deserialize_strict("extruder_layer_height", "0.12,0.3");
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        {{"extruder", "1"}}, {{"extruder", "2"}}
    };
    init_print({make_cube(8., 8., 2.5), make_cube(8., 8., 2.5)}, print, model, config, &overrides);
    print.set_status_silent();
    print.process();
    for (size_t object_id = 0; object_id < 2; ++object_id) {
        const double expected = object_id == 0 ? 0.12 : 0.3;
        double previous_z = 0.;
        size_t count = 0;
        for (const Layer *layer : print.objects()[object_id]->layers()) {
            std::vector<double> heights;
            for (const LayerRegion *region : layer->regions())
                collect_heights(region->perimeters, heights);
            if (heights.empty())
                continue;
            ++count;
            REQUIRE_THAT(layer->print_z - previous_z, WithinAbs(count == 1 ? 0.1 : expected, 1e-5));
            for (double height : heights)
                REQUIRE_THAT(height, WithinAbs(count == 1 ? 0.1 : expected, 1e-5));
            previous_z = layer->print_z;
        }
        REQUIRE(count == (object_id == 0 ? 21 : 9));
        REQUIRE_THAT(previous_z, WithinAbs(2.5, 1e-5));
    }
}

TEST_CASE("Mixed nozzle sizes retain the common first layer", "[ExtruderLayerHeight]")
{
    auto config = layer_config();
    config.set_deserialize_strict({{"nozzle_diameter", "0.2,0.8"},
        {"extruder_layer_height", "0.1,0.4"}, {"min_layer_height", "0.04,0.16"},
        {"max_layer_height", "0.14,0.56"}});
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        {{"extruder", "1"}}, {{"extruder", "2"}}
    };
    init_print({make_cube(8., 8., 2.1), make_cube(8., 8., 2.1)}, print, model, config, &overrides);
    print.set_status_silent();
    print.process();
    size_t coarse_layers = 0;
    for (const Layer *layer : print.objects()[1]->layers()) {
        std::vector<double> heights;
        for (const LayerRegion *region : layer->regions())
            collect_heights(region->perimeters, heights);
        if (heights.empty())
            continue;
        ++coarse_layers;
        for (double height : heights)
            REQUIRE_THAT(height, WithinAbs(coarse_layers == 1 ? 0.1 : 0.4, 1e-5));
    }
    REQUIRE(coarse_layers == 6);
    REQUIRE(print.objects()[1]->layers().front()->has_extrusions());
}

TEST_CASE("Supports use the selected extruder's process height", "[ExtruderLayerHeight]")
{
    auto config = layer_config();
    const auto support_type = GENERATE("normal(auto)", "tree(auto)");
    CAPTURE(support_type);
    config.set_deserialize_strict({{"enable_support", "1"}, {"support_filament", "2"},
        {"support_interface_filament", "2"}, {"support_type", support_type}, {"support_style", "tree_slim"},
        {"support_top_z_distance", "0.2"}, {"support_bottom_z_distance", "0.2"},
        {"support_interface_top_layers", "2"}, {"independent_support_layer_height", "0"}});
    TriangleMesh overhang = make_cube(6., 6., 4.1);
    TriangleMesh roof = make_cube(20., 6., 1.);
    roof.translate(-7., 0., 4.);
    overhang.merge(roof);
    Print print;
    Model model;
    init_print({overhang}, print, model, config);
    print.set_status_silent();
    print.process();
    bool has_requested_height = false;
    REQUIRE_FALSE(print.objects().front()->support_layers().empty());
    for (const SupportLayer *layer : print.objects().front()->support_layers()) {
        if (!layer->has_extrusions())
            continue;
        REQUIRE(layer->height <= 0.2 + 1e-5);
        has_requested_height |= std::abs(layer->height - 0.2) < 1e-5;
    }
    REQUIRE(has_requested_height);
}

TEST_CASE("Prime tower columns use their tool heights on a fine object grid", "[ExtruderLayerHeight][WipeTower]")
{
    const bool mixed_nozzles = GENERATE(false, true);
    const bool ramming = GENERATE(false, true);
    const auto wall = GENERATE("rectangle", "rib");
    CAPTURE(mixed_nozzles, ramming, wall);
    auto config = layer_config();
    config.set_deserialize_strict({{"enable_prime_tower", "1"}, {"wipe_tower_type", "type2"},
        {"single_extruder_multi_material", "0"}, {"prime_tower_width", "30"},
        {"nozzle_diameter", mixed_nozzles ? "0.2,0.6" : "0.4,0.4"},
        {"extruder_layer_height", "0.05,0.3"}, {"max_layer_height", "0.14,0.42"},
        {"filament_multitool_ramming", ramming ? "1,1" : "0,0"},
        {"wipe_tower_filament", "2"}, {"wipe_tower_x", "60"}, {"wipe_tower_y", "60"},
        {"wipe_tower_wall_type", wall}, {"wipe_tower_rotation_angle", "30"},
        {"change_filament_gcode", "T[next_extruder]\n"}, {"gcode_comments", "1"}});
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        {{"extruder", "1"}}, {{"extruder", "2"}}
    };
    init_print({make_cube(8., 8., 1.3), make_cube(8., 8., 1.3)}, print, model, config, &overrides);
    print.set_status_silent();
    GCodeProcessorResult processed;
    const std::string output = gcode(print, &processed);
    REQUIRE(output.find("T0") != std::string::npos);
    REQUIRE(output.find("T1") != std::string::npos);
    REQUIRE(output.find("Clear independent tower columns") != std::string::npos);
    REQUIRE(print.has_wipe_tower());
    std::set<int> coarse_tops;
    size_t coarse_visits = 0;
    for (const auto &layer : print.wipe_tower_data().tool_changes)
        for (const auto &visit : layer) {
            if (visit.combined_away)
                continue;
            REQUIRE_FALSE(visit.segments.empty());
            for (const auto &part : visit.segments) {
                REQUIRE(part.independent_column);
                const double requested = part.new_tool == 0 ? 0.05 : 0.3;
                REQUIRE_THAT(part.layer_height, WithinAbs(part.print_z < 0.10001 ? 0.1 : requested, 1e-5));
                if (part.new_tool == 1 && part.print_z > 0.10001) {
                    ++coarse_visits;
                    coarse_tops.insert(int(std::lround(part.print_z * 1000000)));
                }
            }
        }
    REQUIRE(coarse_tops.size() >= 3);
    // Fine-grid revisits must share the reserved purge space in a thick layer,
    // rather than depositing another thick layer on the same footprint.
    REQUIRE(coarse_visits > coarse_tops.size());
    size_t coarse_moves = 0;
    for (const auto &move : processed.moves)
        if (move.type == EMoveType::Extrude && move.extrusion_role == erWipeTower && move.position.z() > 0.10001) {
            const double requested = move.extruder_id == 0 ? 0.05 : 0.3;
            CHECK_THAT(move.height, WithinAbs(requested, 1e-5));
            const double band = (move.position.z() - 0.1) / requested;
            CHECK_THAT(band, WithinAbs(std::round(band), 1e-4));
            coarse_moves += move.extruder_id == 1;
        }
    REQUIRE(coarse_moves > 0);
}

TEST_CASE("Three tool-height tower columns fit in the bed corner", "[ExtruderLayerHeight][WipeTower]")
{
    auto config = layer_config(3);
    config.set_deserialize_strict({
        {"enable_prime_tower", "1"}, {"wipe_tower_type", "type2"}, {"single_extruder_multi_material", "0"},
        {"nozzle_diameter", "0.2,0.2,0.6"}, {"extruder_layer_height", "0.05,0.1,0.3"},
        {"max_layer_height", "0.14,0.14,0.42"}, {"prime_tower_width", "30"},
        {"filament_multitool_ramming", "1,1,1"}, {"filament_multitool_ramming_volume", "5,5,5"},
        {"wipe_tower_wall_type", "rib"}, {"wipe_tower_x", "185"}, {"wipe_tower_y", "181"},
        {"printable_area", "0x0,270x0,270x270,0x270"}});
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        {{"extruder", "1"}}, {{"extruder", "2"}}, {{"extruder", "3"}}
    };
    init_print({make_cube(8., 8., 1.3), make_cube(8., 8., 1.3), make_cube(8., 8., 1.3)}, print, model, config, &overrides);
    print.set_status_silent();
    REQUIRE_NOTHROW(print.process());
    REQUIRE(print.has_wipe_tower());
    std::set<unsigned int> tools;
    for (const auto &layer : print.wipe_tower_data().tool_changes)
        for (const auto &visit : layer)
            for (const auto &part : visit.segments)
                tools.insert(part.new_tool);
    REQUIRE(tools.size() == 3);
}

TEST_CASE("Unused configured tool heights do not activate tower columns", "[ExtruderLayerHeight][WipeTower]")
{
    auto config = layer_config(3);
    config.set_deserialize_strict({{"enable_prime_tower", "1"}, {"wipe_tower_type", "type2"},
        {"single_extruder_multi_material", "0"}, {"prime_tower_width", "30"},
        {"nozzle_diameter", "0.4,0.4,0.8"}, {"extruder_layer_height", "0,0,0.3"},
        {"max_layer_height", "0.3,0.3,0.6"}, {"filament_map", "1,2,3"},
        {"wipe_tower_x", "60"}, {"wipe_tower_y", "60"},
        {"change_filament_gcode", "T[next_extruder]\n"}});
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        {{"extruder", "1"}}, {{"extruder", "2"}}
    };
    init_print({make_cube(8., 8., 1.1), make_cube(8., 8., 1.1)}, print, model, config, &overrides);
    print.set_status_silent();
    REQUIRE_FALSE(gcode(print).empty());
    for (const auto &layer : print.wipe_tower_data().tool_changes)
        for (const auto &change : layer)
            REQUIRE(change.segments.empty());
}

TEST_CASE("Unsupported layer schedules are rejected before slicing", "[ExtruderLayerHeight]")
{
    auto config = layer_config();
    config.set_deserialize_strict("extruder_layer_height", "0.101,0.2");
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        {{"extruder", "1"}}, {{"extruder", "2"}}
    };
    init_print({make_cube(8., 8., 1.), make_cube(8., 8., 1.)}, print, model, config, &overrides);
    const auto error = print.validate();
    REQUIRE(error.opt_key == "extruder_layer_height");
    REQUIRE(error.string.find("0.01") != std::string::npos);
}

TEST_CASE("Organic support reports its incompatible layer schedule", "[ExtruderLayerHeight]")
{
    auto config = layer_config();
    config.set_deserialize_strict({{"enable_support", "1"}, {"support_filament", "2"},
        {"support_interface_filament", "2"}, {"support_type", "tree(auto)"}, {"support_style", "organic"}});
    Print print;
    Model model;
    init_print({make_cube(8., 8., 1.)}, print, model, config);
    REQUIRE(print.validate().opt_key == "support_style");
}

TEST_CASE("Unused nozzle heights do not constrain a single used tool", "[ExtruderLayerHeight]")
{
    const bool explicit_height = GENERATE(false, true);
    const int filament = GENERATE(1, 2);
    auto config = layer_config(4);
    config.set_deserialize_strict({{"nozzle_diameter", "0.2,0.4,0.6,0.8"}, {"filament_map", "2,2,3,4"},
        {"min_layer_height", "0.05,0.07,0.1,0.15"}, {"max_layer_height", "0.15,0.3,0.45,0.6"},
        {"extruder_layer_height", explicit_height ? "0.101,0.2,0.233,0.377" : "0.101,0,0.233,0.377"}});
    config.set_deserialize_strict("layer_change_gcode", "G92 E0");
    REQUIRE(config.option<ConfigOptionStrings>("filament_type")->size() == 4);
    REQUIRE(config.option<ConfigOptionFloats>("flush_volumes_matrix")->size() == 4 * 4 * 4);
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{{"extruder", std::to_string(filament)}}};
    init_print({make_cube(8., 8., 1.1)}, print, model, config, &overrides);
    const auto error = print.validate();
    CAPTURE(explicit_height, filament, error.opt_key);
    INFO(error.string);
    REQUIRE(error.string.empty());
    REQUIRE_THAT(print.objects().front()->slicing_parameters().layer_height, WithinAbs(0.2, 1e-6));
    REQUIRE(has_extruder_layer_heights(*print.objects().front()) == explicit_height);
    print.set_status_silent();
    print.process();
    REQUIRE(print.objects().front()->layer_count() == 6);
}

TEST_CASE("Changes in used tools invalidate the shared slicing grid", "[ExtruderLayerHeight]")
{
    auto config = layer_config();
    config.set_deserialize_strict("extruder_layer_height", GENERATE("0.1,0.2", "0.1,0"));
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        {{"extruder", "1"}}, {{"extruder", "2"}}
    };
    init_print({make_cube(8., 8., 1.1), make_cube(8., 8., 1.1)}, print, model, config, &overrides);
    print.set_status_silent();
    print.process();
    REQUIRE_THAT(print.objects()[1]->slicing_parameters().layer_height, WithinAbs(0.1, 1e-6));
    model.objects[0]->config.set_key_value("extruder", new ConfigOptionInt(2));
    print.apply(model, config);
    REQUIRE_THAT(print.objects()[1]->slicing_parameters().layer_height, WithinAbs(0.2, 1e-6));
    print.process();
    REQUIRE(print.objects()[1]->layer_count() == 6);
}

TEST_CASE("Support bodies and interfaces use independent tool heights", "[ExtruderLayerHeight][SupportMaterial]")
{
    const auto style = GENERATE("normal", "snug", "tree_slim", "tree_strong", "tree_hybrid");
    const bool zero_gap = GENERATE(false, true);
    const bool normal = std::string(style) == "normal" || std::string(style) == "snug";
    const bool coarse_interface = GENERATE(false, true);
    const bool mapped = GENERATE(false, true);
    const double fine_height = GENERATE(0.1, 0.15);
    const int interface_count = GENERATE(1, 3);
    const bool staggered = GENERATE(false, true);
    CAPTURE(style, zero_gap, coarse_interface, mapped, fine_height, interface_count, staggered);
    auto config = layer_config(3);
    config.set_deserialize_strict({
        {"nozzle_diameter", coarse_interface ? "0.2,0.6,0.2" : "0.6,0.2,0.2"},
        {"extruder_layer_height", coarse_interface ? "0.1,0.3,0.1" : "0.3,0.1,0.1"},
        {"min_layer_height", "0.04,0.04,0.04"}, {"max_layer_height", "0.42,0.42,0.16"},
        {"filament_map", "1,2,3"}, {"enable_support", "1"}, {"support_filament", "1"},
        {"support_interface_filament", "2"}, {"support_type", normal ? "normal(auto)" : "tree(auto)"},
        {"support_style", std::string(style) == "normal" ? "grid" : style},
        {"support_top_z_distance", zero_gap ? "0" : "0.2"}, {"support_bottom_z_distance", "0.2"},
        {"support_interface_top_layers", "3"}, {"support_interface_bottom_layers", "3"},
        {"independent_support_layer_height", "0"}, {"thick_bridges", "0"},
        {"layer_change_gcode", "G92 E0\n"}});
    config.set_deserialize_strict("support_interface_top_layers", std::to_string(interface_count));
    auto &heights = config.option<ConfigOptionFloats>("extruder_layer_height")->values;
    heights[coarse_interface ? 0 : 1] = fine_height;
    if (mapped) {
        std::swap(heights[0], heights[1]);
        auto &nozzles = config.option<ConfigOptionFloats>("nozzle_diameter")->values;
        std::swap(nozzles[0], nozzles[1]);
        config.set_deserialize_strict("filament_map", "2,1,3");
    }
    REQUIRE(config.option<ConfigOptionStrings>("filament_type")->size() == 3);
    REQUIRE(config.option<ConfigOptionFloats>("flush_volumes_matrix")->size() == 3 * 3 * 3);
    TriangleMesh overhang = make_cube(6., 6., 4.1);
    TriangleMesh roof = make_cube(20., 6., 1.);
    roof.translate(-7., 0., 4.);
    overhang.merge(roof);
    if (staggered) {
        TriangleMesh second_pillar = make_cube(6., 6., 4.2);
        second_pillar.translate(25., 0., 0.);
        TriangleMesh second_roof = make_cube(20., 6., 1.);
        second_roof.translate(18., 0., 4.1);
        overhang.merge(second_pillar);
        overhang.merge(second_roof);
    }
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{{"extruder", "3"}}};
    Print print;
    Model model;
    init_print({overhang}, print, model, config, &overrides);
    print.set_status_silent();
    const auto error = print.validate();
    INFO(error.string);
    REQUIRE(error.string.empty());
    print.process();
    std::vector<double> base_heights, interface_heights;
    double print_z = 0.;
    double interface_bottom = std::numeric_limits<double>::max();
    double interface_top = 0.;
    std::function<void(const ExtrusionEntity &)> collect = [&](const ExtrusionEntity &entity) {
        if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
            for (const auto *child : collection->entities)
                collect(*child);
        } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
            if (path->role() == erSupportMaterial)
                base_heights.push_back(path->height);
            else if (path->role() == erSupportMaterialInterface) {
                interface_heights.push_back(path->height);
                interface_bottom = std::min(interface_bottom, print_z - path->height);
                interface_top = std::max(interface_top, print_z);
            }
        } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
            for (const auto &path : loop->paths)
                collect(path);
        } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
            for (const auto &path : multi->paths)
                collect(path);
        }
    };
    for (const auto *layer : print.objects().front()->support_layers()) {
        print_z = layer->print_z;
        if (print_z > 0.1 + 1e-5)
            collect(layer->support_fills);
    }
    const auto check = [](const std::vector<double> &heights, double requested) {
        REQUIRE_FALSE(heights.empty());
        bool has_requested = false;
        for (double height : heights) {
            CHECK(height > 0.);
            CHECK(height <= requested + 1e-5);
            has_requested |= std::abs(height - requested) < 1e-5;
        }
        CHECK(has_requested);
    };
    check(base_heights, coarse_interface ? fine_height : 0.3);
    check(interface_heights, coarse_interface ? 0.3 : fine_height);
    // More than one configured interface layer includes one adhesion layer on the base tool.
    // A second ceiling introduces extra geometry boundaries. They may subdivide the
    // deposited layers, but must not shorten the requested physical interface depth.
    const double interface_depth = (interface_count > 1 ? interface_count - 1 : 1) * (coarse_interface ? 0.3 : fine_height);
    CHECK(interface_top - interface_bottom >= interface_depth - 1e-5);
    CHECK(interface_top - interface_bottom <= interface_depth + (staggered ? 0.1 : 0.) + 1e-5);
}
