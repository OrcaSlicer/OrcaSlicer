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
    CHECK(build_start_script("cube.gcode", box_of_tool_1based, /*bed_leveling*/ true) ==
          "G30\n"
          "SAVE_VARIABLE VARIABLE=box_modify_t0 VALUE=2\n"
          "SAVE_VARIABLE VARIABLE=box_modify_t0_backup VALUE=2\n"
          "SAVE_VARIABLE VARIABLE=box_modify_t1 VALUE=0\n"
          "SAVE_VARIABLE VARIABLE=box_modify_t1_backup VALUE=0\n"
          "SDCARD_PRINT_FILE FILENAME=\"cube.gcode\"");
}

TEST_CASE("Omits the probe when auto leveling is off", "[WonderMakerProtocol]")
{
    // Absent means "don't probe": the firmware has no command for disabling it, so the off case
    // must emit nothing rather than something.
    const std::string script = build_start_script("a.gcode", {1}, /*bed_leveling*/ false);
    CHECK(script.find("G30") == std::string::npos);
    CHECK(script ==
          "SAVE_VARIABLE VARIABLE=box_modify_t0 VALUE=0\n"
          "SAVE_VARIABLE VARIABLE=box_modify_t0_backup VALUE=0\n"
          "SDCARD_PRINT_FILE FILENAME=\"a.gcode\"");
}

TEST_CASE("Leaves a tool the plate does not print untouched", "[WonderMakerProtocol]")
{
    // A zero carries no pick. Writing box_modify_t1 anyway would overwrite whatever the printer's
    // own screen last set for a tool this print never selects.
    const std::string script = build_start_script("a.gcode", {2, 0, 1}, /*bed_leveling*/ false);
    CHECK(script.find("box_modify_t1 ") == std::string::npos);
    CHECK(script.find("box_modify_t0 VALUE=1") != std::string::npos);
    CHECK(script.find("box_modify_t2 VALUE=0") != std::string::npos);
}

TEST_CASE("The job form takes its leveling choice from the send dialog", "[WonderMakerProtocol]")
{
    DevicePrintJobInfo job;
    job.filament_map_1based = {1, 2};
    job.options["bed_leveling"] = "0";
    CHECK(build_start_script("a.gcode", job).find("G30") == std::string::npos);
    job.options["bed_leveling"] = "1";
    CHECK(build_start_script("a.gcode", job).rfind("G30\n", 0) == 0);
}

TEST_CASE("The ZR Ultra S declares permutation-only routing and dense tool numbering", "[WonderMakerProtocol]")
{
    // The three capabilities are independent, and this printer is the one that separates them:
    // it resolves the mapping itself (like the U1) but only permutes its tools and cannot address
    // a tool number past its last one (unlike the U1's 32-entry table).
    CHECK(Slic3r::protocol_max_plate_filaments(FilamentMappingProtocol::fmpWonderMaker, 4) == 4);
    CHECK(Slic3r::protocol_requires_dense_tool_numbering(FilamentMappingProtocol::fmpWonderMaker));
    // The U1's wire format is indexed BY the project slot, so it must NOT be renumbered.
    CHECK(Slic3r::protocol_max_plate_filaments(FilamentMappingProtocol::fmpSnapmaker, 4) == 32);
    CHECK(!Slic3r::protocol_requires_dense_tool_numbering(FilamentMappingProtocol::fmpSnapmaker));
    CHECK(!Slic3r::protocol_requires_dense_tool_numbering(FilamentMappingProtocol::fmpNone));
}

TEST_CASE("Declares only the options the ZR firmware implements", "[WonderMakerProtocol]")
{
    const Slic3r::DevicePrintSpec spec = Slic3r::device_print_spec(FilamentMappingProtocol::fmpWonderMaker);
    CHECK(spec.supports_filament_mapping);
    REQUIRE(spec.options.size() == 1);
    CHECK(spec.options.front().key == "bed_leveling");
    CHECK(spec.options.front().default_value == "1");
}
