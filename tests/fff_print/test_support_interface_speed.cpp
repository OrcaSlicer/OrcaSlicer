#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "test_helpers.hpp"
#include "libslic3r/Config.hpp"
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
