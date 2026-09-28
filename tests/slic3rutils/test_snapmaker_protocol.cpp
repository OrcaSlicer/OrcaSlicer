#include <catch2/catch_all.hpp>

#include <boost/algorithm/string/replace.hpp>

#include "slic3r/Utils/DeviceJson.hpp"
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

// The reference below is a VERBATIM capture of the printer's own touchscreen starting a print
// (2026-08-10, stock firmware, five project filaments on four tools, all three options on and all
// four tools in use). Our start script must be byte-compatible with it: the firmware indexes the
// per-LOGICAL-filament arrays (NOZZLE_TEMP, FILAMENT_*, *_USED_*) and the per-PHYSICAL-tool arrays
// (NOZZLE_DIAMETER_LIST, FLOW_CALIBRATE_EXTRUDERS, END_UNLOAD_FILAMENT) differently, so a length
// or ordering mistake is silently wrong on the machine rather than an error.
static Slic3r::DevicePrintJobInfo capture_job()
{
    Slic3r::DevicePrintJobInfo job;
    job.filament_map_1based = { 1, 2, 3, 4, 1 };            // -> [[0,0],[1,1],[2,2],[3,3],[4,0]]
    job.filament_type       = { "PLA", "PLA", "PLA", "PLA", "PLA" };
    job.nozzle_temp         = { 220.0, 220.0, 220.0, 220.0, 220.0 };
    job.flow_ratio          = { 0.98, 0.98, 0.98, 0.966, 0.966 };
    job.filament_diameter   = { 1.75, 1.75, 1.75, 1.75, 1.75 };
    job.used_g              = { 2.15, 0.63, 1.14, 1.72, 1.87 };
    job.used_mm             = { 720.48, 191.14, 346.38, 575.94, 626.28 };
    job.nozzle_diameter     = { 0.4, 0.4, 0.4, 0.4 };
    job.used_physical_tools = { 0, 1, 2, 3 };
    job.line_width          = 0.42;
    job.layer_height        = 0.2;
    job.outer_wall_speed    = 200.0;
    job.options             = { {"bed_leveling", "1"}, {"flow_calibrate", "1"}, {"time_lapse", "1"} };
    return job;
}

TEST_CASE("The start script matches a screen-initiated start byte for byte", "[SnapmakerProtocol]")
{
    const std::string expected =
        "SDCARD_PRINT_FILE_WITH_PARAMETERS FILENAME=\"Cube_PLA_27m42s.gcode\""
        " MAP_TABLE=\"[[0, 0], [1, 1], [2, 2], [3, 3], [4, 0]]\""
        " BED_LEVEL=\"1\" TIME_LAPSE_CAMERA=\"1\""
        " FLOW_CALIBRATE=\"1\" FLOW_CALIBRATE_EXTRUDERS=\"[0, 1, 2, 3]\""
        " END_UNLOAD_FILAMENT=\"[0, 0, 0, 0]\""
        " LINE_WIDTH=\"0.42\" LAYER_HEIGHT=\"0.2\" OUTER_WALL_SPEED=\"200.0\""
        " NOZZLE_DIAMETER_LIST=\"[0.4, 0.4, 0.4, 0.4]\""
        " NOZZLE_TEMP=\"[220.0, 220.0, 220.0, 220.0, 220.0]\""
        " FILAMENT_TYPE=\"['PLA', 'PLA', 'PLA', 'PLA', 'PLA']\""
        " FILAMENT_FLOW_RATIO=\"[0.98, 0.98, 0.98, 0.966, 0.966]\""
        " FILAMENT_DIAMETER=\"[1.75, 1.75, 1.75, 1.75, 1.75]\""
        " FILAMENT_USED_G=\"[2.15, 0.63, 1.14, 1.72, 1.87]\""
        " FILAMENT_USED_MM=\"[720.48, 191.14, 346.38, 575.94, 626.28]\"";
    CHECK(Slic3r::SnapmakerProtocol::build_start_script("Cube_PLA_27m42s.gcode", capture_job()) == expected);
}

TEST_CASE("Unchecked options are stated as off, never omitted", "[SnapmakerProtocol]")
{
    // The firmware reads an ABSENT parameter as OFF, so "off" must still be transmitted -- that is
    // what makes an unchecked box distinguishable from an Orca build that doesn't know the option.
    Slic3r::DevicePrintJobInfo job = capture_job();
    job.options = { {"bed_leveling", "0"}, {"flow_calibrate", "0"}, {"time_lapse", "0"} };
    const std::string script = Slic3r::SnapmakerProtocol::build_start_script("f.gcode", job);
    CHECK(script.find("BED_LEVEL=\"0\"") != std::string::npos);
    CHECK(script.find("TIME_LAPSE_CAMERA=\"0\"") != std::string::npos);
    CHECK(script.find("FLOW_CALIBRATE=\"0\"") != std::string::npos);
    // With calibration off no tool is listed, but the parameter is still present.
    CHECK(script.find("FLOW_CALIBRATE_EXTRUDERS=\"[]\"") != std::string::npos);
}

TEST_CASE("Flow calibration covers only the tools the plate uses", "[SnapmakerProtocol]")
{
    // One checkbox in the UI; the heads are derived. A tool that prints nothing must not be
    // calibrated -- that would spend minutes of purge on filament this plate never touches.
    Slic3r::DevicePrintJobInfo job = capture_job();
    job.used_physical_tools = { 0, 2 };
    const std::string script = Slic3r::SnapmakerProtocol::build_start_script("f.gcode", job);
    CHECK(script.find("FLOW_CALIBRATE_EXTRUDERS=\"[0, 2]\"") != std::string::npos);
}

// Orca: the printer's filament_detect reply is external input, and its shape depends on what is
// physically loaded. A U1 lane holding an NFC-tagged spool reports CARD_UID as an ARRAY of tag
// bytes; a lane without one reports the NUMBER 0. Reading it as a number only threw
// type_error.302 out of the agent and aborted the slicer as soon as a tagged spool was present.
// Payloads below are the capture from the report:
// https://github.com/OrcaSlicer/OrcaSlicer/pull/15145#issuecomment-5292008500

TEST_CASE("Reads a CARD_UID reported as an array of tag bytes", "[SnapmakerProtocol]")
{
    const auto slot = nlohmann::json::parse(R"({
        "VERSION": 1, "VENDOR": "ELEGOO", "MANUFACTURER": "ELEGOO", "MAIN_TYPE": "PLA",
        "SUB_TYPE": "Translucent", "BED_TEMP": 60, "FIRST_LAYER_TEMP": 210,
        "OTHER_LAYER_TEMP": 210, "HOTEND_MAX_TEMP": 220, "HOTEND_MIN_TEMP": 210,
        "ARGB_COLOR": 4293519847, "DIAMETER": 175, "OFFICIAL": true,
        "CARD_UID": [4, 214, 239, 50, 197, 42, 129], "SPOOL_ID": "132"})");

    // Each byte becomes two hex digits, left-padded to the 16-digit tag-id convention.
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(slot) == "0004D6EF32C52A81");
    CHECK(Slic3r::json_number_or(slot, "BED_TEMP", 0) == 60);
    CHECK(Slic3r::json_number_or(slot, "FIRST_LAYER_TEMP", 0) == 210);
    CHECK(Slic3r::json_string_or(slot, "VENDOR", "NONE") == "ELEGOO");
}

TEST_CASE("A lane with no tag reports no tag id", "[SnapmakerProtocol]")
{
    // CARD_UID 0 is "no tag": the empty string is what consumers test to tell a tag-backed slot
    // from a manually configured one, so an all-zero id must not read as a real tag.
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(nlohmann::json::parse(R"({"VENDOR":"NONE","CARD_UID":0})")).empty());
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(nlohmann::json::parse(R"({"CARD_UID":[0,0,0]})")).empty());
    // Absent, or a shape we have never seen, is also "no tag" rather than a crash.
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(nlohmann::json::parse(R"({"VENDOR":"NONE"})")).empty());
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(nlohmann::json::parse(R"({"CARD_UID":"04D6EF"})")).empty());
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(nlohmann::json::parse(R"({"CARD_UID":[4,"x"]})")).empty());
}

TEST_CASE("A numeric CARD_UID still renders as a 16-digit tag id", "[SnapmakerProtocol]")
{
    // The shape the agent originally assumed; firmware that reports it must keep working.
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(nlohmann::json::parse(R"({"CARD_UID":81985529216486895})")) ==
          "0123456789ABCDEF");
}

TEST_CASE("A field of the wrong type falls back instead of throwing", "[SnapmakerProtocol]")
{
    // value(key, fallback) falls back only when the key is ABSENT -- present-but-wrong-type
    // converts, and throws. Every read of printer JSON has to check the type first.
    const auto slot = nlohmann::json::parse(R"({"BED_TEMP":"60","FIRST_LAYER_TEMP":null,"VENDOR":7})");
    CHECK(Slic3r::json_number_or(slot, "BED_TEMP", 0) == 0);
    CHECK(Slic3r::json_number_or(slot, "FIRST_LAYER_TEMP", 7) == 7);
    CHECK(Slic3r::json_number_or(slot, "MISSING", 42) == 42);
    CHECK(Slic3r::json_string_or(slot, "VENDOR", "NONE") == "NONE");
}
