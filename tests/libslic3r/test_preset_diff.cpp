#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include "libslic3r/Config.hpp"
#include <vector>
#include <cstddef>
#include <string>

using namespace Slic3r;

// Regression test for the python-plugin branch's intentional divergence from
// upstream in add_correct_opts_to_diff() (src/libslic3r/Preset.cpp): a vector
// option entry whose index is beyond the reference vector's length is reported
// dirty even when it duplicates an existing value. On main these duplicates
// were NOT flagged. See the comment on add_correct_opts_to_diff() in src/libslic3r/Preset.cpp.
TEST_CASE("deep_diff flags new vector entries that duplicate values[0]", "[PresetDiff][Config]")
{
    // reference: single-extruder vector (one entry)
    Preset reference(Preset::TYPE_PRINTER, "ref");
    reference.config.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4});

    // edited: a second extruder entry was added whose value duplicates the first
    Preset edited(Preset::TYPE_PRINTER, "edited");
    edited.config.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4, 0.4});

    // deep_compare = true routes through deep_diff() -> add_correct_opts_to_diff()
    std::vector<std::string> diff =
        PresetCollection::dirty_options(&edited, &reference, /*deep_compare=*/true);

    // The new index #1 is reported dirty even though 0.4 == values[0] (0.4).
    REQUIRE(std::find(diff.begin(), diff.end(), "nozzle_diameter#1") != diff.end());

    // Sanity: the unchanged existing index #0 is NOT reported, so the rule is
    // specific to new indices rather than flagging the whole vector.
    REQUIRE(std::find(diff.begin(), diff.end(), "nozzle_diameter#0") == diff.end());
}

TEST_CASE("deep_diff distinguishes absolute and percentage speeds for each variant", "[PresetDiff][Config]")
{
    const size_t changed_index = GENERATE(size_t(0), size_t(1));
    Preset reference(Preset::TYPE_PRINT, "ref");
    reference.config.set_key_value("small_perimeter_speed", new ConfigOptionFloatsOrPercents{{50., false}, {50., false}});

    Preset edited = reference;
    edited.config.option<ConfigOptionFloatsOrPercents>("small_perimeter_speed")->values[changed_index].percent = true;

    const auto diff = PresetCollection::dirty_options(&edited, &reference, /*deep_compare=*/true);
    REQUIRE(diff == std::vector<std::string>{"small_perimeter_speed#" + std::to_string(changed_index)});

    DynamicPrintConfig transferred = reference.config;
    transferred.apply_only(edited.config, diff);
    REQUIRE(*transferred.option("small_perimeter_speed") == *edited.config.option("small_perimeter_speed"));
}

TEST_CASE("multi-extruder mixed nozzle preset compatibility", "[Preset][Compatibility]")
{
    Preset printer(Preset::TYPE_PRINTER, "Snapmaker U1 (0.4 nozzle)");
    printer.config.set_key_value("printer_model", new ConfigOptionString("Snapmaker U1"));
    printer.config.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4, 0.4, 0.4, 0.8});

    PresetWithVendorProfile active_printer(printer, nullptr);

    // 0.4 nozzle process preset: should be compatible
    Preset process_04(Preset::TYPE_PRINT, "0.20 Standard @Snapmaker U1 (0.4 nozzle)");
    process_04.config.set_key_value("compatible_printers", new ConfigOptionStrings{"Snapmaker U1 (0.4 nozzle)"});
    CHECK(is_compatible_with_printer(PresetWithVendorProfile(process_04, nullptr), active_printer));

    // 0.8 nozzle process preset: should be compatible because extruder 3 has 0.8mm nozzle
    Preset process_08(Preset::TYPE_PRINT, "0.24 Standard @Snapmaker U1 (0.8 nozzle)");
    process_08.config.set_key_value("compatible_printers", new ConfigOptionStrings{"Snapmaker U1 (0.8 nozzle)"});
    CHECK(is_compatible_with_printer(PresetWithVendorProfile(process_08, nullptr), active_printer));

    // 0.2 nozzle process preset: should NOT be compatible (no 0.2 nozzle installed)
    Preset process_02(Preset::TYPE_PRINT, "0.06 Standard @Snapmaker U1 (0.2 nozzle)");
    process_02.config.set_key_value("compatible_printers", new ConfigOptionStrings{"Snapmaker U1 (0.2 nozzle)"});
    CHECK_FALSE(is_compatible_with_printer(PresetWithVendorProfile(process_02, nullptr), active_printer));

    // 0.6 nozzle process preset: should NOT be compatible (no 0.6 nozzle installed)
    Preset process_06(Preset::TYPE_PRINT, "0.18 Standard @Snapmaker U1 (0.6 nozzle)");
    process_06.config.set_key_value("compatible_printers", new ConfigOptionStrings{"Snapmaker U1 (0.6 nozzle)"});
    CHECK_FALSE(is_compatible_with_printer(PresetWithVendorProfile(process_06, nullptr), active_printer));

    // Single-extruder printer with 0.4 nozzle only: 0.8 preset should NOT be compatible
    Preset single_printer(Preset::TYPE_PRINTER, "Snapmaker U1 (0.4 nozzle)");
    single_printer.config.set_key_value("printer_model", new ConfigOptionString("Snapmaker U1"));
    single_printer.config.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4});
    PresetWithVendorProfile active_single_printer(single_printer, nullptr);
    CHECK(is_compatible_with_printer(PresetWithVendorProfile(process_04, nullptr), active_single_printer));
    CHECK_FALSE(is_compatible_with_printer(PresetWithVendorProfile(process_08, nullptr), active_single_printer));

    // Filament preset with 0.8 nozzle compatibility
    Preset filament_08(Preset::TYPE_FILAMENT, "Snapmaker PLA SnapSpeed @U1 0.8 nozzle");
    filament_08.config.set_key_value("compatible_printers", new ConfigOptionStrings{"Snapmaker U1 (0.8 nozzle)"});
    CHECK(is_compatible_with_printer(PresetWithVendorProfile(filament_08, nullptr), active_printer));

    // Longer model prefix collision: printer model "Ender-3" must NOT match "Ender-3 V2 (0.8 nozzle)"
    Preset ender3(Preset::TYPE_PRINTER, "Creality Ender-3 (0.4 nozzle)");
    ender3.config.set_key_value("printer_model", new ConfigOptionString("Creality Ender-3"));
    ender3.config.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4, 0.8});
    PresetWithVendorProfile active_ender3(ender3, nullptr);

    Preset ender3_v2_preset(Preset::TYPE_PRINT, "0.20 Standard @Creality Ender-3 V2 (0.8 nozzle)");
    ender3_v2_preset.config.set_key_value("compatible_printers", new ConfigOptionStrings{"Creality Ender-3 V2 (0.8 nozzle)"});
    CHECK_FALSE(is_compatible_with_printer(PresetWithVendorProfile(ender3_v2_preset, nullptr), active_ender3));
}

