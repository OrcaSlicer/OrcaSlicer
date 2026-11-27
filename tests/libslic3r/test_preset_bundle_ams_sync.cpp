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
