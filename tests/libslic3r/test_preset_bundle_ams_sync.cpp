#include <catch2/catch_all.hpp>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

using namespace Slic3r;

namespace {

// In-memory system filament preset. Mirrors the helper in test_preset_bundle_loading.cpp.
Preset &add_system_filament(PresetBundle &bundle, const std::string &name, const std::string &filament_id, const std::string &filament_type)
{
    DynamicPrintConfig config(bundle.filaments.default_preset().config);
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = "";
    config.option<ConfigOptionStrings>("filament_type", true)->values    = {filament_type};
    Preset &preset = bundle.filaments.load_preset(std::string(), name, config, /*select=*/false);
    preset.is_system   = true;
    preset.filament_id = filament_id;
    return preset;
}

// One AMS tray entry as build_filament_ams_list produces it.
DynamicPrintConfig make_tray(const std::string &filament_id, const std::string &filament_type, const std::string &color, const std::string &ams_id,
                             const std::string &slot_id)
{
    DynamicPrintConfig tray;
    tray.set_key_value("filament_id", new ConfigOptionStrings{filament_id});
    tray.set_key_value("filament_type", new ConfigOptionStrings{filament_type});
    tray.set_key_value("filament_colour", new ConfigOptionStrings{color});
    tray.set_key_value("filament_colour_type", new ConfigOptionStrings{"1"});
    tray.set_key_value("filament_multi_colour", new ConfigOptionStrings{});
    tray.set_key_value("ams_id", new ConfigOptionStrings{ams_id});
    tray.set_key_value("slot_id", new ConfigOptionStrings{slot_id});
    return tray;
}

} // namespace

// A tray the printer UI wrote carries a material type but no OrcaSlicer preset id. It must still
// sync, resolved to Generic <type> with the tray's own color.
TEST_CASE("AMS sync resolves a printer-set tray to Generic by material type", "[Preset][AMS]")
{
    PresetBundle bundle;
    add_system_filament(bundle, "Generic PETG @Q2", "OFYPdQJh", "PETG");
    add_system_filament(bundle, "Generic PLA @Q2", "GFL99", "PLA");

    bundle.filament_ams_list[0] = make_tray("OFYPdQJh", "PETG", "#FE717A", "0", "0"); // preset-backed
    bundle.filament_ams_list[1] = make_tray("", "PETG", "#898F9B", "0", "1");         // printer-set, no id
    bundle.filament_ams_list[2] = make_tray("", "PLA", "#FAFAFA", "0", "2");          // printer-set, no id
    bundle.filament_ams_list[3] = make_tray("", "", "#000000", "0", "3");             // empty slot

    std::vector<std::pair<DynamicPrintConfig *, std::string>> unknowns;
    std::map<int, AMSMapInfo>                                 maps;
    MergeFilamentInfo                                         merge;

    const unsigned int count = bundle.sync_ams_list(unknowns, /*use_map=*/false, maps, /*enable_append=*/false, merge);

    CHECK(count == 3);
    REQUIRE(bundle.filament_presets.size() == 3);
    CHECK(bundle.filament_presets[0] == "Generic PETG @Q2");
    CHECK(bundle.filament_presets[1] == "Generic PETG @Q2");
    CHECK(bundle.filament_presets[2] == "Generic PLA @Q2");

    const ConfigOptionStrings *colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    REQUIRE(colors != nullptr);
    REQUIRE(colors->values.size() == 3);
    CHECK(colors->values[0] == "#FE717A");
    CHECK(colors->values[1] == "#898F9B");
    CHECK(colors->values[2] == "#FAFAFA");
}

// Without a preset id and without a material type there is nothing to resolve; the tray stays
// skipped in a direct sync.
TEST_CASE("AMS sync skips a tray without a preset id or a material type", "[Preset][AMS]")
{
    PresetBundle bundle;
    add_system_filament(bundle, "Generic PETG @Q2", "OFYPdQJh", "PETG");
    bundle.filament_ams_list[0] = make_tray("", "", "#000000", "0", "0");

    std::vector<std::pair<DynamicPrintConfig *, std::string>> unknowns;
    std::map<int, AMSMapInfo>                                 maps;
    MergeFilamentInfo                                         merge;

    CHECK(bundle.sync_ams_list(unknowns, /*use_map=*/false, maps, /*enable_append=*/false, merge) == 0);
    CHECK(bundle.filament_presets.empty());
}

// Guard the pre-existing behavior: a non-empty id that matches no preset still resolves by type.
TEST_CASE("AMS sync still resolves an unmatched preset id by material type", "[Preset][AMS]")
{
    PresetBundle bundle;
    add_system_filament(bundle, "Generic PETG @Q2", "OFYPdQJh", "PETG");
    bundle.filament_ams_list[0] = make_tray("REMOVED_ID", "PETG", "#123456", "0", "0");

    std::vector<std::pair<DynamicPrintConfig *, std::string>> unknowns;
    std::map<int, AMSMapInfo>                                 maps;
    MergeFilamentInfo                                         merge;

    CHECK(bundle.sync_ams_list(unknowns, /*use_map=*/false, maps, /*enable_append=*/false, merge) == 1);
    REQUIRE(bundle.filament_presets.size() == 1);
    CHECK(bundle.filament_presets[0] == "Generic PETG @Q2");
}

// Mapping-mode sync used to force "Generic PLA" for an id-less tray; it must use the type instead.
TEST_CASE("AMS sync in mapping mode resolves a printer-set tray to Generic, not Generic PLA", "[Preset][AMS]")
{
    PresetBundle bundle;
    add_system_filament(bundle, "Generic PETG @Q2", "OFYPdQJh", "PETG");
    add_system_filament(bundle, "Generic PLA @Q2", "GFL99", "PLA");

    bundle.filament_presets                                                     = {"Generic PLA @Q2"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values      = {"#000000"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour_type")->values = {"1"};

    bundle.filament_ams_list[0] = make_tray("", "PETG", "#898F9B", "0", "1");

    std::map<int, AMSMapInfo> maps;
    maps[0] = AMSMapInfo{"0", "1"};

    std::vector<std::pair<DynamicPrintConfig *, std::string>> unknowns;
    MergeFilamentInfo                                         merge;

    const unsigned int count = bundle.sync_ams_list(unknowns, /*use_map=*/true, maps, /*enable_append=*/false, merge);

    CHECK(count == 1);
    REQUIRE(bundle.filament_presets.size() == 1);
    CHECK(bundle.filament_presets[0] == "Generic PETG @Q2");
    CHECK(bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values[0] == "#898F9B");
}

// The dialog builds ams_mapping2 one entry per filament_presets index, while the generated
// G-code's toolchange names the filament's index in the slice's filament config arrays. Those
// are the same index only if full_config() collects the filament arrays in filament_presets
// order; if a refactor reorders one side, every mapped print aims at the wrong lane.
TEST_CASE("Full config filament arrays follow the selected filament preset order", "[Preset][AMS]")
{
    PresetBundle bundle;
    add_system_filament(bundle, "Generic PETG @Q2", "OFYPdQJh", "PETG");
    add_system_filament(bundle, "Generic PLA @Q2", "GFL99", "PLA");

    // Deliberately not the load order: the arrays must follow this selection.
    bundle.filament_presets = {"Generic PLA @Q2", "Generic PETG @Q2"};

    const DynamicPrintConfig full = bundle.full_config(/*apply_extruder=*/false);
    REQUIRE(full.option<ConfigOptionStrings>("filament_type") != nullptr);
    CHECK(full.option<ConfigOptionStrings>("filament_settings_id")->values == std::vector<std::string>{"Generic PLA @Q2", "Generic PETG @Q2"});
    CHECK(full.option<ConfigOptionStrings>("filament_type")->values == std::vector<std::string>{"PLA", "PETG"});
    // The AMS sync matches trays by filament_id, so this array must follow the same order.
    CHECK(full.option<ConfigOptionStrings>("filament_ids")->values == std::vector<std::string>{"GFL99", "OFYPdQJh"});
}
