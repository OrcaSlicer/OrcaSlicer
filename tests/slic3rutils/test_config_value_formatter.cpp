#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/ConfigValueFormatter.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

TEST_CASE("ConfigValueFormatter pure key extraction", "[ConfigValueFormatter]")
{
    REQUIRE(get_pure_opt_key("filament_max_volumetric_speed#1") == "filament_max_volumetric_speed");
    REQUIRE(get_pure_opt_key("nozzle_diameter#0") == "nozzle_diameter");
    REQUIRE(get_pure_opt_key("layer_height") == "layer_height");
}

TEST_CASE("ConfigValueFormatter handles undefined or unregistered options safely without crashing", "[ConfigValueFormatter]")
{
    DynamicPrintConfig config;

    // Missing key
    REQUIRE(get_string_value("nonexistent_option", config) == "N/A");
    REQUIRE(get_full_label("nonexistent_option", config) == "N/A");

    // Dynamic key not registered in ConfigDef (such as cloud IDs or custom keys)
    config.set_key_value("custom_unregistered_key", new ConfigOptionString("custom_value"));
    REQUIRE(get_string_value("custom_unregistered_key", config) == "custom_value");
    REQUIRE(get_full_label("custom_unregistered_key", config) == "N/A");

    // Vector option with index out of bounds
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4});
    REQUIRE(get_string_value("nozzle_diameter#0", config) == "0.4");
    REQUIRE(get_string_value("nozzle_diameter#5", config) == "Undefined");

    // Float or percent
    config.set_key_value("small_perimeter_speed", new ConfigOptionFloatsOrPercents{{50.0, false}});
    REQUIRE(get_string_value("small_perimeter_speed#0", config) == "50");
    REQUIRE(get_string_value("small_perimeter_speed#5", config) == "Undefined");
}
