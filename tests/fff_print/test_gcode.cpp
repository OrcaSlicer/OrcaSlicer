#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <catch2/catch_message.hpp>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
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
#include <algorithm>
#include <cstddef>
#include <fstream>
#include <ios>
#include <map>
#include <sstream>
#include <string>
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

// Klipper keeps the last value each SET_PRINT_STATS_INFO parameter was given. Returns the
// "current/total" layer it holds whenever an object starts printing, per object label.
static std::map<std::string, std::vector<std::string>> print_stats_info_per_object(const std::string &gcode)
{
    std::map<std::string, std::vector<std::string>> info;
    int total = 0, current = 0;
    std::istringstream lines(gcode);
    for (std::string line; std::getline(lines, line);) {
        if (line.rfind("SET_PRINT_STATS_INFO", 0) == 0) {
            if (size_t pos = line.find("TOTAL_LAYER="); pos != std::string::npos)
                total = std::stoi(line.substr(pos + 12));
            if (size_t pos = line.find("CURRENT_LAYER="); pos != std::string::npos)
                current = std::stoi(line.substr(pos + 14));
        } else if (line.rfind("EXCLUDE_OBJECT_START NAME=", 0) == 0) {
            const std::string state = std::to_string(current) + "/" + std::to_string(total);
            std::vector<std::string> &seen = info[line.substr(26)];
            if (seen.empty() || seen.back() != state)
                seen.push_back(state);
        }
    }
    return info;
}

TEST_CASE("Sequential Klipper prints report the layer within each object", "[GCode]")
{
    // A profile reporting the whole-print layer on layer change must not override the per-object values.
    const std::string layer_change_gcode = GENERATE(std::string(), std::string("SET_PRINT_STATS_INFO CURRENT_LAYER={layer_num + 1}"));
    INFO("layer_change_gcode: " << layer_change_gcode);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"gcode_flavor", "klipper"},
        {"exclude_object", "1"},
        {"print_sequence", "by object"},
        {"layer_height", "0.25"},
        {"initial_layer_print_height", "0.25"},
        {"raft_layers", "0"},
        {"enable_support", "0"},
    });
    config.set_key_value("layer_change_gcode", new ConfigOptionString(layer_change_gcode));

    // At 0.25 mm layers the 20 mm cube has 80 layers and the 10 mm block 40.
    const auto info = print_stats_info_per_object(Test::slice({make_cube(20., 20., 20.), make_cube(20., 20., 10.)}, config));
    REQUIRE(info.size() == 2);
    std::vector<int> totals;
    for (const auto &[label, seen] : info) {
        INFO(label);
        const int total = int(seen.size());
        std::vector<std::string> expected;
        for (int layer = 1; layer <= total; ++layer)
            expected.push_back(std::to_string(layer) + "/" + std::to_string(total));
        CHECK(seen == expected);
        totals.push_back(total);
    }
    std::sort(totals.begin(), totals.end());
    CHECK(totals == std::vector<int>{40, 80});
}

TEST_CASE("Prints other than sequential Klipper prints emit no SET_PRINT_STATS_INFO", "[GCode]")
{
    const auto [print_sequence, gcode_flavor] = GENERATE(table<std::string, std::string>({
        {"by layer", "klipper"},
        {"by object", "marlin2"},
    }));
    INFO(print_sequence << ", " << gcode_flavor);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"gcode_flavor", gcode_flavor},
        {"exclude_object", "1"},
        {"print_sequence", print_sequence},
        {"machine_start_gcode", ""},
        {"layer_change_gcode", ""},
    });

    const std::string gcode = Test::slice({make_cube(20., 20., 20.), make_cube(20., 20., 10.)}, config);
    CHECK(gcode.find("SET_PRINT_STATS_INFO") == std::string::npos);
}
