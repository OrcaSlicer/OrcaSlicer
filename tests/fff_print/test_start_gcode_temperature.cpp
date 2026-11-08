#include <catch2/catch_all.hpp>

#include "test_helpers.hpp"

#include "libslic3r/GCode/StartGCodeTemperature.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Warnings raised while exporting G-code that mention `needle`.
size_t count_export_warnings(const Print &print, const std::string &needle)
{
    const auto state = print.step_state_with_warnings(psGCodeExport);
    return std::count_if(state.warnings.begin(), state.warnings.end(),
                         [&needle](const PrintStateBase::Warning &w) { return w.message.find(needle) != std::string::npos; });
}

std::string first_export_warning(const Print &print, const std::string &needle)
{
    const auto state = print.step_state_with_warnings(psGCodeExport);
    for (const auto &w : state.warnings)
        if (w.message.find(needle) != std::string::npos)
            return w.message;
    return {};
}

const std::string NOZZLE_WARNING = "start G-code heats the nozzle";
const std::string BED_WARNING    = "start G-code heats the bed";

// Slices a 20 mm cube with a 260 degree first layer nozzle temperature and a 100 degree
// first layer bed temperature on the textured plate, using `start_gcode`.
void slice_with_start_gcode(Print &print, Model &model, const std::string &start_gcode, const std::string &print_sequence = "by layer",
                            const std::string &filament_start_gcode = "")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "machine_start_gcode",               start_gcode },
        { "nozzle_temperature_initial_layer",  "260" },
        { "nozzle_temperature",                "260" },
        { "curr_bed_type",                     "Textured PEI Plate" },
        { "textured_plate_temp_initial_layer", "100" },
        { "textured_plate_temp",               "100" },
        { "filament_settings_id",              "\"Generic ASA\"" },
        { "print_sequence",                    print_sequence },
    });
    if (!filament_start_gcode.empty())
        config.set_key_value("filament_start_gcode", new ConfigOptionStrings{ filament_start_gcode });
    init_print({ cube(20) }, print, model, config);
    gcode(print);
}

} // namespace

TEST_CASE("The last temperature command in effect is found", "[StartGCodeTemperature]")
{
    const auto nozzle = nozzle_temperature_commands(false);
    CHECK(last_gcode_temperature("M104 S150\nG28\nM109 S205 ; wait\n", nozzle) == 205);
    CHECK(last_gcode_temperature("M109 R230\n", nozzle) == 230);
    CHECK(last_gcode_temperature("G28\nM1040 S20\n", nozzle) == std::nullopt);
    CHECK(last_gcode_temperature("; M109 S205\n", nozzle) == std::nullopt);
    CHECK(last_gcode_temperature("M109S215\n", nozzle) == 215);
    // Tool words are ignored, as they are when deciding that the start G-code heats.
    CHECK(last_gcode_temperature("M109 S260\nM104 T1 S150\n", nozzle) == 150);
    // CR-only line endings.
    CHECK(last_gcode_temperature("M104 S150\rM109 S205\rG28\r", nozzle) == 205);
    CHECK(last_gcode_temperature("M190 S60\n", bed_temperature_commands()) == 60);
}

TEST_CASE("RepRapFirmware G10 sets the nozzle temperature", "[StartGCodeTemperature]")
{
    CHECK(last_gcode_temperature("G10 P0 S215 R150\n", nozzle_temperature_commands(true)) == 215);
    CHECK(last_gcode_temperature("G10 P0 S215 R150\n", nozzle_temperature_commands(false)) == std::nullopt);
    CHECK(start_gcode_literal_temperature_mismatch("G10 P0 S205 R150\n", "G10 P0 S205 R150\n", nozzle_temperature_commands(true), 260) == 205);
}

TEST_CASE("Literal temperatures are told apart from placeholders", "[StartGCodeTemperature]")
{
    const auto nozzle = nozzle_temperature_commands(false);
    CHECK(gcode_template_last_temperature_is_literal("M109 S205;\n", nozzle, 205));
    CHECK(gcode_template_last_temperature_is_literal("M109 S205.0\n", nozzle, 205));
    CHECK_FALSE(gcode_template_last_temperature_is_literal("M109 S[nozzle_temperature_initial_layer]\n", nozzle, 205));
    CHECK_FALSE(gcode_template_last_temperature_is_literal("M109 S{nozzle_temperature_initial_layer[0]}\n", nozzle, 205));
    // Same-line conditionals.
    CHECK(gcode_template_last_temperature_is_literal("{if curr_bed_type==\"Cool Plate\"}M190 S35{endif}\n", bed_temperature_commands(), 35));
    CHECK(gcode_template_sets_temperature("{if 1}M109 S[nozzle_temperature_initial_layer]{endif}\n", nozzle));
    CHECK_FALSE(gcode_template_sets_temperature("G28\nM106 S255\n", nozzle));
}

TEST_CASE("A literal start G-code temperature far from the filament's is reported", "[StartGCodeTemperature]")
{
    const auto nozzle = nozzle_temperature_commands(false);
    // Literal 205 against a 260 degree filament: reported.
    CHECK(start_gcode_literal_temperature_mismatch("M109 S205;\n", "M109 S205;\n", nozzle, 260) == 205);
    // A tool-addressed literal also suppresses the slicer's heat-up, so it is reported too.
    CHECK(start_gcode_literal_temperature_mismatch("M104 T2 S200\n", "M104 T2 S200\n", nozzle, 260) == 200);
    // Placeholder: the processed G-code holds a number but the template does not.
    CHECK(start_gcode_literal_temperature_mismatch("M109 S{nozzle_temperature_initial_layer[0]-55}\n", "M109 S205\n", nozzle, 260) == std::nullopt);
    // The last command decides: a literal earlier in the template with the same value as a
    // later placeholder expression is not reported.
    CHECK(start_gcode_literal_temperature_mismatch("M104 S150\nM104 S{nozzle_temperature_initial_layer[0]-110}\n", "M104 S150\nM104 S150\n", nozzle, 260) == std::nullopt);
    // Within the tolerance: not reported.
    CHECK(start_gcode_literal_temperature_mismatch("M109 S255\n", "M109 S255\n", nozzle, 260) == std::nullopt);
    CHECK(start_gcode_literal_temperature_mismatch("M109 S250\n", "M109 S250\n", nozzle, 260) == std::nullopt);
    CHECK(start_gcode_literal_temperature_mismatch("M109 S249\n", "M109 S249\n", nozzle, 260) == 249);
    // A literal preheat followed by a placeholder heat-up ends at the filament's temperature.
    CHECK(start_gcode_literal_temperature_mismatch("M104 S140\nM109 S[nozzle_temperature_initial_layer]\n", "M104 S140\nM109 S260\n", nozzle, 260) == std::nullopt);
    // Heater off, left to a firmware macro: not reported.
    CHECK(start_gcode_literal_temperature_mismatch("M104 S0\nSTART_PRINT EXTRUDER_TEMP=[nozzle_temperature_initial_layer]\n", "M104 S0\nSTART_PRINT EXTRUDER_TEMP=260\n", nozzle, 260) == std::nullopt);
}

TEST_CASE("Slicing warns about a literal nozzle temperature in the start G-code", "[StartGCodeTemperature]")
{
    Print print;
    Model model;
    slice_with_start_gcode(print, model, "M109 S205;\n");
    REQUIRE(count_export_warnings(print, NOZZLE_WARNING) == 1);
    const std::string msg = first_export_warning(print, NOZZLE_WARNING);
    CHECK(msg.find("205") != std::string::npos);
    CHECK(msg.find("260") != std::string::npos);
    CHECK(msg.find("Generic ASA") != std::string::npos);
    CHECK(msg.find("[nozzle_temperature_initial_layer]") != std::string::npos);
    CHECK(msg.find("one at a time") == std::string::npos);
    // A warning, not an error: the G-code is still exported.
    CHECK(print.is_step_done(psGCodeExport));
}

TEST_CASE("Slicing does not warn about a placeholder nozzle temperature", "[StartGCodeTemperature]")
{
    Print print;
    Model model;
    slice_with_start_gcode(print, model, "M109 S[nozzle_temperature_initial_layer]\nM190 S[bed_temperature_initial_layer_single]\n");
    CHECK(count_export_warnings(print, NOZZLE_WARNING) == 0);
    CHECK(count_export_warnings(print, BED_WARNING) == 0);
}

TEST_CASE("Slicing does not warn about a literal within 10 degrees", "[StartGCodeTemperature]")
{
    Print print;
    Model model;
    // 252 vs 260 and 95 vs 100 are inside the 10 degree tolerance.
    slice_with_start_gcode(print, model, "M109 S252\nM190 S95\n");
    CHECK(count_export_warnings(print, NOZZLE_WARNING) == 0);
    CHECK(count_export_warnings(print, BED_WARNING) == 0);
}

TEST_CASE("Slicing warns about a literal bed temperature in the start G-code", "[StartGCodeTemperature]")
{
    Print print;
    Model model;
    slice_with_start_gcode(print, model, "M190 S60\nM109 S[nozzle_temperature_initial_layer]\n");
    CHECK(count_export_warnings(print, NOZZLE_WARNING) == 0);
    REQUIRE(count_export_warnings(print, BED_WARNING) == 1);
    const std::string msg = first_export_warning(print, BED_WARNING);
    CHECK(msg.find("60") != std::string::npos);
    CHECK(msg.find("100") != std::string::npos);
}

TEST_CASE("The warning mentions the first object when printing by object", "[StartGCodeTemperature]")
{
    Print print;
    Model model;
    slice_with_start_gcode(print, model, "M109 S205;\n", "by object");
    REQUIRE(count_export_warnings(print, NOZZLE_WARNING) == 1);
    CHECK(first_export_warning(print, NOZZLE_WARNING).find("one at a time") != std::string::npos);
}

TEST_CASE("Slicing warns about a tool-addressed literal nozzle temperature", "[StartGCodeTemperature]")
{
    Print print;
    Model model;
    // The T word does not stop the start G-code from suppressing the slicer's heat-up.
    slice_with_start_gcode(print, model, "M104 T2 S200\n");
    CHECK(count_export_warnings(print, NOZZLE_WARNING) == 1);
}

TEST_CASE("Slicing does not warn when the last start G-code temperature is an expression", "[StartGCodeTemperature]")
{
    Print print;
    Model model;
    // 260 - 110 = 150, the same value as the earlier literal.
    slice_with_start_gcode(print, model, "M104 S150\nM104 S{nozzle_temperature_initial_layer[0]-110}\n");
    CHECK(count_export_warnings(print, NOZZLE_WARNING) == 0);
}

TEST_CASE("Slicing does not warn when the filament start G-code sets the nozzle temperature", "[StartGCodeTemperature]")
{
    Print print;
    Model model;
    slice_with_start_gcode(print, model, "M109 S205;\n", "by layer", "M109 S[nozzle_temperature_initial_layer]\n");
    CHECK(count_export_warnings(print, NOZZLE_WARNING) == 0);
}

TEST_CASE("The bed warning does not mention printing one object at a time", "[StartGCodeTemperature]")
{
    Print print;
    Model model;
    slice_with_start_gcode(print, model, "M190 S60\nM109 S[nozzle_temperature_initial_layer]\n", "by object");
    REQUIRE(count_export_warnings(print, BED_WARNING) == 1);
    CHECK(first_export_warning(print, BED_WARNING).find("one at a time") == std::string::npos);
}
