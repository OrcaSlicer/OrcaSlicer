#include <catch2/catch_test_macros.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/WonderMakerPrinterAgent.hpp"
#include <string>
#include <vector>

using Slic3r::FilamentMappingProtocol;
using Slic3r::WonderMakerProtocol::build_start_script;

// Orca: ZR Ultra S start dialect, checked against hardware.
TEST_CASE("Renders the ZR Ultra S start script for a permuted two-tool plate", "[WonderMakerProtocol]")
{
    // Values on the wire are 0-based.
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
    const std::string script = build_start_script("a.gcode", {1}, /*bed_leveling*/ false, /*time_lapse*/ false);
    CHECK(script ==
          "_SET_TIMELAPSE_SETUP ENABLE=False\n"
          "G31\n"
          "SAVE_VARIABLE VARIABLE=box_modify_t0 VALUE=0\n"
          "SAVE_VARIABLE VARIABLE=box_modify_t0_backup VALUE=0\n"
          "SDCARD_PRINT_FILE FILENAME=\"a.gcode\"");
}

TEST_CASE("Timelapse on enables the component and clears stale hyperlapse", "[WonderMakerProtocol]")
{
    const std::string script = build_start_script("a.gcode", {1}, /*bed_leveling*/ true, /*time_lapse*/ true);
    CHECK(script.rfind("_SET_TIMELAPSE_SETUP ENABLE=True\nHYPERLAPSE ACTION=STOP\nG30\n", 0) == 0);
}

TEST_CASE("Leaves a tool the plate does not print untouched", "[WonderMakerProtocol]")
{
    const std::string script = build_start_script("a.gcode", {2, 0, 1}, /*bed_leveling*/ false, /*time_lapse*/ false);
    CHECK(script.find("box_modify_t1 ") == std::string::npos);
    CHECK(script.find("box_modify_t0 VALUE=1") != std::string::npos);
    CHECK(script.find("box_modify_t2 VALUE=0") != std::string::npos);
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

TEST_CASE("A ZR with openACE keeps its prelude and sends the map through openACE", "[WonderMakerProtocol]")
{
    Slic3r::DevicePrintJobInfo job;
    job.filament_map_1based = {4, 1};
    job.options             = { {"bed_leveling", "1"}, {"time_lapse", "0"} };
    // The sync reads openACE's lane data as AFC; the send finds its "openace" Klipper object.
    const auto delivery =
        Slic3r::print_time_map_delivery(Slic3r::effective_map_delivery(FilamentMappingProtocol::fmpWonderMaker, "afc"), true);
    CHECK(delivery == Slic3r::MapDelivery::openace);
    CHECK(Slic3r::build_device_start_script(FilamentMappingProtocol::fmpWonderMaker, delivery, "cube.gcode", job) ==
          "_SET_TIMELAPSE_SETUP ENABLE=False\n"
          "G30\n"
          "SDCARD_PRINT_FILE FILENAME=\"cube.gcode\" OPENACE_MAP=\"[[0,3],[1,0]]\"");
    CHECK(Slic3r::device_print_spec(FilamentMappingProtocol::fmpWonderMaker, delivery).supports_filament_mapping);
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
