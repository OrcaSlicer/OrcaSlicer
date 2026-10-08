#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <catch2/catch_message.hpp>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Point.hpp"
#include <cmath>
#include <cstddef>
#include <fstream>
#include <ios>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace Slic3r;

TEST_CASE("Klipper object labels name each copy without the characters Klipper cannot parse", "[GCode]")
{
    const auto [name, label] = GENERATE(table<std::string, std::string>({
        {"my part (2)", "my_part_2"},
        {"(cube)", "cube"},
    }));
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"gcode_flavor", "klipper"}, {"exclude_object", "1"}});
    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{Test::cube(20.)}, print, model, config, nullptr, false, 2);
    model.objects.front()->name = name;
    arrange_objects(model, BoundingBox{Point::new_scale(0., 0.), Point::new_scale(500., 500.)},
                    ArrangeParams{scaled(min_object_distance(config))});
    print.apply(model, config);

    const std::string gcode = Test::gcode(print);
    for (const char *copy : {"0", "1"}) {
        const std::string instance_label = label + "_id_0_copy_" + copy;
        INFO(instance_label);
        CHECK(gcode.find("EXCLUDE_OBJECT_DEFINE NAME=" + instance_label + " ") != std::string::npos);
        CHECK(gcode.find("EXCLUDE_OBJECT_START NAME=" + instance_label + "\n") != std::string::npos);
    }
}

// Firmwares read the compatibility header lines (Anycubic's GoKlipper heats the bed with
// first_layer_bed_temperature), so they must describe the filament the plate prints, not filament 1.
TEST_CASE("The G-code header names the first layer temperatures of the filament the plate prints", "[GCode]")
{
    const std::string formula = GENERATE("by_first_filament", "by_highest_temp");
    DYNAMIC_SECTION("bed_temperature_formula " << formula) {
        // Filament 1: bed 60, nozzle 200. Filament 2: bed 100, nozzle 250. Everything prints with filament 2.
        DynamicPrintConfig config = Test::multifilament_config(2, {
            {"bed_temperature_formula", formula},
            // Authored 1..n as PresetBundle writes it; the all-1s default collapses every per-filament value
            // to filament 1.
            {"filament_self_index", "1,2"},
            {"filament_extruder_variant", "Direct Drive Standard;Direct Drive Standard"},
            {"enable_prime_tower", 0},
            {"nozzle_temperature_initial_layer", "200,250"},
            {"nozzle_temperature", "200,250"},
            {"supertack_plate_temp_initial_layer", "60,100"},
            {"cool_plate_temp_initial_layer", "60,100"},
            {"textured_cool_plate_temp_initial_layer", "60,100"},
            {"eng_plate_temp_initial_layer", "60,100"},
            {"hot_plate_temp_initial_layer", "60,100"},
            {"textured_plate_temp_initial_layer", "60,100"},
            {"sparse_infill_filament_id", 2},
            {"internal_solid_filament_id", 2},
            {"top_surface_filament_id", 2},
            {"bottom_surface_filament_id", 2},
            {"outer_wall_filament_id", 2},
            {"inner_wall_filament_id", 2},
            {"skirt_loops", 0},
            {"brim_type", "no_brim"},
        });
        const std::string gcode = Test::slice({Test::cube(20)}, config);

        const auto header_value = [&gcode](const std::string& key) {
            const std::string token = "; " + key + " = ";
            const size_t pos = gcode.find(token);
            REQUIRE(pos != std::string::npos);
            return std::stoi(gcode.substr(pos + token.size()));
        };
        CHECK(header_value("first_layer_bed_temperature") == 100);
        CHECK(header_value("first_layer_temperature") == 250);
    }
}

// Printing by object reassigns the initial filament to each object's first filament while it emits the objects, so
// the header must still name the filament the print starts with: the firmware heats the bed for the first object.
TEST_CASE("The G-code header names the first object's filament when printing by object", "[GCode]")
{
    // Filament 1: bed 60, nozzle 200. Filament 2: bed 100, nozzle 250. Object 1 prints with filament 2, object 2
    // with filament 1.
    DynamicPrintConfig config = Test::multifilament_config(2, {
        {"print_sequence", "by object"},
        {"bed_temperature_formula", "by_first_filament"},
        {"filament_self_index", "1,2"},
        {"filament_extruder_variant", "Direct Drive Standard;Direct Drive Standard"},
        {"enable_prime_tower", 0},
        {"nozzle_temperature_initial_layer", "200,250"},
        {"nozzle_temperature", "200,250"},
        {"supertack_plate_temp_initial_layer", "60,100"},
        {"cool_plate_temp_initial_layer", "60,100"},
        {"textured_cool_plate_temp_initial_layer", "60,100"},
        {"eng_plate_temp_initial_layer", "60,100"},
        {"hot_plate_temp_initial_layer", "60,100"},
        {"textured_plate_temp_initial_layer", "60,100"},
        {"skirt_loops", 0},
        {"brim_type", "no_brim"},
    });
    const std::string gcode = Test::slice_with_object_overrides({Test::cube(20), Test::cube(20)}, config,
                                                                {{{"extruder", "2"}}, {{"extruder", "1"}}});

    // Filament 2 starts and filament 1 follows, so the last object's filament differs from the first one's.
    const size_t first_t1 = gcode.find("\nT1");
    const size_t first_t0 = gcode.find("\nT0");
    REQUIRE(first_t1 != std::string::npos);
    REQUIRE(first_t0 != std::string::npos);
    REQUIRE(first_t1 < first_t0);

    const auto header_value = [&gcode](const std::string& key) {
        const std::string token = "; " + key + " = ";
        const size_t pos = gcode.find(token);
        REQUIRE(pos != std::string::npos);
        return std::stoi(gcode.substr(pos + token.size()));
    };
    CHECK(header_value("first_layer_bed_temperature") == 100);
    CHECK(header_value("first_layer_temperature") == 250);
}

TEST_CASE("Object label comments number each object and copy with or without exclude object", "[GCode]")
{
    // repetier has no exclude-object commands, so set_object_info() writes nothing for it.
    // BBL printers write M624/M625 skip blocks whatever exclude_object is set to.
    const auto [flavor, exclude_object, bbl] = GENERATE(table<std::string, std::string, bool>({
        {"marlin2", "0", false},
        {"klipper", "0", false},
        {"klipper", "1", false},
        {"repetier", "1", false},
        {"marlin2", "0", true},
    }));
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"gcode_flavor", flavor}, {"exclude_object", exclude_object}, {"gcode_label_objects", "1"}});
    Print print;
    print.is_BBL_printer() = bbl;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{Test::cube(20.), Test::cube(20.)}, print, model, config, nullptr, false, 2);
    arrange_objects(model, BoundingBox{Point::new_scale(0., 0.), Point::new_scale(500., 500.)},
                    ArrangeParams{scaled(min_object_distance(config))});
    print.apply(model, config);

    const std::string gcode = Test::gcode(print);
    INFO(flavor << ", exclude_object " << exclude_object << ", BBL " << bbl);
    for (const char *label : {"id:0 copy 0", "id:0 copy 1", "id:1 copy 0", "id:1 copy 1"}) {
        INFO(label);
        CHECK(gcode.find("; printing object object.stl " + std::string(label) + "\n") != std::string::npos);
    }
    if (bbl)
        for (const PrintObject *object : print.objects())
            for (const PrintInstance &instance : object->instances()) {
                const std::string label_id = std::to_string(instance.model_instance->get_labeled_id());
                INFO("label id " << label_id);
                CHECK(gcode.find("; start printing object, unique label id: " + label_id + "\nM624 ") != std::string::npos);
                CHECK(gcode.find("; stop printing object, unique label id: " + label_id + "\nM625\n") != std::string::npos);
            }
}

TEST_CASE("An exported G-code's settings load back as the settings it was sliced with", "[GCode]")
{
    // Two filaments, so the config block carries the flush matrix already multiplied by flush_multiplier.
    const DynamicPrintConfig config = Test::multifilament_config(2, {
        { "flush_multiplier", "0.5" },
        { "printable_area", "0x0,250x0,250x210,0x210" },
        { "printable_height", "180" },
    });
    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{ Test::cube(20.) }, print, model, config, nullptr, false);
    ScopedTemporaryFile file(".gcode");
    {
        std::ofstream os(file.string(), std::ios::binary);
        os << Test::gcode(print);
        REQUIRE(os.good());
    }

    PresetBundle bundle;
    // The config block leaves prime_volume_mode out, so the receiver's choice has to survive the load.
    bundle.project_config.option<ConfigOptionEnum<PrimeVolumeMode>>("prime_volume_mode")->value = PrimeVolumeMode::pvmFast;
    ConfigSubstitutions substitutions;
    bundle.load_config_model(file.string(), bundle.config_from_gcode(file.string(), ForwardCompatibilitySubstitutionRule::Enable, substitutions));

    const DynamicPrintConfig &printer = bundle.printers.get_edited_preset().config;
    CHECK(printer.opt_serialize("printable_area") == config.opt_serialize("printable_area"));
    CHECK_THAT(printer.opt_float("printable_height"), Catch::Matchers::WithinAbs(180., 1e-6));
    // The purge between the two filaments is what the slice used: 280 at a 0.5 multiplier.
    const std::vector<double> &matrix     = bundle.project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
    const std::vector<double> &multiplier = bundle.project_config.option<ConfigOptionFloats>("flush_multiplier")->values;
    REQUIRE(matrix.size() == 4);
    REQUIRE(multiplier.size() == 1);
    CHECK_THAT(matrix[1] * multiplier[0], Catch::Matchers::WithinAbs(140., 1e-6));
    CHECK(bundle.project_config.option<ConfigOptionEnum<PrimeVolumeMode>>("prime_volume_mode")->value == PrimeVolumeMode::pvmFast);
}

namespace {

constexpr double filament_diameter = 1.75;
constexpr double max_volumetric_speed = 20.;
constexpr double layer_height = 0.2;

// A 20mm cube sliced at 0.2mm layers with volumetric speeds `enabled` and `extra` applied last.
std::string volumetric_speed_gcode(bool enabled, std::initializer_list<ConfigBase::SetDeserializeItem> extra)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_volumetric_speeds", enabled ? "1" : "0"},
        {"filament_diameter", filament_diameter},
        {"filament_max_volumetric_speed", max_volumetric_speed},
        {"layer_height", layer_height},
        {"initial_layer_print_height", layer_height},
        {"slow_down_for_layer_cooling", "0"},
        {"slow_down_layers", "0"},
        {"enable_arc_fitting", "0"},
    });
    config.set_deserialize_strict(extra);
    return Test::slice({Test::cube(20.)}, config);
}

// For each extrusion move longer than 1mm of `role` above the first layer: its feed rate (mm/s) and extruded flow (mm³/s).
std::vector<std::pair<double, double>> role_speeds_and_flows(const std::string &gcode, std::string_view role)
{
    const double filament_area = 0.25 * M_PI * filament_diameter * filament_diameter;
    std::vector<std::pair<double, double>> moves;
    bool in_role = false;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view comment = line.comment();
        if (comment.find("TYPE:") != std::string_view::npos)
            in_role = comment.substr(comment.find("TYPE:") + 5) == role;
        if (in_role && line.extruding(self) && line.dist_XY(self) > 1. && line.new_Z(self) > 1.5 * layer_height) {
            const double speed = line.new_F(self) / Test::MM_PER_MIN;
            moves.emplace_back(speed, line.dist_E(self) * filament_area / line.dist_XY(self) * speed);
        }
    });
    return moves;
}

} // namespace

TEST_CASE("Volumetric speeds print each feature at its configured flow", "[GCode]")
{
    const auto [key, role] = GENERATE(table<std::string, std::string>({
        {"outer_wall_volumetric_flow", "Outer wall"},
        {"inner_wall_volumetric_flow", "Inner wall"},
        {"sparse_infill_volumetric_flow", "Sparse infill"},
        {"internal_solid_infill_volumetric_flow", "Internal solid infill"},
        {"top_surface_volumetric_flow", "Top surface"},
    }));
    INFO(key);
    const double flow = 6.;
    const auto moves = role_speeds_and_flows(volumetric_speed_gcode(true, {{key, flow}}), role);

    REQUIRE_FALSE(moves.empty());
    for (const auto &[speed, extruded_flow] : moves)
        CHECK_THAT(extruded_flow, Catch::Matchers::WithinRel(flow, 0.01));
}

TEST_CASE("A volumetric speed in percent is a share of the filament's max volumetric speed, which caps it", "[GCode]")
{
    const auto [value, expected_flow] = GENERATE(table<std::string, double>({
        {"50%", 0.5 * max_volumetric_speed},
        {"300%", max_volumetric_speed},
        {"40", max_volumetric_speed},
    }));
    INFO(value);
    const auto moves = role_speeds_and_flows(volumetric_speed_gcode(true, {{"outer_wall_volumetric_flow", value}}), "Outer wall");

    REQUIRE_FALSE(moves.empty());
    for (const auto &[speed, extruded_flow] : moves)
        CHECK_THAT(extruded_flow, Catch::Matchers::WithinRel(expected_flow, 0.01));
}

TEST_CASE("Volumetric speeds are ignored while disabled", "[GCode]")
{
    const double outer_wall_speed = 50.;
    const auto moves = role_speeds_and_flows(
        volumetric_speed_gcode(false, {{"outer_wall_speed", outer_wall_speed}, {"outer_wall_volumetric_flow", "2"}}), "Outer wall");

    REQUIRE_FALSE(moves.empty());
    for (const auto &[speed, extruded_flow] : moves)
        CHECK_THAT(speed, Catch::Matchers::WithinRel(outer_wall_speed, 0.001));
}
