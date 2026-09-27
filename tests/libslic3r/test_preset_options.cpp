// Regression test for the "option in def + UI but missing from preset key list"
// crash class.
//
// The print preset's DynamicPrintConfig is seeded with only the keys returned by
// Preset::print_options() (PresetBundle.cpp). A field added to PrintRegionConfig
// or PrintObjectConfig and registered via print_config_def plus a TabPrint
// optgroup, but left out of print_options(), still gets its control built; on tab
// activation reload_config -> get_config_value dispatches to opt_bool/opt_int on a
// DynamicPrintConfig with no entry for the key, and the accessor null-derefs the
// result of option<T>(key).
//
// The invariant asserted here is the inverse: every key declared on
// PrintRegionConfig and PrintObjectConfig appears in Preset::print_options() or
// Preset::filament_options(), the two preset key lists that seed a print preset's
// DynamicConfig.

#include <catch2/catch_all.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <set>

using namespace Slic3r;

namespace {

// Deprecated keys renamed in handle_legacy() (ironing_direction ->
// ironing_angle, wall_infill_order -> wall_sequence); neither is in a
// preset list. Register new options in a preset list, not here.
const std::set<std::string> kDeprecatedRegionFields = {
    "ironing_direction",
    "wall_infill_order",
};

void check_keys_are_in_a_preset(const t_config_option_keys& keys, const std::string& class_name)
{
    REQUIRE_FALSE(keys.empty());
    const auto& print_options    = Preset::print_options();
    const auto& filament_options = Preset::filament_options();
    const std::set<std::string> in_print(print_options.begin(), print_options.end());
    const std::set<std::string> in_filament(filament_options.begin(), filament_options.end());
    for (const std::string& key : keys) {
        DYNAMIC_SECTION(class_name << "::" << key)
        {
            INFO("'" << key << "' on " << class_name
                     << " is missing from "
                        "Preset::print_options()/filament_options(); add it to "
                        "s_Preset_print_options (or s_Preset_filament_options) in Preset.cpp.");
            const bool registered = in_print.count(key) || in_filament.count(key) || kDeprecatedRegionFields.count(key);
            REQUIRE(registered);
        }
    }
}

} // namespace

// Bodies are laid out like the rest of the test suite rather than collapsed
// onto the brace line.
// clang-format off
TEST_CASE("Every PrintRegionConfig field is registered in a preset key list", "[Preset][Config]")
{
    check_keys_are_in_a_preset(PrintRegionConfig::defaults().keys(), "PrintRegionConfig");
}

TEST_CASE("Every PrintObjectConfig field is registered in a preset key list", "[Preset][Config]")
{
    check_keys_are_in_a_preset(PrintObjectConfig::defaults().keys(), "PrintObjectConfig");
}
// clang-format on
