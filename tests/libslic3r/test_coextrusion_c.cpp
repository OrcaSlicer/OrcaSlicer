#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/CoExtrusionC.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeWriter.hpp"

#include <algorithm>
#include <cmath>

using namespace Slic3r;

TEST_CASE("Co-extrusion C-axis angles stay bounded and use the shortest wrap", "[CoExtrusionC]")
{
    REQUIRE(CoExtrusionCController::normalize_angle(-1.0) == Catch::Approx(359.0));
    REQUIRE(CoExtrusionCController::normalize_angle(360.0) == Catch::Approx(0.0));
    REQUIRE(CoExtrusionCController::shortest_angular_delta(359.0, 1.0) == Catch::Approx(2.0));
    REQUIRE(CoExtrusionCController::shortest_angular_delta(1.0, 359.0) == Catch::Approx(-2.0));
}

TEST_CASE("Co-extrusion C-axis target aligns a sector with either wall-normal side", "[CoExtrusionC]")
{
    CoExtrusionCController controller;

    // A +X tangent has a -Y (270 degree) right normal.
    auto angle = controller.update_for_segment(1.0, 0.0, true, 0.0, 0.0, false, 0.0);
    REQUIRE(angle.has_value());
    REQUIRE(*angle == Catch::Approx(270.0));

    controller.reset();
    angle = controller.update_for_segment(1.0, 0.0, false, 0.0, 0.0, false, 0.0);
    REQUIRE(*angle == Catch::Approx(90.0));

    controller.reset();
    angle = controller.update_for_segment(1.0, 0.0, true, 120.0, 0.0, false, 0.0);
    REQUIRE(*angle == Catch::Approx(150.0));
}

TEST_CASE("Co-extrusion distance filter crosses zero without taking the long way", "[CoExtrusionC]")
{
    constexpr double pi = 3.14159265358979323846;
    auto tangent_for_right_normal = [pi](double normal_degrees) {
        const double tangent = (normal_degrees + 90.0) * pi / 180.0;
        return std::pair<double, double>(std::cos(tangent), std::sin(tangent));
    };

    CoExtrusionCController controller;
    auto [dx0, dy0] = tangent_for_right_normal(359.0);
    auto [dx1, dy1] = tangent_for_right_normal(1.0);
    REQUIRE(*controller.update_for_segment(dx0, dy0, true, 0.0, 0.0, false, 1.0) == Catch::Approx(359.0));

    const double filtered = *controller.update_for_segment(dx1, dy1, true, 0.0, 0.0, false, 1.0);
    REQUIRE(filtered >= 0.0);
    REQUIRE(filtered < 2.0);
}

TEST_CASE("G-code formatter emits a bounded C-axis word on a coordinated move", "[CoExtrusionC]")
{
    GCodeG1Formatter formatter;
    formatter.emit_xy(Vec2d(10.0, 20.0));
    formatter.emit_e(1.25);
    formatter.emit_c(359.75);
    const std::string gcode = formatter.string();

    REQUIRE(gcode.find(" C359.75") != std::string::npos);
}

TEST_CASE("Painted 3MF colors map to physical sectors independently of slot order", "[CoExtrusionC]")
{
    const std::vector<size_t> mapping = map_coextrusion_filament_colors_to_sectors(
        {"#0000FF", "#FF0000", "#00FF00"},
        {"#FF0000", "#00FF00", "#0000FF"});

    REQUIRE(mapping == std::vector<size_t>{2, 0, 1});
}

TEST_CASE("Color mapping uses a one-to-one nearest match", "[CoExtrusionC]")
{
    const std::vector<size_t> mapping = map_coextrusion_filament_colors_to_sectors(
        {"#F01010", "#10F010", "#1010F0"},
        {"#FF0000", "#00FF00", "#0000FF"});

    REQUIRE(mapping == std::vector<size_t>{0, 1, 2});
}

TEST_CASE("G-code preview preserves the physical co-extrusion color sector", "[CoExtrusionC]")
{
    FullPrintConfig config;
    config.coextrusion_c_axis_enable.value = true;
    config.coextrusion_c_axis_colors.values = {"#FF0000", "#00FF00", "#0000FF"};

    GCodeProcessor processor;
    processor.initialize_result_moves();
    processor.apply_config(config);

    const std::string gcode =
        "M83\n"
        "G1 X0 Y0 Z0.2 F600\n;" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role) + "Outer wall\n;" +
        GCodeProcessor::reserved_tag(GCodeProcessor::ETags::CoExtrusion_Color) + "2\n"
        "G1 X10 Y0 E1 F1200\n;" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::CoExtrusion_Color) + "0\n"
        "G1 X10 Y10 E1 F1200\n";
    processor.process_buffer(gcode);

    const GCodeProcessorResult &result = processor.get_result();
    REQUIRE(result.coextrusion_colors == config.coextrusion_c_axis_colors.values);

    std::vector<unsigned char> external_wall_colors;
    for (const GCodeProcessorResult::MoveVertex &move : result.moves)
        if (move.type == EMoveType::Extrude && move.extrusion_role == erExternalPerimeter)
            external_wall_colors.emplace_back(move.coextrusion_color_id);
    REQUIRE(external_wall_colors == std::vector<unsigned char>{2, 0});
}
