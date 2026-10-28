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

TEST_CASE("A string list read without an index lists every entry", "[ConfigValueFormatter]")
{
    DynamicPrintConfig config;
    config.set_key_value("post_process", new ConfigOptionStrings({"a.ps1", "b.ps1"}));

    CHECK(get_string_value("post_process", config).utf8_string() == "a.ps1\nb.ps1");
}

TEST_CASE("A compatible printers list reads as its printers, one per line", "[ConfigValueFormatter]")
{
    DynamicPrintConfig config;
    config.set_key_value("compatible_printers", new ConfigOptionStrings({"Printer A", "Printer B"}));

    CHECK(get_string_value("compatible_printers", config).utf8_string() == "Printer A\nPrinter B");
}

TEST_CASE("An empty compatible printers list reads as all printers", "[ConfigValueFormatter]")
{
    DynamicPrintConfig config;
    config.set_key_value("compatible_printers", new ConfigOptionStrings());

    CHECK(get_string_value("compatible_printers", config) == "All");
}
