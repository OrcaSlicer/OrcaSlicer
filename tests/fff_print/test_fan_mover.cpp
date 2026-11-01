#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

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
