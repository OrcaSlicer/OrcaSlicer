#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

#include <string>
#include <vector>

#include "libslic3r/GCode/FanMover.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;

TEST_CASE("Fan mover keeps the selected precision when it splits a move, and the standard one for supports", "[FanMover][Regression]")
{
    const bool high_precision = GENERATE(false, true);
    const bool support = GENERATE(false, true);
    CAPTURE(high_precision, support);
    PrintConfig config;
    config.high_precision_gcode.value     = high_precision;
    config.use_relative_e_distances.value = true;
    GCodeWriter writer;
    writer.apply_print_config(config);

    // At 10 mm/s the move takes 1.01234 s. Moving the fan command 0.5 s earlier splits the move after 0.51234 s,
    // at X 10 * 0.51234 with E 0.012346 * 0.51234 / 1.01234, and leaves the rest of E to the second part.
    FanMover mover(writer, 0.5f, false, true, false, 0.f);
    const std::string type = support ? ";TYPE:Support\n" : ";TYPE:Outer wall\n";
    const std::string gcode = mover.process_gcode(type + "G1 F600\nG1 X10.1234 E0.012346\nM106 S255\n", true);
    // Supports are written with the standard precision, see GCode::extrude_support().
    const bool written_with_high_precision = high_precision && ! support;

    std::vector<GCodeReader::GCodeLine> moves;
    GCodeReader reader;
    reader.apply_config(config);
    reader.parse_buffer(gcode, [&moves](GCodeReader &, const GCodeReader::GCodeLine &line) {
        if (line.has_x())
            moves.push_back(line);
    });
    REQUIRE(moves.size() == 2);
    CHECK_THAT(moves.front().precise_value(X), Catch::Matchers::WithinAbs(written_with_high_precision ? 5.1234 : 5.123, 1e-9));
    CHECK_THAT(moves.front().precise_value(E), Catch::Matchers::WithinAbs(written_with_high_precision ? 0.006248 : 0.00625, 1e-9));
    CHECK_THAT(moves.back().precise_value(E), Catch::Matchers::WithinAbs(written_with_high_precision ? 0.006098 : 0.0061, 1e-9));
}

namespace {

// The heights the moves of `gcode` go to, following G90 and G91.
std::vector<float> move_heights(const std::string &gcode)
{
    std::vector<float> heights;
    float z = 0.f;
    bool relative = false;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
        if (line.cmd_is("G90") || line.cmd_is("G91"))
            relative = line.cmd_is("G91");
        else if (line.cmd_is("G1") && line.has_z())
            heights.push_back(z = relative ? z + line.z() : line.z());
    });
    return heights;
}

} // namespace

TEST_CASE("Fan mover keeps the height after custom G-code that lifts the nozzle in relative mode", "[FanMover][Regression]")
{
    PrintConfig config;
    GCodeWriter writer;
    writer.apply_print_config(config);
    // The fan command moves 1 s back, to the middle of the 2 s move from the lifted 12 mm back down to 10 mm.
    FanMover mover(writer, 1.f, false, true, false, 0.f);
    const std::string gcode = mover.process_gcode(";TYPE:Outer wall\nG1 F600\nG1 Z10\n"
                                                  ";_FAN_MOVER_SKIP_START\nG91\nG1 Z2\nG90\n;_FAN_MOVER_SKIP_END\n"
                                                  "G1 Z10 F60\nM106 S255\n", true);
    const std::vector<float> heights = move_heights(gcode);
    REQUIRE(heights.size() == 4);
    CHECK_THAT(heights[1], Catch::Matchers::WithinAbs(12., 1e-4));
    CHECK_THAT(heights[2], Catch::Matchers::WithinAbs(11., 1e-4));
    CHECK_THAT(heights[3], Catch::Matchers::WithinAbs(10., 1e-4));
}

TEST_CASE("Fan mover does not split a move in relative mode", "[FanMover]")
{
    PrintConfig config;
    GCodeWriter writer;
    writer.apply_print_config(config);
    FanMover mover(writer, 0.5f, false, true, false, 0.f);
    const std::string gcode = mover.process_gcode(";TYPE:Outer wall\nG1 F60\nG1 Z10\nG91\nG1 Z2\nM106 S255\nG90\n", true);
    CHECK_THAT(move_heights(gcode), Catch::Matchers::Approx(std::vector<float>{ 10.f, 12.f }).margin(1e-4));
    CHECK_THAT(gcode, Catch::Matchers::ContainsSubstring("M106 S255\nG1 Z2"));
}

TEST_CASE("Fan mover reads the moves after a role tag as absolute, also where custom G-code left G91 through a macro", "[FanMover]")
{
    PrintConfig config;
    GCodeWriter writer;
    writer.apply_print_config(config);
    // RESTORE_GCODE_STATE ends G91 out of the reader's sight. Orca's next move follows a role tag, in absolute positioning.
    FanMover mover(writer, 1.f, false, true, false, 0.f);
    const std::string gcode = mover.process_gcode(";TYPE:Outer wall\nG1 F600\nG1 Z10\n"
                                                  "SAVE_GCODE_STATE NAME=lift\nG91\nG1 Z2\nRESTORE_GCODE_STATE NAME=lift\n"
                                                  ";TYPE:Inner wall\nG1 Z10 F60\nM106 S255\n", true);
    // The fan command moves 1 s back, splitting the 2 s move from 12 mm down to 10 mm in the middle.
    std::vector<float> heights;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&heights](GCodeReader &, const GCodeReader::GCodeLine &line) {
        if (line.cmd_is("G1") && line.has_z())
            heights.push_back(line.z());
    });
    REQUIRE(heights.size() == 4);
    CHECK_THAT(heights[2], Catch::Matchers::WithinAbs(11., 1e-4));
}
