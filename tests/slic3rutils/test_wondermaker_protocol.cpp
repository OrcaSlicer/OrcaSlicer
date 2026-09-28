#include <catch2/catch_all.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/WonderMakerPrinterAgent.hpp"

using Slic3r::DevicePrintJobInfo;
using Slic3r::FilamentMappingProtocol;
using Slic3r::WonderMakerProtocol::build_start_script;

// Orca: pins the ZR Ultra S start dialect against the 2026-08-10 hardware session, where a print
// started with logical T0 mapped to box 2 left box_modify_t0 = 0 / box_modify_t0_backup = 2 (the
// live variable consumed on first tool selection, the backup durable) and the untouched tools'
// variables unchanged. Both forms are therefore written for every tool the plate prints.

TEST_CASE("Renders the ZR Ultra S start script for a permuted two-tool plate", "[WonderMakerProtocol]")
{
    // Two dense tools: T0 pulls from box 3, T1 from box 1. Values on the wire are 0-based.
    const std::vector<int> box_of_tool_1based = {3, 1};
    CHECK(build_start_script("cube.gcode", box_of_tool_1based, /*bed_leveling*/ true, /*time_lapse*/ false) ==
          "_SET_TIMELAPSE_SETUP ENABLE=False\n"
          "G30\n"
          "SAVE_VARIABLE VARIABLE=box_modify_t0 VALUE=2\n"
          "SAVE_VARIABLE VARIABLE=box_modify_t0_backup VALUE=2\n"
          "SAVE_VARIABLE VARIABLE=box_modify_t1 VALUE=0\n"
          "SAVE_VARIABLE VARIABLE=box_modify_t1_backup VALUE=0\n"
          "SDCARD_PRINT_FILE FILENAME=\"cube.gcode\"");
}

TEST_CASE("Leveling off emits the explicit load-saved-mesh command", "[WonderMakerProtocol]")
{
    // G31 loads the saved default mesh. Explicit, never absent: adaptive_mesh_enable is sticky
    // printer state, so emitting nothing would inherit whatever the previous print chose
    // (captured from the vendor touchscreen's own start sequence, 2026-08-18).
    const std::string script = build_start_script("a.gcode", {1}, /*bed_leveling*/ false, /*time_lapse*/ false);
    CHECK(script.find("G30") == std::string::npos);
    CHECK(script ==
          "_SET_TIMELAPSE_SETUP ENABLE=False\n"
          "G31\n"
          "SAVE_VARIABLE VARIABLE=box_modify_t0 VALUE=0\n"
          "SAVE_VARIABLE VARIABLE=box_modify_t0_backup VALUE=0\n"
          "SDCARD_PRINT_FILE FILENAME=\"a.gcode\"");
}

TEST_CASE("Timelapse on enables the component and clears stale hyperlapse", "[WonderMakerProtocol]")
{
    // Mirrors the touchscreen's own start order: setup, hyperlapse clear, leveling, start.
    const std::string script = build_start_script("a.gcode", {1}, /*bed_leveling*/ true, /*time_lapse*/ true);
    CHECK(script.rfind("_SET_TIMELAPSE_SETUP ENABLE=True\nHYPERLAPSE ACTION=STOP\nG30\n", 0) == 0);
}

TEST_CASE("Leaves a tool the plate does not print untouched", "[WonderMakerProtocol]")
{
    // A zero carries no pick. Writing box_modify_t1 anyway would overwrite whatever the printer's
    // own screen last set for a tool this print never selects.
    const std::string script = build_start_script("a.gcode", {2, 0, 1}, /*bed_leveling*/ false, /*time_lapse*/ false);
    CHECK(script.find("box_modify_t1 ") == std::string::npos);
    CHECK(script.find("box_modify_t0 VALUE=1") != std::string::npos);
    CHECK(script.find("box_modify_t2 VALUE=0") != std::string::npos);
}

TEST_CASE("The job form takes its leveling choice from the send dialog", "[WonderMakerProtocol]")
{
    DevicePrintJobInfo job;
    job.filament_map_1based = {1, 2};
    job.options["bed_leveling"] = "0";
    const auto render = [&job]() {
        return Slic3r::build_device_start_script(FilamentMappingProtocol::fmpWonderMaker, Slic3r::MapDelivery::wondermaker, "a.gcode", job);
    };
    CHECK(render().find("G30") == std::string::npos);
    job.options["bed_leveling"] = "1";
    CHECK(render().find("\nG30\n") != std::string::npos);
}

TEST_CASE("The ZR Ultra S's namespace is its tool count; the U1's is its 32-entry table", "[WonderMakerProtocol]")
{
    // What separates the two: the ZR only permutes its tools and cannot address a tool number
    // past its last one, so a plate reaching past T4 is renumbered (FilamentCompaction); the U1
    // addresses 32 as-is. Both are the same rule with a different namespace size.
    Slic3r::DynamicPrintConfig zr = Slic3r::DynamicPrintConfig::full_print_config();
    zr.set_deserialize_strict({ { "filament_mapping_protocol", "wondermaker" } });
    CHECK(Slic3r::filament_namespace_size(zr, 4) == 4);
    Slic3r::DynamicPrintConfig u1 = Slic3r::DynamicPrintConfig::full_print_config();
    u1.set_deserialize_strict({ { "filament_mapping_protocol", "snapmaker" } });
    CHECK(Slic3r::filament_namespace_size(u1, 4) == 32);
}

TEST_CASE("Declares only the options the ZR firmware implements", "[WonderMakerProtocol]")
{
    const Slic3r::DevicePrintSpec spec = Slic3r::device_print_spec(FilamentMappingProtocol::fmpWonderMaker, Slic3r::MapDelivery::wondermaker);
    CHECK(spec.supports_filament_mapping);
    REQUIRE(spec.options.size() == 2);
    CHECK(spec.options.front().key == "time_lapse");
    CHECK(spec.options.front().default_value == "0");
    CHECK(spec.options.back().key == "bed_leveling");
    CHECK(spec.options.back().default_value == "1");
}

// With openACE installed the ZR keeps its own prelude (the options its screen offers) and
// hands the map to openACE: vendor options are the adapter's, delivery is the changer's.
TEST_CASE("A ZR with openACE keeps its prelude and sends the map through openACE", "[WonderMakerProtocol]")
{
    Slic3r::DevicePrintJobInfo job;
    job.filament_map_1based = {4, 1};
    job.options             = { {"bed_leveling", "1"}, {"time_lapse", "0"} };
    const auto delivery = Slic3r::effective_map_delivery(FilamentMappingProtocol::fmpWonderMaker, "openace");
    CHECK(delivery == Slic3r::MapDelivery::openace);
    CHECK(Slic3r::build_device_start_script(FilamentMappingProtocol::fmpWonderMaker, delivery, "cube.gcode", job) ==
          "_SET_TIMELAPSE_SETUP ENABLE=False\n"
          "G30\n"
          "SDCARD_PRINT_FILE FILENAME=\"cube.gcode\" OPENACE_MAP=\"[[0,3],[1,0]]\"");
    // The options survive the changer: the print dialog still offers them.
    const auto spec = Slic3r::device_print_spec(FilamentMappingProtocol::fmpWonderMaker, delivery);
    CHECK(spec.supports_filament_mapping);
    REQUIRE(spec.options.size() == 2);
    CHECK(spec.options[0].key == "time_lapse");
    CHECK(spec.options[1].key == "bed_leveling");
}

TEST_CASE("A stock ZR still maps through box_modify after its prelude", "[WonderMakerProtocol]")
{
    Slic3r::DevicePrintJobInfo job;
    job.filament_map_1based = {2};
    job.options             = { {"bed_leveling", "0"}, {"time_lapse", "0"} };
    const auto delivery = Slic3r::effective_map_delivery(FilamentMappingProtocol::fmpWonderMaker, "");
    CHECK(delivery == Slic3r::MapDelivery::wondermaker);
    CHECK(Slic3r::build_device_start_script(FilamentMappingProtocol::fmpWonderMaker, delivery, "a.gcode", job) ==
          "_SET_TIMELAPSE_SETUP ENABLE=False\nG31\n"
          "SAVE_VARIABLE VARIABLE=box_modify_t0 VALUE=1\n"
          "SAVE_VARIABLE VARIABLE=box_modify_t0_backup VALUE=1\n"
          "SDCARD_PRINT_FILE FILENAME=\"a.gcode\"");
}
