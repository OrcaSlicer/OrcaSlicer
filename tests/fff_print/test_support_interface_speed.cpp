#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "test_helpers.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Circle.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/CoolingBuffer.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/GCode/ExtrusionProcessor.hpp"
#include "libslic3r/GCode/SupportInterfaceSpeed.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

static ExPolygon rectangle(double left, double right, double bottom = -1., double top = 1.)
{
    return ExPolygon(Polygon({Point::new_scale(left, bottom), Point::new_scale(right, bottom),
                              Point::new_scale(right, top), Point::new_scale(left, top)}));
}

TEST_CASE("Support speed splits crossing paths without slowing holes or reversing travel", "[SupportInterfaceSpeed]")
{
    const bool reverse = GENERATE(false, true);
    ExPolygon region = rectangle(2, 8);
    Polygon hole = rectangle(4, 6, -0.5, 0.5).contour;
    hole.reverse();
    region.holes.push_back(hole);
    std::vector<ProcessedPoint> points{
        {Point3::new_scale(0, 0, 0), 100.f, 0.8f},
        {Point3::new_scale(10, 0, 1), 100.f, 0.6f}};
    if (reverse)
        std::reverse(points.begin(), points.end());
    REQUIRE(limit_speed_above_support(points, {region}, ConfigOptionFloatOrPercent(25, false)));
    REQUIRE(points.size() == 6);
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const double mid = unscale_((points[i].p.x() + points[i + 1].p.x()) / 2);
        const bool above = (mid > 2 && mid < 4) || (mid > 6 && mid < 8);
        CHECK_THAT(points[i].speed, WithinAbs(above ? 25. : 100., 1e-5));
        CHECK_THAT(unscale_(points[i].p.z()), WithinAbs(unscale_(points[i].p.x()) / 10., 1e-5));
        CHECK_THAT(points[i].overlap, WithinAbs(0.8 - unscale_(points[i].p.x()) * 0.02, 1e-5));
        CHECK((points[i + 1].p.x() > points[i].p.x()) == !reverse);
    }
}

TEST_CASE("Support speed percentages use each segment speed and never raise it", "[SupportInterfaceSpeed]")
{
    std::vector<ProcessedPoint> points{
        {Point3::new_scale(0, 0, 0), 80.f, 1.f},
        {Point3::new_scale(5, 0, 0), 20.f, 1.f},
        {Point3::new_scale(10, 0, 0), 20.f, 1.f}};
    const double percent = GENERATE(0., 50., 100., 150.);
    const bool changed = limit_speed_above_support(points, {rectangle(-1, 11)}, ConfigOptionFloatOrPercent(percent, true));
    CHECK(changed == (percent == 50.));
    CHECK_THAT(points[0].speed, WithinAbs(changed ? 40. : 80., 1e-5));
    CHECK_THAT(points[1].speed, WithinAbs(changed ? 10. : 20., 1e-5));
}

TEST_CASE("Interfaces affect only the first visible model surface within the contact gap", "[SupportInterfaceSpeed]")
{
    Print print;
    Model model;
    init_print({cube(20)}, print, model, {{"support_top_z_distance", 0.2}});
    PrintObject *object = print.objects_mutable().front();
    auto *support = object->add_support_layer(0, 0, 0.2, 1.0);
    support->top_interface_polygons = {rectangle(0, 10)};
    Layer *first = object->add_layer(0, 0.2, 1.4, 1.3);
    first->lslices = {rectangle(0, 10)};
    Layer *second = object->add_layer(1, 0.2, 1.6, 1.5);
    second->lslices = first->lslices;
    REQUIRE_FALSE(support_interface_regions_below(*first).empty());
    CHECK(support_interface_regions_below(*second).empty());
    Layer *distant = object->add_layer(2, 0.2, 3., 2.9);
    CHECK(support_interface_regions_below(*distant).empty());

    auto *base = object->add_support_layer(1, 1, 0.1, 1.1);
    base->support_islands = support->top_interface_polygons;
    CHECK(support_interface_regions_below(*first).empty());
    object->clear_support_layers();
    CHECK(support_interface_regions_below(*first).empty());
}

TEST_CASE("Support contact detection allows independently sized support layers", "[SupportInterfaceSpeed]")
{
    Print print;
    Model model;
    init_print({cube(20)}, print, model, {{"support_top_z_distance", 0.2}});
    PrintObject *object = print.objects_mutable().front();
    auto *support = object->add_support_layer(0, 0, 0.2, 10.0);
    support->top_interface_polygons = {rectangle(0, 10)};

    // At upper adaptive layers the model layer may be thinner than the
    // independently generated support layer. The support height supplies the
    // rounding allowance in that case.
    Layer *thin_model_layer = object->add_layer(0, 0.08, 10.43, 10.39);
    thin_model_layer->lslices = {rectangle(0, 10)};
    CHECK_FALSE(support_interface_regions_below(*thin_model_layer).empty());
}

TEST_CASE("Fan-only support clipping follows interface boundaries", "[SupportInterfaceSpeed]")
{
    std::vector<ProcessedPoint> points{
        {Point3::new_scale(0, 0, 0), 80.f, 1.f},
        {Point3::new_scale(10, 0, 0), 80.f, 1.f}};
    REQUIRE(limit_speed_above_support(points, {rectangle(2, 8)}, ConfigOptionFloatOrPercent(0, false), true));
    REQUIRE(points.size() == 4);
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        CHECK_THAT(points[i].speed, WithinAbs(80., 1e-5));
        CHECK(points[i].above_support_interface == (i == 1));
    }
}

TEST_CASE("Support speed is blended toward normal speed on following layers", "[SupportInterfaceSpeed]")
{
    std::vector<ProcessedPoint> points{
        {Point3::new_scale(0, 0, 0), 100.f, 1.f},
        {Point3::new_scale(10, 0, 0), 100.f, 1.f}};
    REQUIRE(limit_speed_above_support(points, {rectangle(0, 10)}, ConfigOptionFloatOrPercent(20, false), false, 0.5));
    CHECK_THAT(points.front().speed, WithinAbs(60., 1e-5));
}

TEST_CASE("The support speed setting defaults to disabled and round trips percentages", "[SupportInterfaceSpeed]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    const auto *option = config.option<ConfigOptionFloatOrPercent>("first_layer_above_support_speed");
    REQUIRE(option != nullptr);
    CHECK_THAT(option->value, WithinAbs(0., 1e-8));
    config.set_deserialize_strict("first_layer_above_support_speed", "35%");
    CHECK(config.opt_serialize("first_layer_above_support_speed") == "35%");
    config.set_deserialize_strict("first_layer_above_support_speed", "25");
    CHECK(config.opt_serialize("first_layer_above_support_speed") == "25");
    CHECK(config.opt_serialize("first_layer_above_support_fan_speed") == "-1");
    config.set_deserialize_strict("first_layer_above_support_fan_speed", "80");
    CHECK(config.opt_serialize("first_layer_above_support_fan_speed") == "80");
    CHECK(config.opt_int("slow_layers_above_support") == 1);
    config.set_deserialize_strict("slow_layers_above_support", "25");
    CHECK(config.opt_int("slow_layers_above_support") == 25);
    CHECK_THAT(config.opt_float("slow_area_above_support_expansion"), WithinAbs(0., 1e-8));
    config.set_deserialize_strict("slow_area_above_support_expansion", "1.5");
    CHECK_THAT(config.opt_float("slow_area_above_support_expansion"), WithinAbs(1.5, 1e-8));
}

TEST_CASE("Generated model G-code uses the support cap for normal and tree interfaces", "[SupportInterfaceSpeed]")
{
    const auto [type, style] = GENERATE(table<const char *, const char *>({
        {"normal(auto)", "snug"}, {"tree(auto)", "organic"}, {"tree(auto)", "tree_slim"}}));
    CAPTURE(type, style);
    const int interface_layers = GENERATE(0, 3);
    CAPTURE(interface_layers);
    constexpr double cap = 17.;
    const std::string g = slice({TestMesh::overhang}, {
        {"enable_support", true}, {"support_type", type}, {"support_style", style},
        {"support_interface_top_layers", interface_layers}, {"support_top_z_distance", 0.2},
        {"layer_height", 0.2}, {"first_layer_above_support_speed", cap},
        {"outer_wall_speed", 100.}, {"inner_wall_speed", 100.},
        {"internal_solid_infill_speed", 100.}, {"bridge_speed", 100.},
        {"enable_overhang_speed", false}, {"slow_down_for_layer_cooling", false},
        {"support_speed", 60.}, {"support_interface_speed", 60.},
        {"gcode_comments", true}});
    size_t capped_moves = 0;
    GCodeReader reader;
    reader.parse_buffer(g, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.extruding(self) && std::abs(line.new_F(self) - cap * 60.) < 0.1) {
            ++capped_moves;
            CHECK(line.comment().find("support material") == std::string_view::npos);
        }
    });
    CHECK((capped_moves > 0) == (interface_layers > 0));
}

TEST_CASE("Model fan override above support works without a speed cap", "[SupportInterfaceSpeed]")
{
    const int interface_layers = GENERATE(0, 3);
    CAPTURE(interface_layers);
    const std::string g = slice({TestMesh::overhang}, {
        {"enable_support", true}, {"support_type", "normal(auto)"}, {"support_style", "snug"},
        {"support_interface_top_layers", interface_layers}, {"support_top_z_distance", 0.2},
        {"layer_height", 0.2}, {"first_layer_above_support_speed", 0.},
        {"first_layer_above_support_fan_speed", 80},
        {"close_fan_the_first_x_layers", 0}, {"fan_min_speed", 0}, {"fan_max_speed", 0},
        {"enable_overhang_bridge_fan", false}, {"slow_down_for_layer_cooling", false}});
    CHECK((g.find("M106 S204") != std::string::npos) == (interface_layers > 0));
    CHECK(g.find(";_ABOVE_SUPPORT_FAN_START") == std::string::npos);
    CHECK(g.find(";_ABOVE_SUPPORT_FAN_END") == std::string::npos);
}

TEST_CASE("Tree support wall loops are not capped at two", "[SupportWallLoops]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict("tree_support_wall_count", "5");
    CHECK(config.opt_int("tree_support_wall_count") == 5);
}


TEST_CASE("Overlapping support limits use the original speed in either order", "[SupportInterfaceSpeed][Regression]")
{
    const bool reverse = GENERATE(false, true);
    std::vector<ProcessedPoint> points{
        {Point3::new_scale(0, 0, 0), 100.f, 0.4f},
        {Point3::new_scale(10, 0, 1), 100.f, 0.6f}};
    const auto apply = [&](double blend) {
        REQUIRE(limit_speed_above_support(points, {rectangle(2, 8)}, ConfigOptionFloatOrPercent(50, true), true, blend));
    };
    apply(reverse ? 0.5 : 0.);
    apply(reverse ? 0. : 0.5);
    REQUIRE(points.size() == 4);
    CHECK_THAT(points[1].speed, WithinAbs(50., 1e-5));
    CHECK_THAT(points[1].above_support_blend_to_normal, WithinAbs(0., 1e-5));
    CHECK_THAT(points[0].speed, WithinAbs(100., 1e-5));
    CHECK_THAT(points[2].speed, WithinAbs(100., 1e-5));
    CHECK_THAT(points[1].overlap, WithinAbs(0.44, 1e-5));
    CHECK_THAT(unscale_(points[1].p.z()), WithinAbs(0.2, 1e-5));
}

TEST_CASE("Batch support clipping preserves retraced paths and their original speeds", "[SupportInterfaceSpeed][Regression]")
{
    std::vector<ProcessedPoint> points{
        {Point3::new_scale(0, 0, 0), 100.f, 0.2f},
        {Point3::new_scale(10, 0, 0), 40.f, 0.4f},
        {Point3::new_scale(0, 0, 0), 40.f, 0.6f}};
    REQUIRE(limit_speed_above_support(points, {rectangle(2, 8)}, ConfigOptionFloatOrPercent(50, true)));
    REQUIRE(points.size() == 7);
    CHECK_THAT(points[1].speed, WithinAbs(50., 1e-5));
    CHECK_THAT(points[4].speed, WithinAbs(20., 1e-5));
    CHECK(points[1].p.x() < points[2].p.x());
    CHECK(points[4].p.x() > points[5].p.x());
}

TEST_CASE("Fan-only arc spans retain the circle and select the strongest overlap", "[SupportInterfaceSpeed][Regression]")
{
    const bool clockwise = GENERATE(false, true);
    const Point center = Point::new_scale(0, 0);
    const ArcSegment arc(center, scale_(10.), Point::new_scale(10, 0), Point::new_scale(-10, 0),
        clockwise ? ArcDirection::Arc_Dir_CW : ArcDirection::Arc_Dir_CCW);
    const std::vector<std::pair<ExPolygons, double>> regions{
        {{rectangle(-20, 0, -20, 20)}, 0.5}, {{rectangle(-20, -5, -20, 20)}, 0.}};
    const auto spans = support_fan_spans(arc, regions);
    REQUIRE(spans.size() == 3);
    CHECK_THAT(spans[0].end, WithinAbs(0.5, 1e-6));
    CHECK_THAT(spans[0].blend, WithinAbs(-1., 1e-6));
    CHECK_THAT(spans[1].blend, WithinAbs(0.5, 1e-6));
    CHECK_THAT(spans[2].blend, WithinAbs(0., 1e-6));
    CHECK(support_arc_point(arc, 0.) == arc.start_point);
    CHECK(support_arc_point(arc, 1.) == arc.end_point);
    for (const SupportFanSpan &span : spans) {
        const Point p = support_arc_point(arc, span.end);
        CHECK_THAT((p - center).cast<double>().norm(), WithinAbs(arc.radius, 2.));
    }
}

TEST_CASE("Support interface geometry is cached per layer until the next export", "[SupportInterfaceSpeed][Regression]")
{
    Print print;
    Model model;
    init_print({cube(20)}, print, model, {{"support_top_z_distance", 0.2}});
    PrintObject *object = print.objects_mutable().front();
    SupportLayer *support = object->add_support_layer(0, 0, 0.2, 1.0);
    support->top_interface_polygons = {rectangle(0, 10)};
    Layer *layer = object->add_layer(0, 0.2, 1.4, 1.3);
    layer->lslices = {rectangle(0, 10)};
    SupportInterfaceRegionCache cache;
    const ExPolygons *cached = &cache.regions(*layer);
    REQUIRE_FALSE(cached->empty());
    CHECK(&cache.regions(*layer) == cached);
    support->top_interface_polygons.clear();
    CHECK_FALSE(cache.regions(*layer).empty());
    cache.clear();
    CHECK(cache.regions(*layer).empty());
}

static std::string cool_support_markers(const DynamicPrintConfig &dynamic, std::string gcode, size_t layer_id)
{
    PrintConfig config;
    config.apply(dynamic);
    GCode generator;
    generator.apply_print_config(config);
    generator.writer().set_extruders({0});
    CoolingBuffer cooling(generator);
    return cooling.process_layer(std::move(gcode), layer_id, true);
}

static DynamicPrintConfig support_cooling_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"fan_min_speed", 20}, {"fan_max_speed", 20}, {"reduce_fan_stop_start_freq", true},
        {"slow_down_for_layer_cooling", false}, {"close_fan_the_first_x_layers", 0},
        {"initial_layer_fan_speed", -1}, {"full_fan_speed_layer", 0},
        {"first_layer_above_support_fan_speed", 80}, {"gcode_flavor", "marlin"},
        {"part_cooling_fan_min_pwm", 0}, {"use_relative_e_distances", true}});
    return config;
}

TEST_CASE("Cooling parses nonzero support fan blends and removes their markers", "[SupportInterfaceSpeed][Regression]")
{
    const auto [blend, speed] = GENERATE(table<const char *, int>({{"B0", 80}, {"B50", 50}, {"B100", 20}}));
    const std::string input = std::string("G1 X0 Y0 F600
;_ABOVE_SUPPORT_FAN_START ") + blend +
        "
G1 X10 Y0 E1 F600
;_ABOVE_SUPPORT_FAN_END
G1 X20 Y0 E1 F600
";
    const std::string output = cool_support_markers(support_cooling_config(), input, 5);
    CHECK(output.find(GCodeWriter::set_fan(gcfMarlinLegacy, speed, 0)) != std::string::npos);
    CHECK(output.find("_ABOVE_SUPPORT_FAN") == std::string::npos);
    if (speed != 80)
        CHECK(output.find(GCodeWriter::set_fan(gcfMarlinLegacy, 80, 0)) == std::string::npos);
}

TEST_CASE("Above-support cooling follows the fan ramp and its initial-layer anchor", "[SupportInterfaceSpeed][Regression]")
{
    const bool anchored = GENERATE(false, true);
    DynamicPrintConfig config = support_cooling_config();
    config.set_deserialize_strict({{"first_layer_above_support_fan_speed", 100}, {"full_fan_speed_layer", 5},
        {"initial_layer_fan_speed", anchored ? 20 : -1}});
    const std::string input = "G1 X0 Y0 F600
;_ABOVE_SUPPORT_FAN_START B0
G1 X10 Y0 E1 F600
;_ABOVE_SUPPORT_FAN_END
";
    // Second physical layer: 2/5 from zero, or 1/4 from an explicit 20% anchor.
    const std::string output = cool_support_markers(config, input, 1);
    CHECK(output.find(GCodeWriter::set_fan(gcfMarlinLegacy, 40, 0)) != std::string::npos);
    CHECK(output.find(GCodeWriter::set_fan(gcfMarlinLegacy, 100, 0)) == std::string::npos);
}

TEST_CASE("Above-support cooling respects suppressed layers and overhang cooling", "[SupportInterfaceSpeed][Regression]")
{
    const bool suppressed = GENERATE(false, true);
    DynamicPrintConfig config = support_cooling_config();
    config.set_deserialize_strict({{"overhang_fan_speed", 100}, {"first_layer_above_support_fan_speed", 40},
        {"close_fan_the_first_x_layers", suppressed ? 2 : 0}});
    const std::string input = "G1 X0 Y0 F600
;_OVERHANG_FAN_START
;_ABOVE_SUPPORT_FAN_START B0
"
        "G1 X10 Y0 E1 F600
;_ABOVE_SUPPORT_FAN_END
;_OVERHANG_FAN_END
";
    const std::string output = cool_support_markers(config, input, 1);
    CHECK((output.find(GCodeWriter::set_fan(gcfMarlinLegacy, 100, 0)) != std::string::npos) == !suppressed);
    CHECK(output.find(GCodeWriter::set_fan(gcfMarlinLegacy, 40, 0)) == std::string::npos);
}

TEST_CASE("Above-support fan changes invalidate only the G-code export", "[SupportInterfaceSpeed][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict("first_layer_above_support_fan_speed", "-1");
    Print print;
    Model model;
    init_print({cube(2.)}, print, model, config);
    print.process();
    print.set_gcode_file_ready();
    REQUIRE(print.is_step_done(psGCodeExport));
    REQUIRE(print.is_step_done(psWipeTower));
    REQUIRE(print.is_step_done(psSkirtBrim));
    config.set_deserialize_strict("first_layer_above_support_fan_speed", "80");
    print.apply(model, config);
    CHECK_FALSE(print.is_step_done(psGCodeExport));
    CHECK(print.is_step_done(psWipeTower));
    CHECK(print.is_step_done(psSkirtBrim));
    CHECK(print.objects().front()->is_step_done(posSlice));
    CHECK(print.objects().front()->is_step_done(posPerimeters));
}


TEST_CASE("Fan-only above-support control preserves fitted arcs and extrusion feedrates", "[SupportInterfaceSpeed][Regression]")
{
    // A round shelf on a narrower pedestal gives the supported model walls
    // genuine arcs. A plain cube would not detect a fallback from G2/G3 to G1.
    TriangleMesh shelf = make_cylinder(15., 3., 2. * PI / 360.);
    shelf.translate(0., 0., 5.);
    TriangleMesh part = make_cylinder(5., 5., 2. * PI / 360.);
    part.merge(shelf);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_arc_fitting", "enabled"}, {"enable_support", true},
        {"support_type", "normal(auto)"}, {"support_style", "snug"},
        {"support_interface_top_layers", 3}, {"support_top_z_distance", 0.2},
        {"layer_height", 0.2}, {"first_layer_above_support_speed", 0.},
        {"first_layer_above_support_fan_speed", -1}, {"enable_overhang_speed", false},
        {"enable_overhang_bridge_fan", false}, {"slow_down_for_layer_cooling", false},
        {"close_fan_the_first_x_layers", 0}, {"full_fan_speed_layer", 0},
        {"fan_min_speed", 0}, {"fan_max_speed", 0}, {"z_hop", 0}});
    const std::string baseline = slice({part}, config);
    config.set_deserialize_strict("first_layer_above_support_fan_speed", "80");
    const std::string fan_only = slice({part}, config);
    const auto arc_count = [](const std::string &gcode) {
        size_t count = 0;
        std::istringstream input(gcode);
        std::string line;
        while (std::getline(input, line))
            if (line.rfind("G2 ", 0) == 0 || line.rfind("G3 ", 0) == 0)
                ++count;
        return count;
    };
    REQUIRE(arc_count(baseline) > 0);
    CHECK(arc_count(fan_only) >= arc_count(baseline));
    CHECK(fan_only.find("M106 S204") != std::string::npos);
    const auto feedrates = [](const std::string &gcode) {
        std::vector<double> speeds;
        GCodeReader reader;
        reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
            if (line.extruding(self)) {
                const double speed = line.new_F(self);
                if (speeds.empty() || std::abs(speeds.back() - speed) > 0.01)
                    speeds.push_back(speed);
            }
        });
        return speeds;
    };
    const auto expected = feedrates(baseline);
    const auto actual = feedrates(fan_only);
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        CHECK_THAT(actual[i], WithinAbs(expected[i], 0.01));
}
