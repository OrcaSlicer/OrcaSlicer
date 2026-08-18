#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/CoExtrusionC.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

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

TEST_CASE("Color mapping uses an independent nearest match", "[CoExtrusionC]")
{
    const std::vector<size_t> mapping = map_coextrusion_filament_colors_to_sectors(
        {"#F01010", "#10F010", "#1010F0"},
        {"#FF0000", "#00FF00", "#0000FF"});

    REQUIRE(mapping == std::vector<size_t>{0, 1, 2});
}

TEST_CASE("More logical colors than physical sectors map many-to-one", "[CoExtrusionC]")
{
    const std::vector<size_t> mapping = map_coextrusion_filament_colors_to_sectors(
        {"#FFFFFF", "#00C1AE", "#F4E2C1", "#0000FF"},
        {"#FFFFFF", "#00FF00", "#0000FF"});

    REQUIRE(mapping == std::vector<size_t>{0, 1, 0, 2});

    FullPrintConfig config;
    config.single_extruder_multi_material.value = true;
    config.coextrusion_c_axis_enable.value = true;
    config.filament_colour.values = {"#FFFFFF", "#00C1AE", "#F4E2C1", "#0000FF"};
    config.coextrusion_c_axis_colors.values = {"#FFFFFF", "#00FF00", "#0000FF"};
    config.coextrusion_c_axis_color_angles.values = {0., 120., 240.};
    REQUIRE(validate(config).count("coextrusion_c_axis_color_angles") == 0);
}

TEST_CASE("Explicit co-extrusion mapping overrides nearest color and keeps auto entries", "[CoExtrusionC]")
{
    const std::vector<size_t> mapping = map_coextrusion_filament_colors_to_sectors(
        {"#FF0000", "#00FF00", "#0000FF"},
        {"#FF0000", "#00FF00", "#0000FF"},
        {3, 0, 1});

    REQUIRE(mapping == std::vector<size_t>{2, 1, 0});
}

TEST_CASE("Invalid explicit co-extrusion sectors remain safely unmapped", "[CoExtrusionC]")
{
    const std::vector<size_t> mapping = map_coextrusion_filament_colors_to_sectors(
        {"#FF0000"}, {"#FF0000", "#00FF00"}, {4});

    REQUIRE(mapping == std::vector<size_t>{std::numeric_limits<size_t>::max()});
}

TEST_CASE("Co-extrusion face colors survive logical filament deletion", "[CoExtrusionC]")
{
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(make_cube(10., 10., 10.));

    volume->mmu_segmentation_facets.reserve(volume->mesh().facets_count());
    // This is the existing 3MF encoding of a whole face painted with logical filament 3.
    volume->mmu_segmentation_facets.set_triangle_from_string(0, "0C");
    volume->coextrusion_segmentation_facets.assign(volume->mmu_segmentation_facets);
    const std::string source_paint = volume->coextrusion_segmentation_facets.get_triangle_as_string(0);

    volume->update_extruder_count_when_delete_filament(2, 3, 1);

    REQUIRE(volume->mmu_segmentation_facets.get_triangle_as_string(0) != source_paint);
    REQUIRE(volume->coextrusion_segmentation_facets.get_triangle_as_string(0) == source_paint);
}

TEST_CASE("Co-extrusion source face colors survive a 3MF round trip", "[CoExtrusionC]")
{
    Model source_model;
    ModelObject *object = source_model.add_object();
    ModelVolume *volume = object->add_volume(make_cube(10., 10., 10.));
    object->add_instance();
    volume->coextrusion_segmentation_facets.reserve(volume->mesh().facets_count());
    volume->coextrusion_segmentation_facets.set_triangle_from_string(0, "0C");

    const boost::filesystem::path path = boost::filesystem::temp_directory_path() /
                                         boost::filesystem::unique_path("orca-coextrusion-%%%%-%%%%.3mf");
    REQUIRE(store_3mf(path.string().c_str(), &source_model, nullptr, false));

    Model loaded_model;
    DynamicPrintConfig loaded_config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    const bool loaded = load_3mf(path.string().c_str(), loaded_config, substitutions, &loaded_model, false);
    boost::filesystem::remove(path);

    REQUIRE(loaded);
    REQUIRE(loaded_model.objects.size() == 1);
    REQUIRE(loaded_model.objects.front()->volumes.size() == 1);
    REQUIRE(loaded_model.objects.front()->volumes.front()->coextrusion_segmentation_facets.get_triangle_as_string(0) == "0C");
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

TEST_CASE("G-code preview uses filament-owned co-extrusion colors", "[CoExtrusionC]")
{
    FullPrintConfig config;
    config.coextrusion_c_axis_enable.value = true;
    config.filament_coextrusion_enable.value = true;
    config.filament_coextrusion_colors.values = {"#FFFFFF", "#00FF00", "#0000FF"};
    config.coextrusion_c_axis_colors.values = {"#FF0000"};

    GCodeProcessor processor;
    processor.initialize_result_moves();
    processor.apply_config(config);

    REQUIRE(processor.get_result().coextrusion_colors == config.filament_coextrusion_colors.values);
}

TEST_CASE("G-code preview clears an invalid co-extrusion color sector", "[CoExtrusionC]")
{
    FullPrintConfig config;
    config.coextrusion_c_axis_enable.value = true;
    config.coextrusion_c_axis_colors.values = {"#FFFFFF", "#00FF00", "#0000FF"};

    GCodeProcessor processor;
    processor.initialize_result_moves();
    processor.apply_config(config);

    const std::string gcode =
        "M83\n"
        "G1 X0 Y0 Z0.2 F600\n;" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role) + "Outer wall\n;" +
        GCodeProcessor::reserved_tag(GCodeProcessor::ETags::CoExtrusion_Color) + "2\n"
        "G1 X10 Y0 E1 F1200\n;" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::CoExtrusion_Color) + "999\n"
        "G1 X10 Y10 E1 F1200\n";
    processor.process_buffer(gcode);

    std::vector<unsigned char> external_wall_colors;
    for (const GCodeProcessorResult::MoveVertex &move : processor.get_result().moves)
        if (move.type == EMoveType::Extrude && move.extrusion_role == erExternalPerimeter)
            external_wall_colors.emplace_back(move.coextrusion_color_id);
    REQUIRE(external_wall_colors == std::vector<unsigned char>{2, COEXTRUSION_COLOR_ID_NONE});
}
