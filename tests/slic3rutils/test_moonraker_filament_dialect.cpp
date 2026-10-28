#include <nlohmann/json.hpp>

#include <fstream>
#include <set>
#include <string>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <vector>
#include "slic3r/Utils/IPrinterAgent.hpp"

using Slic3r::IPrinterAgent;
using namespace Slic3r::MoonrakerFilamentDialect;

// Orca: g-code each Moonraker filament-changer dialect takes for slot writes and print-time maps.

namespace {

IPrinterAgent::FilamentSlotInfo red_pla(int slot, const std::string& name = "")
{
    IPrinterAgent::FilamentSlotInfo info;
    info.slot       = slot;
    info.name       = name;
    info.vendor     = "Polymaker";
    info.type       = "PLA";
    info.sub_type   = "PolyTerra";
    info.color_rgba = "FF0000FF";
    return info;
}

} // namespace

TEST_CASE("AFC has no combined command, so a slot write is two commands", "[MoonrakerFilamentDialect]")
{
    CHECK(afc_push_scripts(red_pla(2, "e1")) ==
          std::vector<std::string>{"SET_COLOR LANE=e1 COLOR=FF0000", "SET_MATERIAL LANE=e1 MATERIAL=PLA"});
}

TEST_CASE("An AFC write with no colour falls back to white", "[MoonrakerFilamentDialect]")
{
    IPrinterAgent::FilamentSlotInfo info = red_pla(0, "lane1");
    info.color_rgba.clear();
    CHECK(afc_push_scripts(info).front() == "SET_COLOR LANE=lane1 COLOR=FFFFFF");
}

TEST_CASE("Happy Hare writes a gate in one MMU_GATE_MAP command", "[MoonrakerFilamentDialect]")
{
    CHECK(happy_hare_push_script(red_pla(3)) == "MMU_GATE_MAP GATE=3 MATERIAL=PLA COLOR=FF0000");
}

TEST_CASE("Every changer dialect but none supports a push", "[MoonrakerFilamentDialect]")
{
    CHECK(dialect_supports_push(Dialect::afc_lane_data));
    CHECK(dialect_supports_push(Dialect::happy_hare));
    CHECK_FALSE(dialect_supports_push(Dialect::none));
    CHECK(dialect_name(Dialect::afc_lane_data) == "afc");
}

// Print-time mapping: tool_to_slot is a 1-based slot per 0-based tool, 0 = unassigned.

TEST_CASE("AFC start script resets the map, assigns used tools by lane name, then starts", "[MoonrakerFilamentDialect]")
{
    const std::vector<std::string> names = {"lane1", "lane2", "lane3"};
    CHECK(afc_mapping_start_script("cube.gcode", {3, 1, 0}, names) ==
          "RESET_AFC_MAPPING\n"
          "SET_MAP LANE=lane3 MAP=T0\n"
          "SET_MAP LANE=lane1 MAP=T1\n"
          "SDCARD_PRINT_FILE FILENAME=\"cube.gcode\"");
}

TEST_CASE("An AFC tool mapped to a slot with no lane name renders no script", "[MoonrakerFilamentDialect]")
{
    CHECK(afc_mapping_start_script("cube.gcode", {2}, {"lane1", ""}).empty());
    CHECK(afc_mapping_start_script("cube.gcode", {2}, {"lane1"}).empty());
}

TEST_CASE("Happy Hare start script resets the tool-to-gate map and assigns used tools", "[MoonrakerFilamentDialect]")
{
    CHECK(happy_hare_mapping_start_script("cube.gcode", {3, 1, 0}) ==
          "MMU_TTG_MAP RESET=1\n"
          "MMU_TTG_MAP TOOL=0 GATE=2\n"
          "MMU_TTG_MAP TOOL=1 GATE=0\n"
          "SDCARD_PRINT_FILE FILENAME=\"cube.gcode\"");
}

TEST_CASE("A reported changer renders the map in its own dialect on a plain Klipper printer", "[MoonrakerFilamentDialect]")
{
    using Slic3r::FilamentMappingProtocol;
    using Slic3r::MapDelivery;
    Slic3r::DevicePrintJobInfo job;
    job.filament_map_1based = {2, 1};
    job.slot_names          = {"lane1", "lane2"};
    CHECK(Slic3r::effective_map_delivery(FilamentMappingProtocol::fmpNone, "afc") == MapDelivery::afc);
    CHECK(Slic3r::build_device_start_script(FilamentMappingProtocol::fmpNone, MapDelivery::afc, "cube.gcode", job) ==
          "RESET_AFC_MAPPING\nSET_MAP LANE=lane2 MAP=T0\nSET_MAP LANE=lane1 MAP=T1\nSDCARD_PRINT_FILE FILENAME=\"cube.gcode\"");
    CHECK(Slic3r::build_device_start_script(FilamentMappingProtocol::fmpNone, MapDelivery::happy_hare, "cube.gcode", job) ==
          "MMU_TTG_MAP RESET=1\nMMU_TTG_MAP TOOL=0 GATE=1\nMMU_TTG_MAP TOOL=1 GATE=0\nSDCARD_PRINT_FILE FILENAME=\"cube.gcode\"");
    const Slic3r::DevicePrintSpec spec = Slic3r::device_print_spec(FilamentMappingProtocol::fmpNone, MapDelivery::afc);
    CHECK(spec.supports_filament_mapping);
    CHECK(spec.options.empty());
}

TEST_CASE("A plain Klipper printer with no changer has no delivery and no map script", "[MoonrakerFilamentDialect]")
{
    using Slic3r::FilamentMappingProtocol;
    using Slic3r::MapDelivery;
    CHECK(Slic3r::effective_map_delivery(FilamentMappingProtocol::fmpNone, "") == MapDelivery::none);
    CHECK_FALSE(Slic3r::device_print_spec(FilamentMappingProtocol::fmpNone, MapDelivery::none).supports_filament_mapping);
    Slic3r::DevicePrintJobInfo job;
    CHECK(Slic3r::build_device_start_script(FilamentMappingProtocol::fmpNone, MapDelivery::none, "a.gcode", job).empty());
    CHECK(Slic3r::effective_map_delivery(FilamentMappingProtocol::fmpSnapmaker, "") == MapDelivery::snapmaker);
    CHECK(Slic3r::effective_map_delivery(FilamentMappingProtocol::fmpWonderMaker, "afc") == MapDelivery::afc);
}

TEST_CASE("An AFC printer that registers openACE takes the map on the SD start at print time", "[MoonrakerFilamentDialect]")
{
    using Slic3r::MapDelivery;
    CHECK(Slic3r::print_time_map_delivery(MapDelivery::afc, true) == MapDelivery::openace);
    CHECK(Slic3r::print_time_map_delivery(MapDelivery::afc, false) == MapDelivery::afc);
    CHECK(Slic3r::print_time_map_delivery(MapDelivery::happy_hare, true) == MapDelivery::happy_hare);
    CHECK(Slic3r::print_time_map_delivery(MapDelivery::none, true) == MapDelivery::none);
}

TEST_CASE("openACE start script carries the map as OPENACE_MAP pairs on the SD start", "[MoonrakerFilamentDialect]")
{
    CHECK(openace_mapping_start_script("cube.gcode", {4, 0, 2}) ==
          "SDCARD_PRINT_FILE FILENAME=\"cube.gcode\" OPENACE_MAP=\"[[0,3],[2,1]]\"");
    CHECK(openace_mapping_start_script("cube.gcode", {}) == "SDCARD_PRINT_FILE FILENAME=\"cube.gcode\" OPENACE_MAP=\"[]\"");
}

TEST_CASE("openACE delivery renders its start line for a plain Klipper printer", "[MoonrakerFilamentDialect]")
{
    Slic3r::DevicePrintJobInfo job;
    job.filament_map_1based = {4};
    CHECK(Slic3r::build_device_start_script(Slic3r::FilamentMappingProtocol::fmpNone, Slic3r::MapDelivery::openace, "cube.gcode", job) ==
          "SDCARD_PRINT_FILE FILENAME=\"cube.gcode\" OPENACE_MAP=\"[[0,3]]\"");
}

// The U1 lists T4..T31 but not T0..T3, so the count is the highest index, not the entry count.
TEST_CASE("The tool count is the highest registered T command plus one", "[MoonrakerFilamentDialect]")
{
    nlohmann::json help = nlohmann::json::object();
    help["T4"]               = "G-code macro";
    help["T31"]              = "G-code macro";
    help["TEMPERATURE_WAIT"] = "Wait for a temperature";
    help["T1_ALIAS"]         = "";
    CHECK(tool_count_from_gcode_help(help) == 32);
    CHECK(tool_count_from_gcode_help(nlohmann::json::object()) == 0);
    CHECK(tool_count_from_gcode_help(nlohmann::json::array()) == 0);
    help.clear();
    help["T0"] = "Virtual tool T0";
    CHECK(tool_count_from_gcode_help(help) == 1);
}

TEST_CASE("Lane topology is read from the lane's own record", "[MoonrakerFilamentDialect]")
{
    CHECK(extruder_index_from_name("extruder") == 0);
    CHECK(extruder_index_from_name("extruder2") == 2);
    CHECK(extruder_index_from_name("") == -1);

    // A lane_data entry that carries its own topology.
    const nlohmann::json lane = nlohmann::json::parse(
        R"({"lane":"2","extruder_index":1,"unit":"Turtle_1","unit_name":"Left Turtle","slot":1,"head":"h1","extruder":"extruder1"})");
    Slic3r::MoonrakerAmsTrayData tray;
    apply_lane_topology(lane, tray);
    CHECK(tray.unit == "Turtle_1");
    CHECK(tray.unit_label == "Left Turtle");
    CHECK(tray.slot == 1);
    CHECK(tray.extruder == 1);
    CHECK(tray.virtual_tool == 2);
    CHECK(tray.head == "extruder1");
    // "map" is outside the schema and ignored.
    nlohmann::json with_map = lane;
    with_map["map"]         = "T7";
    Slic3r::MoonrakerAmsTrayData same;
    apply_lane_topology(with_map, same);
    CHECK(same.virtual_tool == 2);

    // AFC_stepper status: no unit_name, so the id is the label.
    const nlohmann::json afc = nlohmann::json::parse(R"({"name":"lane3","unit":"Turtle_1","extruder":"extruder","lane":3})");
    Slic3r::MoonrakerAmsTrayData afc_tray;
    apply_lane_topology(afc, afc_tray);
    CHECK(afc_tray.unit == "Turtle_1");
    CHECK(afc_tray.unit_label == "Turtle_1");
    CHECK(afc_tray.slot == 3);
    CHECK(afc_tray.extruder == 0);
    CHECK(afc_tray.virtual_tool == 3);
    CHECK(afc_tray.head == "extruder");

    // AFC's lane_data alone carries only the extruder index.
    const nlohmann::json afc_lane = nlohmann::json::parse(R"({"lane":3,"extruder_index":2})");
    Slic3r::MoonrakerAmsTrayData lane_only;
    apply_lane_topology(afc_lane, lane_only);
    CHECK(lane_only.extruder == 2);
    CHECK(lane_only.head == "extruder2");
    CHECK(lane_only.unit.empty());
    CHECK(lane_only.virtual_tool == 3);
}

TEST_CASE("An AFC_stepper record fills in what lane_data left out without clearing it", "[MoonrakerFilamentDialect]")
{
    Slic3r::MoonrakerAmsTrayData tray;
    apply_lane_topology(nlohmann::json::parse(R"({"lane":"4","unit":"Tools","slot":0,"extruder_index":1})"), tray);

    // The stepper carries no unit and no slot.
    apply_lane_topology(nlohmann::json::parse(R"({"name":"T4","extruder":"extruder1"})"), tray);
    CHECK(tray.unit == "Tools");
    CHECK(tray.unit_label == "Tools");
    CHECK(tray.slot == 0);
    CHECK(tray.extruder == 1);
    CHECK(tray.virtual_tool == 4);
    CHECK(tray.head == "extruder1");
}

TEST_CASE("Lane data that names each lane's unit and slot groups the lanes by unit", "[MoonrakerFilamentDialect]")
{
    std::ifstream        file(std::string(TEST_DATA_DIR) + "/moonraker_lane_data_units.json");
    const nlohmann::json lanes = nlohmann::json::parse(file)["result"]["value"];
    REQUIRE(lanes.size() == 23);
    CHECK(lane_data_has_topology(lanes));

    std::set<std::string> units;
    for (const auto& [key, lane] : lanes.items()) {
        Slic3r::MoonrakerAmsTrayData tray;
        REQUIRE(read_lane_data_entry(key, lane, tray));
        CHECK(key == "T" + std::to_string(tray.virtual_tool));
        CHECK(tray.slot_index == tray.virtual_tool);
        units.insert(tray.unit);
    }
    CHECK(units == std::set<std::string>{"AMS_1", "Ace2_1", "EMU_1", "HTLF_claymore_1", "Tools", "Turtle_1", "Vivid_1"});

    Slic3r::MoonrakerAmsTrayData tool;
    REQUIRE(read_lane_data_entry("T4", lanes["T4"], tool));
    CHECK(tool.unit == "Tools");
    CHECK(tool.slot == 0);
    CHECK(tool.extruder == 1);
    CHECK(tool.has_filament);
    CHECK(tool.tray_type == "ABS");

    Slic3r::MoonrakerAmsTrayData red;
    REQUIRE(read_lane_data_entry("T15", lanes["T15"], red));
    CHECK(red.unit == "AMS_1");
    CHECK(red.slot == 1);
    CHECK(red.virtual_tool == 15);
    CHECK(red.extruder == 2);
    CHECK(red.tray_color == "#FF0000");

    // Null and blank colour and material both read as an empty slot.
    for (const char* key : {"T11", "T12", "T16", "T19"}) {
        Slic3r::MoonrakerAmsTrayData empty;
        REQUIRE(read_lane_data_entry(key, lanes[key], empty));
        CAPTURE(key);
        CHECK_FALSE(empty.has_filament);
        CHECK(empty.tray_color.empty());
        CHECK(empty.bed_temp == 0);
    }
}

TEST_CASE("A lane without its own unit name takes the unit id as its label", "[MoonrakerFilamentDialect]")
{
    Slic3r::MoonrakerAmsTrayData tray;
    REQUIRE(read_lane_data_entry("lane1", nlohmann::json::parse(R"({"lane":"0","unit":"Turtle_1","slot":1})"), tray));
    CHECK(tray.unit_label == "Turtle_1");
}

TEST_CASE("A lane's string filament_id becomes the tray id and a numeric one is ignored", "[MoonrakerFilamentDialect]")
{
    Slic3r::MoonrakerAmsTrayData named;
    REQUIRE(read_lane_data_entry("T0", nlohmann::json::parse(R"({"lane":"0","material":"PLA","filament_id":"GFL99"})"), named));
    CHECK(named.tray_info_idx == "GFL99");

    // Happy Hare writes Spoolman's numeric filament record id under the same key.
    Slic3r::MoonrakerAmsTrayData spoolman;
    REQUIRE(read_lane_data_entry("T1", nlohmann::json::parse(R"({"lane":"1","material":"PLA","filament_id":12})"), spoolman));
    CHECK(spoolman.tray_info_idx.empty());
}

TEST_CASE("Lane data without a unit and slot on every lane asks the AFC steppers", "[MoonrakerFilamentDialect]")
{
    CHECK_FALSE(lane_data_has_topology(nlohmann::json::parse(R"({"lane1":{"lane":"0","unit":"Turtle_1","slot":1},"lane2":{"lane":"1"}})")));
    CHECK_FALSE(lane_data_has_topology(nlohmann::json::parse(R"({"lane1":{"lane":"0","unit":"Turtle_1","slot":"1"}})")));
    CHECK_FALSE(lane_data_has_topology(nlohmann::json::object()));
}
