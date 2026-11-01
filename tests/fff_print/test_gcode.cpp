#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_tostring.hpp>
#include <boost/filesystem/operations.hpp>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/calib.hpp"
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
#include "libslic3r/libslic3r.h"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <ios>
#include <regex>
#include <set>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace Slic3r;

TEST_CASE("Kobra 2 layer-change templates do not emit numeric commands", "[GCode][Kobra2][Regression]")
{
    const char *template_key = GENERATE("before_layer_change_gcode", "layer_change_gcode");
    const std::string profile_path = std::string(PROFILES_DIR) + "/Anycubic/machine/Anycubic Kobra 2 0.4 nozzle.json";
    // CI test artifacts may omit the shipped profiles.
    if (!boost::filesystem::exists(profile_path))
        SKIP("shipped profile not present: " << profile_path);

    DynamicPrintConfig machine;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    std::map<std::string, std::string> metadata;
    std::string reason;
    machine.load_from_json(profile_path, substitutions, false, metadata, reason);
    INFO("profile: " << profile_path << "\nload reason: " << reason);
    REQUIRE(machine.has(template_key));
    REQUIRE_FALSE(machine.opt_string(template_key).empty());

    auto config = Test::multifilament_config(1, {
        {"gcode_flavor", "marlin"},
        {"layer_height", "0.2"},
        {"first_layer_height", "0.2"},
        {"use_relative_e_distances", "0"},
        {"machine_start_gcode", ""},
        {"machine_end_gcode", ""},
        {"before_layer_change_gcode", ""},
        {"layer_change_gcode", ""},
    });
    // Exercise each shipped template separately through a real G-code export.
    config.set_key_value(template_key, machine.option(template_key)->clone());
    const std::string gcode = Test::slice({Test::cube(1.)}, config);
    INFO("template: " << template_key);
    REQUIRE(gcode.find(";LAYER_CHANGE") != std::string::npos);

    GCodeReader reader;
    reader.parse_buffer(gcode, [](GCodeReader &, const GCodeReader::GCodeLine &line) {
        const auto command = line.cmd();
        if (!command.empty()) {
            INFO("executable line: " << line.raw());
            CHECK_FALSE(std::isdigit(static_cast<unsigned char>(command.front())));
        }
    });
}

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

// The outline written to EXCLUDE_OBJECT_DEFINE is simplified with exclude_object_tolerance. A larger value must
// leave fewer points, which keeps the exclude-object array small on printers that stall while parsing it.
TEST_CASE("A larger exclude object tolerance writes fewer outline points", "[GCode]")
{
    const auto slice_with_tolerance = [](const std::string &tolerance) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({{"gcode_flavor", "klipper"}, {"exclude_object", "1"},
                                       {"exclude_object_tolerance", tolerance}});
        // Default facet angle (2 degrees): 180 hull points, well inside either tolerance.
        return Test::slice({make_cylinder(20., 5.)}, config);
    };
    // Points of the POLYGON=[[x,y],...] on the first EXCLUDE_OBJECT_DEFINE line.
    const auto polygon_points = [](const std::string &gcode) {
        const size_t define = gcode.find("EXCLUDE_OBJECT_DEFINE");
        REQUIRE(define != std::string::npos);
        const size_t begin = gcode.find("POLYGON=", define);
        REQUIRE(begin != std::string::npos);
        const std::string polygon = gcode.substr(begin, gcode.find('\n', begin) - begin);
        size_t points = 1;
        for (size_t pos = polygon.find("],["); pos != std::string::npos; pos = polygon.find("],[", pos + 1))
            ++points;
        return points;
    };

    const size_t fine   = polygon_points(slice_with_tolerance("0.1"));
    const size_t coarse = polygon_points(slice_with_tolerance("0.4"));
    INFO("points at 0.1 mm: " << fine << ", at 0.4 mm: " << coarse);
    CHECK(coarse >= 4);
    CHECK(coarse < fine);
}

// Existing profiles must keep writing the outlines they wrote when the tolerance was a constant 0.1 mm.
TEST_CASE("The exclude object tolerance defaults to the former fixed 0.1 mm", "[GCode]")
{
    CHECK_THAT(DynamicPrintConfig::full_print_config().opt_float("exclude_object_tolerance"),
               Catch::Matchers::WithinAbs(0.1, 1e-9));
}

TEST_CASE("The filament flow ratio scales the extruded length once", "[GCode]")
{
    // Retraction off, so every positive relative E move is extrusion and scales with the ratio.
    auto extruded_length = [](double flow_ratio) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({ { "use_relative_e_distances", "1" }, { "retraction_length", "0" }, { "wipe", "0" } });
        config.set_key_value("filament_flow_ratio", new ConfigOptionFloatsNullable{ flow_ratio });
        double total = 0.;
        GCodeReader reader;
        reader.parse_buffer(Test::slice({ Test::cube(20) }, config),
                            [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
                                if ((line.cmd_is("G1") || line.cmd_is("G2") || line.cmd_is("G3")) && line.has_e() && line.e() > 0.f)
                                    total += line.e();
                            });
        return total;
    };

    const double full = extruded_length(1.);
    REQUIRE(full > 0.);
    CHECK_THAT(extruded_length(0.9) / full, Catch::Matchers::WithinRel(0.9, 1e-3));
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

// A filament that starts above the first layer, such as a support interface, never touches the bed. Heating the
// bed to its temperature would soften the filament the part stands on, so only first layer filaments count.
TEST_CASE("The highest temperature bed formula ignores filaments that start above the first layer", "[GCode]")
{
    // Filament 1: bed 65 on the first layer, 60 after it. Filament 2: bed 100. A 30 mm cap on a 10 mm tall stem prints
    // with filament 1 and is supported from the bed, the support with filament 1 and its interface under the cap
    // with filament 2.
    TriangleMesh mesh = make_cube(10, 10, 10);
    mesh.translate(10, 10, 0);
    TriangleMesh cap = make_cube(30, 30, 2);
    cap.translate(0, 0, 10);
    mesh.merge(cap);
    DynamicPrintConfig config = Test::multifilament_config(2, {
        {"bed_temperature_formula", "by_highest_temp"},
        {"enable_prime_tower", 0},
        {"enable_support", 1},
        {"support_filament", 1},
        {"support_interface_filament", 2},
        {"supertack_plate_temp_initial_layer", "65,100"},
        {"cool_plate_temp_initial_layer", "65,100"},
        {"textured_cool_plate_temp_initial_layer", "65,100"},
        {"eng_plate_temp_initial_layer", "65,100"},
        {"hot_plate_temp_initial_layer", "65,100"},
        {"textured_plate_temp_initial_layer", "65,100"},
        {"supertack_plate_temp", "60,100"},
        {"cool_plate_temp", "60,100"},
        {"textured_cool_plate_temp", "60,100"},
        {"eng_plate_temp", "60,100"},
        {"hot_plate_temp", "60,100"},
        {"textured_plate_temp", "60,100"},
        {"skirt_loops", 0},
        {"brim_type", "no_brim"},
    });
    const std::string gcode = Test::slice({mesh}, config);
    REQUIRE(gcode.find("\nT1") != std::string::npos);

    std::vector<int> bed_temps;
    std::istringstream lines(gcode);
    for (std::string line; std::getline(lines, line);)
        if (line.rfind("M140 S", 0) == 0 || line.rfind("M190 S", 0) == 0)
            bed_temps.push_back(std::stoi(line.substr(6)));
    INFO("bed temperatures: " << Catch::Detail::stringify(bed_temps));
    REQUIRE_FALSE(bed_temps.empty());
    CHECK(bed_temps.front() == 65);
    CHECK(std::find(bed_temps.begin(), bed_temps.end(), 60) != bed_temps.end());
    CHECK(std::find(bed_temps.begin(), bed_temps.end(), 100) == bed_temps.end());
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

TEST_CASE("Sparse infill only arc fitting emits arcs in the sparse infill alone", "[GCode]")
{
    const auto arcs_per_type = [](const char *arc_fitting) {
        // 360 facets keep the walls and the concentric infill within the resolution of a circle, so both fit arcs.
        const std::string gcode = Test::slice({ make_cylinder(25.0, 10.0, 2.0 * PI / 360.0) }, {
            { "enable_arc_fitting",    arc_fitting },
            { "sparse_infill_pattern", "concentric" },
            { "sparse_infill_density", "20%" },
            { "z_hop",                 "0" },
        });
        std::map<std::string, size_t> arcs;
        std::istringstream input(gcode);
        std::string line, type;
        while (std::getline(input, line)) {
            if (line.rfind(";TYPE:", 0) == 0)
                type = line.substr(6);
            else if (line.rfind("G2 ", 0) == 0 || line.rfind("G3 ", 0) == 0)
                ++arcs[type];
        }
        return arcs;
    };

    CHECK(arcs_per_type("enabled").count("Outer wall") == 1);

    const std::map<std::string, size_t> sparse_only = arcs_per_type("sparse_infill");
    CHECK(sparse_only.count("Sparse infill") == 1);
    for (const auto &[type, count] : sparse_only) {
        INFO(type << ": " << count << " arcs");
        CHECK(type == "Sparse infill");
    }

    CHECK(arcs_per_type("disabled").empty());
}

TEST_CASE("Exported moves use the decimal places selected by the G-code precision option, and supports use the standard ones", "[GCode]")
{
    const bool high_precision = GENERATE(false, true);
    CAPTURE(high_precision);
    // The pressure equalizer rewrites moves after the writer has formatted them.
    const std::string gcode = Test::slice({ Test::TestMesh::overhang }, {
        { "high_precision_gcode",                high_precision },
        { "max_volumetric_extrusion_rate_slope", 10 },
        { "enable_support",                      1 },
        { "use_relative_e_distances",            1 },
    });

    // Trailing zeros are not written, so the longest fraction per axis shows the precision in use.
    // Only the extrusions of a support are counted apart: its travels that change Z, and the travel to the object
    // path after it, keep the precision of the option.
    std::map<char, size_t> max_decimals;
    std::map<char, size_t> max_support_decimals;
    size_t support_extrusions = 0;
    bool in_support = false;
    std::istringstream input(gcode);
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind(";TYPE:", 0) == 0)
            in_support = line.rfind(";TYPE:Support", 0) == 0;
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G2 ", 0) != 0 && line.rfind("G3 ", 0) != 0)
            continue;
        std::istringstream words(line.substr(0, line.find(';')));
        std::string word;
        words >> word;
        std::map<char, std::string> values;
        while (words >> word)
            values[word.front()] = word.substr(1);
        // With relative E distances, a positive E on an XY move is an extrusion.
        const bool extrusion = (values.count('X') || values.count('Y')) && values.count('E') && values['E'].front() != '-';
        const bool support_extrusion = in_support && extrusion;
        support_extrusions += support_extrusion;
        for (const auto &[axis, value] : values) {
            const size_t point = value.find('.');
            size_t &decimals = (support_extrusion ? max_support_decimals : max_decimals)[axis];
            decimals = std::max(decimals, point == std::string::npos ? size_t(0) : value.size() - point - 1);
        }
    }

    const size_t xyz_decimals = high_precision ? 4 : 3;
    const size_t e_decimals   = high_precision ? 6 : 5;
    CHECK(max_decimals['X'] == xyz_decimals);
    CHECK(max_decimals['Y'] == xyz_decimals);
    CHECK(max_decimals['Z'] <= xyz_decimals);
    CHECK(max_decimals['E'] == e_decimals);
    CHECK(max_decimals['F'] <= 3);
    // Supports are removed after printing, so they keep the standard precision.
    REQUIRE(support_extrusions > 0);
    CHECK(max_support_decimals['X'] == 3);
    CHECK(max_support_decimals['Y'] == 3);
    CHECK(max_support_decimals['E'] == 5);
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

TEST_CASE("Z hop follows the lift settings of the filament printing", "[GCode]")
{
    // One nozzle, two filaments. Filament 2 overrides one lift setting; filament 1 keeps the printer's default,
    // which lifts on every layer.
    auto [key, value] = GENERATE(table<std::string, std::string>({
        {"filament_retract_lift_above", "nil,1"},             // only lift Z above 1 mm
        {"filament_retract_lift_enforce", "nil,Bottom Only"}, // only lift on the first layer
    }));
    const int filament = GENERATE(1, 2);
    CAPTURE(key, filament);
    DynamicPrintConfig config = Test::multifilament_config(2, {
        // each filament its own variant column, as in a real preset
        {"filament_self_index", "1,2"},
        {"filament_extruder_variant", "Direct Drive Standard;Direct Drive Standard"},
        {"z_hop", "0.6"},
        {key, value},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"enable_prime_tower", 0},
    });
    Print print;
    Model model;
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides(2, {{"extruder", std::to_string(filament)}});
    // Two objects, so every layer travels between them with a retraction.
    Test::init_print({Test::cube(20), Test::cube(20)}, print, model, config, &overrides);

    // The heights of the layers with a move that lifts Z above the layer.
    std::set<double>   lift_layers;
    double             layer_z = 0.;
    std::istringstream lines(Test::gcode(print));
    std::string        line;
    const std::regex   z_word(R"(\bZ([-0-9.]+))");
    while (std::getline(lines, line)) {
        if (line.rfind(";Z:", 0) == 0) {
            layer_z = std::stod(line.substr(3));
            continue;
        }
        // Skip the start G-code, which lifts the nozzle before the first layer.
        if (layer_z <= 0. || (line.rfind("G1 ", 0) != 0 && line.rfind("G0 ", 0) != 0))
            continue;
        const std::string move = line.substr(0, line.find(';'));
        std::smatch       match;
        if (std::regex_search(move, match, z_word) && std::stod(match[1]) > layer_z + 0.3)
            lift_layers.insert(layer_z);
    }

    REQUIRE_FALSE(lift_layers.empty());
    const double first = *lift_layers.begin();
    const double last  = *lift_layers.rbegin();
    if (filament == 1) {
        CHECK(first < 0.2 + EPSILON);
        CHECK(last > 1.);
    } else if (key == "filament_retract_lift_above")
        CHECK(first > 1. - EPSILON);
    else
        CHECK(last < 0.2 + EPSILON);
}

// A temperature tower re-targets the hotend at every layer. On a multi-tool printer that
// command has to name the tool printing the tower: a bare M104 goes to whichever tool the
// firmware considers active, which the first-layer M109 right before it already addresses by
// tool index. A single-tool printer keeps the bare form.
TEST_CASE("The temperature tower steps the temperature of the tool printing it", "[GCode][Calib]")
{
    const bool multi_tool = GENERATE(false, true);
    DYNAMIC_SECTION((multi_tool ? "two direct drive tools, the tower on the second one (filament 2 -> T1)" : "one tool")) {
        DynamicPrintConfig config = multi_tool ?
            Test::multifilament_config(2, {
                {"nozzle_diameter", "0.4,0.4"},
                {"printer_extruder_id", "1,2"},
                {"printer_extruder_variant", "Direct Drive Standard,Direct Drive Standard"},
                {"extruder_printable_height", "0,0"},
                {"filament_self_index", "1,2"},
                {"single_extruder_multi_material", 0},
                {"enable_prime_tower", 0},
                {"nozzle_temperature_initial_layer", "230,230"},
                {"nozzle_temperature", "230,230"},
            }) :
            DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            {"skirt_loops", 0},
            {"brim_type", "no_brim"},
            // Without the end G-code's heater-off M104 S0, every temperature command after the
            // first layer change is the tower's.
            {"machine_end_gcode", ""},
        });
        if (multi_tool)
            Test::size_flush_to_nozzles(config);

        const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{{"extruder", multi_tool ? 2 : 1}}};
        Print print;
        Model model;
        // A 20 mm cube at the default 0.2 mm layer height is 100 layers, more than ten per band.
        Test::init_print({Test::cube(20)}, print, model, config, &overrides);
        Calib_Params params;
        params.mode  = CalibMode::Calib_Temp_Tower;
        params.start = 230;
        params.end   = 190;
        params.step  = 5;
        print.set_calib_params(params);
        const std::string gcode = Test::gcode(print);

        const size_t layers_begin = gcode.find(";LAYER_CHANGE");
        REQUIRE(layers_begin != std::string::npos);
        const std::regex      temperature_command(R"(^M10[49] S(\d+)(?: T(\d+))?)");
        std::set<int>         temperatures;
        std::set<std::string> tools;
        std::istringstream    stream(gcode.substr(layers_begin));
        for (std::string line; std::getline(stream, line);) {
            std::smatch m;
            if (!std::regex_search(line, m, temperature_command))
                continue;
            temperatures.insert(std::stoi(m[1]));
            tools.insert(m[2].matched ? m[2].str() : "none");
        }
        // Every 5 degree band from the start to the end temperature, and nothing else.
        CHECK(temperatures == std::set<int>{190, 195, 200, 205, 210, 215, 220, 225, 230});
        CHECK(tools == std::set<std::string>{multi_tool ? "1" : "none"});
    }
}
