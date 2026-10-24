#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <boost/algorithm/string/replace.hpp>
#include <vector>
#include <string>

#include "slic3r/Utils/DeviceJson.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/SnapmakerPrinterAgent.hpp"

using Slic3r::PRINT_HOST_UPLOADED_FILENAME_PLACEHOLDER;
using Slic3r::SnapmakerProtocol::build_start_script;
using Slic3r::SnapmakerProtocol::render_map_table;

TEST_CASE("Renders MAP_TABLE as 0-based [filament, tool] pairs", "[SnapmakerProtocol]")
{
    const auto [filament_map_1based, expected] = GENERATE(table<std::vector<int>, std::string>({
        // Captured from the U1 touchscreen; the input is the captured tools + 1.
        {{4, 3, 1, 2, 2}, "[[0, 3], [1, 2], [2, 0], [3, 1], [4, 1]]"},
        {{1, 2, 3}, "[[0, 0], [1, 1], [2, 2]]"},
        // Two filaments merged onto one tool keep the repeated tool index.
        {{1, 1, 2}, "[[0, 0], [1, 0], [2, 1]]"},
        {{1}, "[[0, 0]]"},
        // Unassigned filaments are omitted; the rest keep their logical indices.
        {{1, 0, 3, 0, 2}, "[[0, 0], [2, 2], [4, 1]]"},
        {{0, 0, 0}, "[]"},
    }));
    CHECK(render_map_table(filament_map_1based) == expected);
}

// Orca: mirrors Moonraker::upload()'s placeholder substitution.
TEST_CASE("Placeholder-rendered start script substitutes to the captured Snapmaker U1 reference", "[SnapmakerProtocol]")
{
    const std::vector<int> filament_map_1based = {4, 3, 1, 2, 2};

    std::string script = build_start_script(PRINT_HOST_UPLOADED_FILENAME_PLACEHOLDER, filament_map_1based);
    CHECK(script.find(PRINT_HOST_UPLOADED_FILENAME_PLACEHOLDER) != std::string::npos);

    boost::replace_first(script, PRINT_HOST_UPLOADED_FILENAME_PLACEHOLDER, "Cube_U1_filamap_sm_PLA_31m18s.gcode");
    // BED_LEVEL is not in the captured reference; the firmware reads it absent as off.
    CHECK(script ==
          "SDCARD_PRINT_FILE_WITH_PARAMETERS FILENAME=\"Cube_U1_filamap_sm_PLA_31m18s.gcode\" "
          "MAP_TABLE=\"[[0, 3], [1, 2], [2, 0], [3, 1], [4, 1]]\" BED_LEVEL=\"1\"");
}

// Verbatim touchscreen capture: five filaments on four tools, all options on, all tools used.
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
    Slic3r::DevicePrintJobInfo job = capture_job();
    job.options = { {"bed_leveling", "0"}, {"flow_calibrate", "0"}, {"time_lapse", "0"} };
    const std::string script = Slic3r::SnapmakerProtocol::build_start_script("f.gcode", job);
    CHECK(script.find("BED_LEVEL=\"0\"") != std::string::npos);
    CHECK(script.find("TIME_LAPSE_CAMERA=\"0\"") != std::string::npos);
    CHECK(script.find("FLOW_CALIBRATE=\"0\"") != std::string::npos);
    CHECK(script.find("FLOW_CALIBRATE_EXTRUDERS=\"[]\"") != std::string::npos);
}

TEST_CASE("Flow calibration covers only the tools the plate uses", "[SnapmakerProtocol]")
{
    Slic3r::DevicePrintJobInfo job = capture_job();
    job.used_physical_tools = { 0, 2 };
    const std::string script = Slic3r::SnapmakerProtocol::build_start_script("f.gcode", job);
    CHECK(script.find("FLOW_CALIBRATE_EXTRUDERS=\"[0, 2]\"") != std::string::npos);
}

// Orca: CARD_UID is an array of tag bytes for a tagged spool and the number 0 otherwise.

TEST_CASE("Reads a CARD_UID reported as an array of tag bytes", "[SnapmakerProtocol]")
{
    const auto slot = nlohmann::json::parse(R"({
        "VERSION": 1, "VENDOR": "ELEGOO", "MANUFACTURER": "ELEGOO", "MAIN_TYPE": "PLA",
        "SUB_TYPE": "Translucent", "BED_TEMP": 60, "FIRST_LAYER_TEMP": 210,
        "OTHER_LAYER_TEMP": 210, "HOTEND_MAX_TEMP": 220, "HOTEND_MIN_TEMP": 210,
        "ARGB_COLOR": 4293519847, "DIAMETER": 175, "OFFICIAL": true,
        "CARD_UID": [4, 214, 239, 50, 197, 42, 129], "SPOOL_ID": "132"})");

    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(slot) == "0004D6EF32C52A81");
    CHECK(Slic3r::json_number_or(slot, "BED_TEMP", 0) == 60);
    CHECK(Slic3r::json_number_or(slot, "FIRST_LAYER_TEMP", 0) == 210);
    CHECK(Slic3r::json_string_or(slot, "VENDOR", "NONE") == "ELEGOO");
}

TEST_CASE("A lane with no tag reports no tag id", "[SnapmakerProtocol]")
{
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(nlohmann::json::parse(R"({"VENDOR":"NONE","CARD_UID":0})")).empty());
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(nlohmann::json::parse(R"({"CARD_UID":[0,0,0]})")).empty());
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(nlohmann::json::parse(R"({"VENDOR":"NONE"})")).empty());
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(nlohmann::json::parse(R"({"CARD_UID":"04D6EF"})")).empty());
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(nlohmann::json::parse(R"({"CARD_UID":[4,"x"]})")).empty());
}

TEST_CASE("A numeric CARD_UID still renders as a 16-digit tag id", "[SnapmakerProtocol]")
{
    CHECK(Slic3r::SnapmakerProtocol::card_uid_hex(nlohmann::json::parse(R"({"CARD_UID":81985529216486895})")) ==
          "0123456789ABCDEF");
}

TEST_CASE("A field of the wrong type falls back instead of throwing", "[SnapmakerProtocol]")
{
    const auto slot = nlohmann::json::parse(R"({"BED_TEMP":"hot","NOZZLE_TEMP":"220","FIRST_LAYER_TEMP":null,"VENDOR":7})");
    CHECK(Slic3r::json_number_or(slot, "BED_TEMP", 0) == 0);
    // A whole number written as a string is still a number.
    CHECK(Slic3r::json_number_or(slot, "NOZZLE_TEMP", 0) == 220);
    CHECK(Slic3r::json_number_or(slot, "FIRST_LAYER_TEMP", 7) == 7);
    CHECK(Slic3r::json_number_or(slot, "MISSING", 42) == 42);
    CHECK(Slic3r::json_string_or(slot, "VENDOR", "NONE") == "NONE");
}
