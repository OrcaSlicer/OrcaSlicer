#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/PressureEqualizer.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <cstddef>
#include <string>

using namespace Slic3r;

// The preview spreads an overhang profile over every piece of its G1 that carries the profile's marker. A line the
// equalizer ramps is written as several pieces, so each one must keep the marker: a fast line after a slow one only
// ramps up, and one between two slow lines also has a steady part and ramps down again.
TEST_CASE("Split extrusions keep the overhang profile marker on every piece", "[PressureEqualizer][Overhang][Regression]")
{
    const bool slow_after = GENERATE(false, true);
    CAPTURE(slow_after);
    GCodeConfig config;
    // At 1 mm^3/s^2 a 30 mm line cannot reach its flow at all. At 20 mm^3/s^2 the flow ramps over about 24 mm at
    // each end of a 60 mm line, leaving a steady part between the ramps.
    config.max_volumetric_extrusion_rate_slope.value                = slow_after ? 20. : 1.;
    config.max_volumetric_extrusion_rate_slope_segment_length.value = 2.;
    config.use_relative_e_distances.value                           = true;
    PressureEqualizer equalizer(config);
    const std::string marker = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Overhang_Line_Apply);
    const double fast_end = slow_after ? 70. : 40.;
    // Lines of one cross section at 10 mm/s and 100 mm/s. The equalizer never adjusts the last extrusion of a
    // segment, so a short move without an X word ends it.
    std::string layer = ";_EXTRUSION_ROLE:" + std::to_string(int(erSolidInfill)) + "\n"
        "G1 X0 Y0 F6000\n"
        "G1 E1 F2400\n"
        "G1 F600;_EXTRUDE_SET_SPEED\n"
        "G1 X10 Y0 E0.4\n"
        ";_EXTRUDE_END\n"
        "G1 F6000;_EXTRUDE_SET_SPEED\n" +
        std::string(slow_after ? "G1 X70 Y0 E2.4" : "G1 X40 Y0 E1.2") + " ; solid infill" + marker + "\n"
        ";_EXTRUDE_END\n";
    if (slow_after)
        layer += "G1 F600;_EXTRUDE_SET_SPEED\n"
                 "G1 X80 Y0 E0.4\n"
                 ";_EXTRUDE_END\n";
    layer += "G1 F600;_EXTRUDE_SET_SPEED\n"
             "G1 Y1 E0.04\n"
             ";_EXTRUDE_END\n";
    equalizer.process_layer(LayerResult{layer, 0, false, false, false});
    const std::string output = equalizer.process_layer(LayerResult::make_nop_layer_result()).gcode;
    size_t pieces = 0;
    size_t marked_pieces = 0;
    GCodeReader reader;
    reader.parse_buffer(output, [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
        if (!line.cmd_is("G1") || !line.has_e() || !line.has_x() || line.x() <= 10. + EPSILON || line.x() > fast_end + EPSILON)
            return;
        ++pieces;
        if (GCodeProcessor::has_inline_tag(line.comment(), GCodeProcessor::ETags::Overhang_Line_Apply))
            ++marked_pieces;
    });
    INFO(output);
    REQUIRE(pieces > 1);
    CHECK(marked_pieces == pieces);
}
