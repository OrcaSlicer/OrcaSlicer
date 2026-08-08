#include <catch2/catch_all.hpp>

#include <boost/algorithm/string/replace.hpp>

#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/SnapmakerPrinterAgent.hpp"

using Slic3r::PRINT_HOST_UPLOADED_FILENAME_PLACEHOLDER;
using Slic3r::SnapmakerProtocol::build_start_script;
using Slic3r::SnapmakerProtocol::render_map_table;

// Orca: pins the device-protocol MAP_TABLE wire format render_map_table produces -- 0-based
// [logical filament index, physical tool index] pairs, ", "-separated, over the full project
// filament list. Both the AGENT send path (SnapmakerPrinterAgent::build_start_print_gcode) and
// the legacy PrintHost path (Plater::send_gcode_legacy, via build_start_script) render the
// mapping through this exact function, so an unnoticed format change here would silently break
// real toolchangers.

TEST_CASE("Renders the captured Snapmaker U1 MAP_TABLE reference", "[SnapmakerProtocol]")
{
    // Captured from the real Snapmaker U1 touchscreen: MAP_TABLE="[[0, 3], [1, 2], [2, 0], [3, 1], [4, 1]]"
    // -- 5 logical filaments onto 4 physical tools, logicals 3 and 4 both merged onto tool 1.
    // render_map_table takes filament_map_1based[i] - 1 as the rendered tool index, so the
    // 1-based input is derived by adding 1 back to each captured 0-based tool: f1->4, f2->3,
    // f3->1, f4->2, f5->2.
    const std::vector<int> filament_map_1based = {4, 3, 1, 2, 2};
    CHECK(render_map_table(filament_map_1based) == "[[0, 3], [1, 2], [2, 0], [3, 1], [4, 1]]");
}

TEST_CASE("Renders an identity map with no merges", "[SnapmakerProtocol]")
{
    const std::vector<int> filament_map_1based = {1, 2, 3};
    CHECK(render_map_table(filament_map_1based) == "[[0, 0], [1, 1], [2, 2]]");
}

TEST_CASE("Preserves a repeated tool index when two filaments merge onto one tool", "[SnapmakerProtocol]")
{
    // Filaments 1 and 2 both map to tool 1: the renderer must repeat the physical value, not
    // deduplicate it -- the wire format is a per-filament table, not a set of distinct tools.
    const std::vector<int> filament_map_1based = {1, 1, 2};
    CHECK(render_map_table(filament_map_1based) == "[[0, 0], [1, 0], [2, 1]]");
}

TEST_CASE("Renders a single filament map", "[SnapmakerProtocol]")
{
    const std::vector<int> filament_map_1based = {1};
    CHECK(render_map_table(filament_map_1based) == "[[0, 0]]");
}

TEST_CASE("Omits unassigned filaments and keeps the remaining logical indices", "[SnapmakerProtocol]")
{
    // 0 means "leave unassigned" (IPrinterAgent.hpp). A naive filament_map_1based[i] - 1 would
    // render this as tool index -1; the entry must be dropped instead, and the surviving entries
    // must still carry their own (non-contiguous) logical filament index.
    const std::vector<int> filament_map_1based = {1, 0, 3, 0, 2};
    CHECK(render_map_table(filament_map_1based) == "[[0, 0], [2, 2], [4, 1]]");
}

TEST_CASE("Renders an empty table when no filament is assigned", "[SnapmakerProtocol]")
{
    const std::vector<int> filament_map_1based = {0, 0, 0};
    CHECK(render_map_table(filament_map_1based) == "[]");
}

// Orca: pins the print-host path's two-step start-script construction -- the GUI caller builds
// the script with PRINT_HOST_UPLOADED_FILENAME_PLACEHOLDER standing in for a filename that isn't
// known yet (the upload hasn't happened), and Moonraker::upload() later substitutes the
// server-confirmed name for that token once the upload response confirms it (see
// Moonraker.cpp's use of boost::replace_first). This test stands in for that substitution without
// spinning up an HTTP server, and checks the end result still matches the captured hardware
// reference exactly.
TEST_CASE("Placeholder-rendered start script substitutes to the captured Snapmaker U1 reference", "[SnapmakerProtocol]")
{
    const std::vector<int> filament_map_1based = {4, 3, 1, 2, 2};

    std::string script = build_start_script(PRINT_HOST_UPLOADED_FILENAME_PLACEHOLDER, filament_map_1based);
    // Confirm the placeholder round-trips through the builder untouched (i.e. build_start_script
    // itself doesn't attempt any filename validation/escaping that would corrupt the token).
    CHECK(script.find(PRINT_HOST_UPLOADED_FILENAME_PLACEHOLDER) != std::string::npos);

    boost::replace_first(script, PRINT_HOST_UPLOADED_FILENAME_PLACEHOLDER, "Cube_U1_filamap_sm_PLA_31m18s.gcode");
    // BED_LEVEL="1" is ours, not part of the captured touchscreen reference: the firmware reads an
    // absent parameter as off, so omitting it silently disabled leveling on every Orca-initiated
    // print. See build_start_script for the hardware evidence.
    CHECK(script ==
          "SDCARD_PRINT_FILE_WITH_PARAMETERS FILENAME=\"Cube_U1_filamap_sm_PLA_31m18s.gcode\" "
          "MAP_TABLE=\"[[0, 3], [1, 2], [2, 0], [3, 1], [4, 1]]\" BED_LEVEL=\"1\"");
}
