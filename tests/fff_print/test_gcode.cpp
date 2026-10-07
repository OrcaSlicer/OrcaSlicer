#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <catch2/catch_message.hpp>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_helpers.hpp"

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Point.hpp"
#include <string>
#include <vector>

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

TEST_CASE("CONFIG_BLOCK tags initial extruder temperature and nozzle diameter for multi-extruder printers", "[GCode]")
{
    DynamicPrintConfig config = Test::multifilament_config(4, {
        {"gcode_flavor", "klipper"},
        {"nozzle_diameter", "0.4,0.4,0.4,0.8"},
        {"nozzle_temperature_initial_layer", "200,205,210,240"},
        {"cool_plate_temp_initial_layer", "50,55,60,70"},
        {"filament_self_index", "1,2,3,4"}
    });

    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{ { {"extruder", "4"} } };
    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{Test::cube(20.)}, print, model, config, &overrides);

    const std::string gcode = Test::gcode(print);

    // Verify CONFIG_BLOCK tags extruder 4 (index 3) values
    CHECK(gcode.find("; nozzle_diameter = 0.8\n") != std::string::npos);
    CHECK(gcode.find("; first_layer_temperature = 240\n") != std::string::npos);
    CHECK(gcode.find("; first_layer_bed_temperature = 70\n") != std::string::npos);

    // Verify the initial tag comes before the serialized vector
    size_t initial_tag = gcode.find("; nozzle_diameter = 0.8\n");
    size_t vector_tag  = gcode.find("; nozzle_diameter = 0.4,0.4,0.4,0.8\n");
    REQUIRE(initial_tag != std::string::npos);
    REQUIRE(vector_tag != std::string::npos);
    CHECK(initial_tag < vector_tag);
}

TEST_CASE("CONFIG_BLOCK preserves initial extruder tags across by-object sequential printing", "[GCode]")
{
    DynamicPrintConfig config = Test::multifilament_config(2, {
        {"gcode_flavor", "klipper"},
        {"print_sequence", "by object"},
        {"nozzle_diameter", "0.8,0.4"},
        {"nozzle_temperature_initial_layer", "240,200"},
        {"cool_plate_temp_initial_layer", "70,50"},
        {"filament_self_index", "1,2"}
    });

    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        { {"extruder", "1"} },
        { {"extruder", "2"} }
    };

    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{Test::cube(20.), Test::cube(20.)}, print, model, config, &overrides);

    const std::string gcode = Test::gcode(print);

    CHECK(gcode.find("; nozzle_diameter = 0.8\n") != std::string::npos);
    CHECK(gcode.find("; first_layer_temperature = 240\n") != std::string::npos);
    CHECK(gcode.find("; first_layer_bed_temperature = 70\n") != std::string::npos);
}

