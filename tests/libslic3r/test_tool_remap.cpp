#include <catch2/catch_test_macros.hpp>

#include "libslic3r/GCode/ToolRemap.hpp"

#include <openssl/md5.h>

#include <map>
#include <string>

using namespace Slic3r;

namespace {

std::string md5_hex(const std::string& data)
{
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
    static const char hex[] = "0123456789abcdef";
    std::string       out;
    for (unsigned char byte : digest) {
        out += hex[byte >> 4];
        out += hex[byte & 0xF];
    }
    return out;
}

} // namespace

TEST_CASE("Tool changes are renumbered through the tool map", "[ToolRemap]")
{
    // Filament 7 (T6) prints on physical tool 2, filament 2 (T1) on tool 0.
    const std::map<int, int> tool_map{{6, 2}, {1, 0}};

    CHECK(remap_gcode_tools("T6\n", tool_map) == "T2\n");
    CHECK(remap_gcode_tools("T1 ; change extruder\n", tool_map) == "T0 ; change extruder\n");
    // A tool number missing from the map is left alone.
    CHECK(remap_gcode_tools("T3\n", tool_map) == "T3\n");
}

TEST_CASE("The tool argument of M104 and M109 is renumbered, other T words are not", "[ToolRemap]")
{
    const std::map<int, int> tool_map{{6, 2}};

    CHECK(remap_gcode_tools("M104 S220 T6\n", tool_map) == "M104 S220 T2\n");
    CHECK(remap_gcode_tools("M109 T6 S220\n", tool_map) == "M109 T2 S220\n");
    // Only a standalone T<n> or an M104 / M109 argument names a tool.
    CHECK(remap_gcode_tools("SET_LED T6\n", tool_map) == "SET_LED T6\n");
    CHECK(remap_gcode_tools("G1 X10 T6\n", tool_map) == "G1 X10 T6\n");
}

TEST_CASE("Comments and line endings survive the renumbering", "[ToolRemap]")
{
    const std::map<int, int> tool_map{{6, 2}};

    CHECK(remap_gcode_tools("; T6 is filament 7\r\nT6\r\n", tool_map) == "; T6 is filament 7\r\nT2\r\n");
    CHECK(remap_gcode_tools("M104 S200 ; T6 next\n", tool_map) == "M104 S200 ; T6 next\n");
    CHECK(remap_gcode_tools("T6", tool_map) == "T2");
}

TEST_CASE("Comment lines go through the comment hook", "[ToolRemap]")
{
    const std::map<int, int> tool_map{{6, 2}};
    const auto               hook = [](std::string& line) {
        if (line == "; tools = 6")
            line = "; tools = 2";
    };

    CHECK(remap_gcode_tools("; tools = 6\nT6\n", tool_map, hook) == "; tools = 2\nT2\n");
}

TEST_CASE("A leading MD5 line is recomputed over the renumbered file", "[ToolRemap]")
{
    const std::map<int, int> tool_map{{6, 2}};
    const std::string        body = "G28\r\nT6\r\n";
    const std::string        gcode = "; MD5:" + md5_hex(body) + "\r\n" + body;

    const std::string expected_body = "G28\r\nT2\r\n";
    CHECK(remap_gcode_tools(gcode, tool_map) == "; MD5:" + md5_hex(expected_body) + "\r\n" + expected_body);
}
