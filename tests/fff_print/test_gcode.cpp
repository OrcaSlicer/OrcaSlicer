#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <catch2/catch_message.hpp>
#include <boost/filesystem/operations.hpp>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_helpers.hpp"

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Point.hpp"
#include <cctype>
#include <map>
#include <string>
#include <vector>

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
