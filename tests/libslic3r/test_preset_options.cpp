// Regression test for the "option in def + UI but missing from preset key list"
// crash class.
//
// Background: the print preset's DynamicPrintConfig is seeded with only the
// keys returned by Preset::print_options() (PresetBundle.cpp). If a new field
// is added to PrintRegionConfig / PrintObjectConfig and registered via
// print_config_def + a TabPrint optgroup, but forgotten from print_options(),
// the option's CheckBox/SpinCtrl will still get built (because
// append_single_option_line resolves the def), and on tab activation
// reload_config -> get_config_value will dispatch to opt_bool/opt_int on a
// DynamicPrintConfig that has no entry for the key. The accessor then null-
// derefs the result of option<T>(key), and the process exits with no
// recoverable error. We hit this on a feature branch when an option was added
// to print_config_def + an optgroup but missed in s_Preset_print_options; the
// crash signature was an opaque ACCESS_VIOLATION at offset 8 from null inside
// inlined opt_bool, with no log line naming the key.
//
// We assert the inverse invariant: every key declared on PrintRegionConfig and
// PrintObjectConfig must appear in Preset::print_options() or
// Preset::filament_options() (the two preset key lists that seed a print
// preset's DynamicConfig). Adding a new field to either macro list without
// the corresponding s_Preset_print_options / s_Preset_filament_options entry
// now fails this test instead of shipping as a runtime null-deref.

#include <catch2/catch_all.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <set>

using namespace Slic3r;

namespace {

// Region-level fields that are intentionally NOT in Preset::print_options() and
// NOT in Preset::filament_options() — they are slicing-internal and have no
// preset/UI exposure today. Listing them here keeps the invariant test honest
// about the carve-out: if you add a new option, do NOT just add it here to
// silence the test; add it to the appropriate preset key list.
const std::set<std::string> kKnownOrphanRegionFields = {
    "ironing_direction",
    "wall_infill_order",
};

void check_keys_are_in_a_preset(const t_config_option_keys &keys,
                                const std::string          &class_name)
{
    const auto &print_options    = Preset::print_options();
    const auto &filament_options = Preset::filament_options();
    const std::set<std::string> in_print(print_options.begin(),    print_options.end());
    const std::set<std::string> in_fila (filament_options.begin(), filament_options.end());
    for (const std::string &key : keys) {
        DYNAMIC_SECTION(class_name << "::" << key) {
            INFO("Field '" << key << "' on " << class_name
                 << " is registered in print_config_def but NOT in "
                    "Preset::print_options() OR Preset::filament_options(). "
                    "If a TabPrint/TabFilament optgroup adds an option_line for "
                    "this key, opt_bool/opt_int will null-deref on tab "
                    "activation. Add it to s_Preset_print_options in "
                    "src/libslic3r/Preset.cpp (or s_Preset_filament_options for "
                    "per-filament overrides).");
            const bool registered = in_print.count(key) || in_fila.count(key)
                                 || kKnownOrphanRegionFields.count(key);
            REQUIRE(registered);
        }
    }
}

} // namespace

TEST_CASE("Every PrintRegionConfig field is registered in a preset key list",
          "[Config][Preset]")
{
    check_keys_are_in_a_preset(PrintRegionConfig::defaults().keys(),
                               "PrintRegionConfig");
}

TEST_CASE("Every PrintObjectConfig field is registered in a preset key list",
          "[Config][Preset]")
{
    check_keys_are_in_a_preset(PrintObjectConfig::defaults().keys(),
                               "PrintObjectConfig");
}
