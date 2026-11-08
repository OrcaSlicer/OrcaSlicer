#include <catch2/catch_all.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/WebAppPresetShare.hpp"
#include "slic3r/GUI/WebAppSettings.hpp"

#include <algorithm>

using namespace Slic3r;
namespace Share = Slic3r::GUI::WebAppPresetShare;

namespace {

std::vector<Share::FlaggedLine> scan(const std::string& text)
{
    std::vector<Share::FlaggedLine> out;
    Share::scan_gcode_text("machine_start_gcode", text, out);
    return out;
}

} // namespace

TEST_CASE("Printer-profile G-code review flags risky M-codes as whole tokens", "[WebAppPresetShare]")
{
    SECTION("plain, lower-case, zero-padded and template-glued forms are flagged")
    {
        for (const char* line : {"M500", "m500", "M0500", "{endif}M500", "  M500 S1", "G28\tM500"}) {
            const auto flagged = scan(line);
            INFO(line);
            REQUIRE(flagged.size() == 1);
            CHECK(flagged.front().reasons == "saves settings permanently (M500)");
        }
    }
    SECTION("longer numbers, decimals, glued letters and comments are not")
    {
        for (const char* line : {"M5000", "M500.1", "XM500", "M50", "G28 ; M500 would save", "; M997 update", "M1500", "M_500"})
            CHECK(scan(line).empty());
    }
    SECTION("each table entry is recognised")
    {
        for (int code : {997, 30, 28, 29, 559, 560, 500, 502, 552, 553, 554, 587, 588, 589, 564, 302, 143, 906, 913, 208, 669, 584, 999}) {
            INFO(code);
            CHECK(scan("M" + std::to_string(code) + " P1").size() == 1);
        }
        CHECK(scan("M104 S200\nM109 S200\nG1 X10").empty());
    }
    SECTION("one entry per line, several reasons joined, comment stripped from the shown line")
    {
        const auto flagged = scan("G90\nM906 X800 Y800 M913 X50 ; quieter\nM552 S1\n");
        REQUIRE(flagged.size() == 2);
        CHECK(flagged[0].key == "machine_start_gcode");
        CHECK(flagged[0].line == "M906 X800 Y800 M913 X50");
        CHECK(flagged[0].reasons == "changes motor current (M906); changes motor current (M913)");
        CHECK(flagged[1].line == "M552 S1");
        CHECK(flagged[1].reasons == "changes network settings (M552)");
    }
    SECTION("CRLF text")
    {
        const auto flagged = scan("G28\r\nM999\r\n");
        REQUIRE(flagged.size() == 1);
        CHECK(flagged.front().line == "M999");
    }
}

TEST_CASE("Printer-profile review lists M98 macros and caps the flagged list", "[WebAppPresetShare]")
{
    std::vector<std::string> macros;
    Share::collect_called_macros("M98 P\"homeall.g\"\nm98 P\"0:/macros/Load PLA.g\" ; load\nM98 Pbed.g\n; M98 P\"hidden.g\"\nM980 P\"no.g\"\nM98 P\"homeall.g\"",
                                 macros);
    CHECK(macros == std::vector<std::string>{"0:/macros/Load PLA.g", "bed.g", "homeall.g"});

    std::vector<Share::FlaggedLine> many;
    for (int i = 0; i < 25; ++i)
        many.push_back({"machine_end_gcode", "M500", "saves settings permanently (M500)"});
    const auto lines = Share::format_flagged(many, 20);
    REQUIRE(lines.size() == 21);
    CHECK(lines.front() == "machine_end_gcode: M500 \xE2\x80\x94 saves settings permanently (M500)");
    CHECK(lines.back() == "and 5 more");
    CHECK(Share::format_flagged({}, 20).empty());
}

TEST_CASE("Printer-profile review scans every *_gcode string option of a config", "[WebAppPresetShare]")
{
    DynamicPrintConfig config;
    config.set_key_value("machine_start_gcode", new ConfigOptionString("G28\nM98 P\"start.g\"\nM302 P1"));
    config.set_key_value("machine_end_gcode", new ConfigOptionString("M104 S0\nM30 \"temp.gcode\""));
    config.set_key_value("printer_notes", new ConfigOptionString("M500 in the notes is not code"));
    const auto flagged = Share::scan_custom_gcode(config);
    REQUIRE(flagged.size() == 2);
    CHECK(flagged[0].key == "machine_end_gcode"); // key order
    CHECK(flagged[0].reasons == "deletes a file (M30)");
    CHECK(flagged[1].key == "machine_start_gcode");
    CHECK(flagged[1].reasons == "allows cold extrusion (M302)");
    CHECK(Share::called_macros(config) == std::vector<std::string>{"start.g"});
}

TEST_CASE("A shared printer profile never keeps a print host or a local path", "[WebAppPresetShare]")
{
    const auto& stripped = Share::stripped_keys();
    for (const std::string& key : GUI::WebAppSettings::WAB_SECRET_KEYS)
        CHECK(std::find(stripped.begin(), stripped.end(), key) != stripped.end());
    CHECK(std::find(stripped.begin(), stripped.end(), "bed_custom_texture") != stripped.end());
    CHECK(std::find(stripped.begin(), stripped.end(), "bed_custom_model") != stripped.end());

    DynamicPrintConfig config;
    config.set_key_value("print_host", new ConfigOptionString("192.0.2.90"));
    config.set_key_value("printhost_apikey", new ConfigOptionString("secret"));
    config.set_key_value("printhost_password", new ConfigOptionString("hunter2"));
    config.set_deserialize_strict("host_type", "octoprint");
    config.set_key_value("bed_custom_texture", new ConfigOptionString("C:\\Users\\someone\\Downloads\\bed.png"));
    config.set_key_value("bed_custom_model", new ConfigOptionString("C:\\Users\\someone\\bed.stl"));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4}));
    config.set_key_value("machine_start_gcode", new ConfigOptionString("G28"));

    CHECK(Share::sanitise_printer_preset(config) == 6);
    for (const std::string& key : stripped)
        CHECK_FALSE(config.has(key));
    CHECK(config.has("nozzle_diameter"));
    CHECK(config.has("machine_start_gcode"));
    CHECK(Share::sanitise_printer_preset(config) == 0); // idempotent
}

TEST_CASE("Printer-profile names must be usable as preset names", "[WebAppPresetShare]")
{
    CHECK(Share::preset_name_usable("My Voron 2.4 (0.4)"));
    CHECK(Share::preset_name_usable("U3D V1.9  HOP DONT 5x"));
    for (const char* bad : {"", " ", ".", "..", "../machine/x", "a/b", "a\\b", "C:x", "a*b", "a?b", "a\"b", "a<b", "a|b", "a\nb"})
        CHECK_FALSE(Share::preset_name_usable(bad));
}
