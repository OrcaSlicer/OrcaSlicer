#include <catch2/catch_all.hpp>

#include <cstddef>
#include <libslic3r/Config.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "catch2/catch_test_macros.hpp"
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

// The filament colours of a device filament list in the order its map iterates.
std::vector<std::string> colours_in_map_order(const std::map<int, DynamicPrintConfig> &list)
{
    std::vector<std::string> out;
    for (const auto &entry : list)
        out.push_back(entry.second.opt_string("filament_colour", 0u));
    return out;
}

void check_filament_order(const std::map<int, DynamicPrintConfig> &list, const std::vector<std::string> &expected)
{
    const std::vector<std::string> actual = colours_in_map_order(list);
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        CHECK(actual[i] == expected[i]);
}

std::string ams_test_colour(int key) { return "#A" + std::to_string(key); }
std::string external_test_colour(int tool) { return "#E" + std::to_string(tool); }

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
    CHECK(unknowns.empty());
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
    CHECK(unknowns.size() == 1);
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
    CHECK(unknowns.empty());
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

// get_ams_cobox_infos builds the combo arrays that SyncAmsInfoDialog pairs with AMS colors
// positionally. A printer-set tray must appear so the array stays aligned with the sync output.
TEST_CASE("AMS combo info keeps a printer-set tray and stays aligned", "[Preset][AMS]")
{
    PresetBundle bundle;
    add_system_filament(bundle, "Generic PETG @Q2", "OFYPdQJh", "PETG");
    add_system_filament(bundle, "Generic PLA @Q2", "GFL99", "PLA");

    auto tray_a = make_tray("OFYPdQJh", "PETG", "#FE717A", "0", "0");
    tray_a.set_key_value("tray_name", new ConfigOptionStrings{"A"});
    auto tray_b = make_tray("", "PETG", "#898F9B", "0", "1");
    tray_b.set_key_value("tray_name", new ConfigOptionStrings{"B"});
    auto tray_c = make_tray("GFL99", "PLA", "#FAFAFA", "0", "2");
    tray_c.set_key_value("tray_name", new ConfigOptionStrings{"C"});
    bundle.filament_ams_list[0] = tray_a;
    bundle.filament_ams_list[1] = tray_b;
    bundle.filament_ams_list[2] = tray_c;

    AMSComboInfo combo_info;
    bundle.get_ams_cobox_infos(combo_info);

    // The id-less PETG tray resolves to a generic preset instead of being dropped, so C keeps
    // its index and its color.
    REQUIRE(combo_info.ams_filament_presets.size() == 3);
    CHECK(combo_info.ams_filament_presets[0] == "Generic PETG @Q2");
    CHECK(combo_info.ams_filament_presets[1] == "Generic PETG @Q2");
    CHECK(combo_info.ams_filament_presets[2] == "Generic PLA @Q2");
    REQUIRE(combo_info.ams_filament_colors.size() == 3);
    CHECK(combo_info.ams_filament_colors[0] == "#FE717A");
    CHECK(combo_info.ams_filament_colors[1] == "#898F9B");
    CHECK(combo_info.ams_filament_colors[2] == "#FAFAFA");
    REQUIRE(combo_info.ams_names.size() == 3);
    CHECK(combo_info.ams_names[0] == "A");
    CHECK(combo_info.ams_names[1] == "B");
    CHECK(combo_info.ams_names[2] == "C");
}

// A generic printer keys an external spool by its virtual tray id (255 -> toolhead 0, 254 -> 1, ...).
// The sidebar's list iterates by key, so without reordering the sync filled the toolheads in reverse.
TEST_CASE("External spools iterate in toolhead order", "[Preset][AMS]")
{
    std::map<int, DynamicPrintConfig> list;
    for (int tool = 0; tool < 4; ++tool) {
        const int          tray_id = 255 - tool;
        DynamicPrintConfig entry   = make_tray("", "PLA", external_test_colour(tool), std::to_string(tray_id), "0");
        entry.set_key_value("device_tray_extruder_id", new ConfigOptionInt(tool));
        list.emplace(tray_id, std::move(entry));
    }

    const std::map<int, DynamicPrintConfig> ordered = PresetBundle::order_external_filaments_by_toolhead(std::move(list), 255, 4);

    check_filament_order(ordered, {external_test_colour(0), external_test_colour(1), external_test_colour(2), external_test_colour(3)});
    // Identity is preserved: every spool keeps the virtual tray id it came from.
    for (const auto &entry : ordered) {
        const int tool = entry.second.opt_int("device_tray_extruder_id");
        REQUIRE(tool >= 0);
        REQUIRE(tool < 4);
        CHECK(entry.second.opt_string("ams_id", 0u) == std::to_string(255 - tool));
    }
}

// The external spools follow every AMS tray, and the AMS trays keep their own order.
TEST_CASE("External spools follow the AMS trays", "[Preset][AMS]")
{
    std::map<int, DynamicPrintConfig> list;
    // Two four-lane generic AMS units: cumulative lane keys 0..7.
    for (int key = 0; key < 8; ++key)
        list.emplace(key, make_tray("", "PLA", ams_test_colour(key), std::to_string(key / 4), std::to_string(key % 4)));
    for (int tool = 0; tool < 4; ++tool) {
        const int          tray_id = 255 - tool;
        DynamicPrintConfig entry   = make_tray("", "PLA", external_test_colour(tool), std::to_string(tray_id), "0");
        entry.set_key_value("device_tray_extruder_id", new ConfigOptionInt(tool));
        list.emplace(tray_id, std::move(entry));
    }

    const std::map<int, DynamicPrintConfig> ordered = PresetBundle::order_external_filaments_by_toolhead(std::move(list), 255, 4);

    check_filament_order(ordered, {ams_test_colour(0), ams_test_colour(1), ams_test_colour(2), ams_test_colour(3), ams_test_colour(4),
                                   ams_test_colour(5), ams_test_colour(6), ams_test_colour(7), external_test_colour(0),
                                   external_test_colour(1), external_test_colour(2), external_test_colour(3)});
    // Every AMS tray kept its key, so the AMS unit order itself is untouched.
    for (int key = 0; key < 8; ++key) {
        const auto it = ordered.find(key);
        REQUIRE(it != ordered.end());
        CHECK(it->second.opt_string("filament_colour", 0u) == ams_test_colour(key));
    }
}

// A non-external key can be above the virtual tray ids. An external spool must still sort after
// every such key, and must not overwrite or drop an entry.
TEST_CASE("External spools stay after non-external keys above the virtual tray ids", "[Preset][AMS]")
{
    std::map<int, DynamicPrintConfig> list;
    // 300 non-external entries with keys 300..599, i.e. all above the virtual tray ids.
    for (int i = 0; i < 300; ++i)
        list.emplace(300 + i, make_tray("", "PLA", ams_test_colour(i), "0", std::to_string(i)));
    for (int tool = 0; tool < 3; ++tool) {
        const int          tray_id = 255 - tool;
        DynamicPrintConfig entry   = make_tray("", "PLA", external_test_colour(tool), std::to_string(tray_id), "0");
        entry.set_key_value("device_tray_extruder_id", new ConfigOptionInt(tool));
        list.emplace(tray_id, std::move(entry));
    }

    const std::map<int, DynamicPrintConfig> ordered = PresetBundle::order_external_filaments_by_toolhead(std::move(list), 255, 3);

    REQUIRE(ordered.size() == 303);
    std::vector<std::string> expected;
    // Every non-external entry keeps its key and its place in the iteration order.
    for (int i = 0; i < 300; ++i) {
        const auto it = ordered.find(300 + i);
        REQUIRE(it != ordered.end());
        expected.push_back(ams_test_colour(i));
    }
    expected.push_back(external_test_colour(0));
    expected.push_back(external_test_colour(1));
    expected.push_back(external_test_colour(2));
    check_filament_order(ordered, expected);
    // The spool keys lie above every non-external key, so none sorts before an AMS-style tray.
    CHECK(ordered.rbegin()->first > 599);
}

// load_ams_list() keeps a slot's refresh metadata by key. A spool that is absent on one refresh
// must leave its key unused, not move the keys of the spools after it; otherwise a spool could pick
// up the metadata of a differently-placed slot that uses the same preset.
TEST_CASE("A missing external spool does not shift the other toolhead keys", "[Preset][AMS]")
{
    const auto make_list = [](const std::vector<int> &tools) {
        std::map<int, DynamicPrintConfig> list;
        for (int tool : tools) {
            const int          tray_id = 255 - tool;
            DynamicPrintConfig entry   = make_tray("", "PLA", external_test_colour(tool), std::to_string(tray_id), "0");
            entry.set_key_value("device_tray_extruder_id", new ConfigOptionInt(tool));
            list.emplace(tray_id, std::move(entry));
        }
        return list;
    };
    const auto key_by_tool = [](const std::map<int, DynamicPrintConfig> &list) {
        std::map<int, int> keys;
        for (const auto &entry : list)
            keys[entry.second.opt_int("device_tray_extruder_id")] = entry.first;
        return keys;
    };

    // Tool 1 has no spool on the first refresh, then does on the second.
    const std::map<int, DynamicPrintConfig> with_missing_tool = PresetBundle::order_external_filaments_by_toolhead(make_list({0, 2, 3}), 255, 4);
    const std::map<int, DynamicPrintConfig> with_all_tools    = PresetBundle::order_external_filaments_by_toolhead(make_list({0, 1, 2, 3}), 255, 4);

    check_filament_order(with_missing_tool, {external_test_colour(0), external_test_colour(2), external_test_colour(3)});
    const std::map<int, int> missing_keys = key_by_tool(with_missing_tool);
    const std::map<int, int> all_keys     = key_by_tool(with_all_tools);
    for (int tool : {0, 2, 3}) {
        REQUIRE(missing_keys.count(tool) == 1);
        REQUIRE(all_keys.count(tool) == 1);
        CHECK(missing_keys.at(tool) == all_keys.at(tool));
    }
    // Tool 1's key was simply unused; it is the gap between tools 0 and 2.
    CHECK(all_keys.at(1) == all_keys.at(0) + 1);
    CHECK(all_keys.at(2) == all_keys.at(0) + 2);
}
