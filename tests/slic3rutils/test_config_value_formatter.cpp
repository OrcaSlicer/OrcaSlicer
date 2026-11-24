#include <catch2/catch_test_macros.hpp>

#include <string>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/ConfigValueFormatter.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

TEST_CASE("An enum vector index past the end of the vector reads as undefined", "[ConfigValueFormatter][Regression]")
{
    const std::string key = "filament_retract_lift_enforce";
    const ConfigOptionDef* def = print_config_def.get(key);
    DynamicPrintConfig config;
    config.set_key_value(key, new ConfigOptionEnumsGenericNullable(def->enum_keys_map, 1, ConfigOptionEnumsGenericNullable::nil_value()));

    CHECK(get_string_value(key + "#1", config) == "Undefined");
}

TEST_CASE("An enum vector index inside the vector reads as its label", "[ConfigValueFormatter]")
{
    const std::string key = "filament_retract_lift_enforce";
    const ConfigOptionDef* def = print_config_def.get(key);
    DynamicPrintConfig config;
    config.set_key_value(key, new ConfigOptionEnumsGenericNullable(def->enum_keys_map, 1, def->enum_keys_map->at("Top Only")));

    CHECK(get_string_value(key + "#0", config).utf8_string() == "Top Only");
}

TEST_CASE("An enum value outside the label list reads as undefined", "[ConfigValueFormatter]")
{
    const std::string key = "filament_retract_lift_enforce";
    const ConfigOptionDef* def = print_config_def.get(key);
    DynamicPrintConfig config;
    config.set_key_value(key, new ConfigOptionEnumsGenericNullable(def->enum_keys_map, 1, int(def->enum_labels.size())));

    CHECK(get_string_value(key + "#0", config) == "Undefined");
}

TEST_CASE("A serialized list read without an index reads as the settings tab shows it", "[ConfigValueFormatter]")
{
    DynamicPrintConfig config;
    config.set_key_value("post_process", new ConfigOptionStrings({"a.ps1", "b.ps1"}));
    config.set_key_value("small_area_infill_flow_compensation_model", new ConfigOptionStrings({"0,0", "\n0.2,0.4444"}));

    CHECK(get_string_value("post_process", config).utf8_string() == "a.ps1;b.ps1;");
    CHECK(get_string_value("small_area_infill_flow_compensation_model", config).utf8_string() == "0,0;\n0.2,0.4444;");
}

TEST_CASE("A string list that is not serialized reads its first entry without an index", "[ConfigValueFormatter]")
{
    DynamicPrintConfig config;
    config.set_key_value("filament_ids", new ConfigOptionStrings({"GFA00", "GFA01"}));

    CHECK(get_string_value("filament_ids", config).utf8_string() == "GFA00");
}
