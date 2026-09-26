#include <catch2/catch_all.hpp>

#include <nlohmann/json.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"
#include "slic3r/Utils/PrintHost.hpp"

using Slic3r::IPrinterAgent;
using namespace Slic3r::MoonrakerFilamentDialect;

// Orca: the materials dialog writes a slot back through IPrinterAgent::push_filament_info. On a
// Moonraker printer the dialect is whichever filament changer reported the slots: AFC (the
// lane_data namespace) or Happy Hare (the mmu object). Both are plain g-code commands, so the
// rendering is pinned here as strings, like the Snapmaker and WonderMaker start scripts.

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
    // Colour goes without alpha (AFC stores RRGGBB); vendor and sub-type have no field in AFC
    // and are not sent.
    // The lane is addressed by the name the slot came with (AmsTrayData::slot_name), not by index.
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
    // The gate is the slot index; MMU_GATE_MAP takes material and colour together.
    CHECK(happy_hare_push_script(red_pla(3)) == "MMU_GATE_MAP GATE=3 MATERIAL=PLA COLOR=FF0000");
}

TEST_CASE("Every changer dialect but none supports a push", "[MoonrakerFilamentDialect]")
{
    CHECK(dialect_supports_push(Dialect::afc_lane_data));
    CHECK(dialect_supports_push(Dialect::happy_hare));
    // openACE adopted AFC's lane commands, keyed by the lane_data key it publishes.
    CHECK(dialect_supports_push(Dialect::openace));
    CHECK_FALSE(dialect_supports_push(Dialect::none));
    CHECK(dialect_from_name(dialect_name(Dialect::openace)) == Dialect::openace);
    CHECK(dialect_name(Dialect::openace) == "openace");
}

// Print-time mapping. The map is reset first so the result does not depend on what the printer's
// own screen left behind, then each USED tool is assigned in ascending order (AFC's SET_MAP is a
// swap; ascending assignment settles), then the plain SD start the Moonraker agent always sends.
// tool_to_slot is the dialog's shape: 1-based slot per 0-based tool, 0 = unassigned.

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
    // An empty script is what the send path treats as "the map never reached the printer" and
    // refuses to auto-start on; better than a SET_MAP naming nothing.
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

// A plain Klipper printer declares no protocol; the changer it reported IS the delivery, and the
// vendor prelude is empty, so the script is the changer's map lines and the SD start.
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
    // The standard device dialog with a mapping and no printer-specific options.
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
    // A vendor protocol is its own delivery until a changer is reported.
    CHECK(Slic3r::effective_map_delivery(FilamentMappingProtocol::fmpSnapmaker, "") == MapDelivery::snapmaker);
    CHECK(Slic3r::effective_map_delivery(FilamentMappingProtocol::fmpWonderMaker, "openace") == MapDelivery::openace);
}

// openACE takes the per-print map as one parameter on the SD start: pairs of [sliced tool,
// openACE virtual tool], both 0-based, no spaces. Omitted tools fall back to the printer's
// default map, so only assigned tools are listed; an empty map is "[]". The job is frozen at
// start and discarded at its end, so there is nothing to reset first.
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

// The printer's logical tool count is the highest T<n> it registers as a g-code command, plus
// one -- read from /printer/gcode/help. The Snapmaker U1's toolchanger registers T0..T3 without
// help text (they are absent from the listing) while its T4..T31 macros are listed, so the
// count is the highest index, not the number of entries. Anything that is not exactly T<digits>
// (TEMPERATURE_WAIT, T1_ALIAS) is ignored; no T commands at all is 0, "not probed".
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
    help["T0"] = "openACE virtual tool T0";
    CHECK(tool_count_from_gcode_help(help) == 1);
}

// Where a slot sits and what it feeds, from the lane's own record. openACE's lane_data entry
// carries all of it; AFC's lane_data does not, so its AFC_stepper status object is used instead
// (same reader, the keys line up). The lane's index is its virtual tool on both.
TEST_CASE("Lane topology is read from the lane's own record", "[MoonrakerFilamentDialect]")
{
    CHECK(extruder_index_from_name("extruder") == 0);
    CHECK(extruder_index_from_name("extruder2") == 2);
    CHECK(extruder_index_from_name("") == -1);

    // openACE lane_data entry: "lane" IS the virtual tool. Units are grouped by their stable id
    // and titled with the user's name for the unit when the record offers one (openACE's
    // unit_name; not part of AFC's schema), so renaming an ACE neither splits nor merges its
    // lanes.
    const nlohmann::json lane = nlohmann::json::parse(
        R"({"lane":"2","extruder_index":1,"unit":"ace0","unit_name":"Left ACE","slot":1,"head":"h1","extruder":"extruder1"})");
    Slic3r::MoonrakerAmsTrayData tray;
    apply_lane_topology(lane, tray);
    CHECK(tray.unit == "ace0");
    CHECK(tray.unit_label == "Left ACE");
    CHECK(tray.slot == 1);
    CHECK(tray.extruder == 1);
    CHECK(tray.virtual_tool == 2);
    CHECK(tray.head == "extruder1");
    // Anything outside the schema ("map", once published by both changers) is ignored.
    nlohmann::json with_map = lane;
    with_map["map"]         = "T7";
    Slic3r::MoonrakerAmsTrayData same;
    apply_lane_topology(with_map, same);
    CHECK(same.virtual_tool == 2);

    // AFC_stepper status: no unit_name (the id doubles as the title), the extruder by name, the
    // lane index as both the position and the tool.
    const nlohmann::json afc = nlohmann::json::parse(R"({"name":"lane3","unit":"Turtle_1","extruder":"extruder","lane":3})");
    Slic3r::MoonrakerAmsTrayData afc_tray;
    apply_lane_topology(afc, afc_tray);
    CHECK(afc_tray.unit == "Turtle_1");
    CHECK(afc_tray.unit_label == "Turtle_1");
    CHECK(afc_tray.slot == 3);
    CHECK(afc_tray.extruder == 0);
    CHECK(afc_tray.virtual_tool == 3);
    CHECK(afc_tray.head == "extruder");

    // AFC's lane_data alone: only the extruder index, from which Klipper's name follows, and the
    // lane index as tool and position until AFC publishes unit and slot.
    const nlohmann::json afc_lane = nlohmann::json::parse(R"({"lane":3,"extruder_index":2})");
    Slic3r::MoonrakerAmsTrayData lane_only;
    apply_lane_topology(afc_lane, lane_only);
    CHECK(lane_only.extruder == 2);
    CHECK(lane_only.head == "extruder2");
    CHECK(lane_only.unit.empty());
    CHECK(lane_only.virtual_tool == 3);
}
