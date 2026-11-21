// The colour ranges of libvgcode's graded views: the stops of each scale and the colours at them, and the Overhang
// view's colours and settings that need no loaded G-code.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <vector>

#include "libvgcode/include/ColorRange.hpp"
#include "libvgcode/include/PathVertex.hpp"
#include "libvgcode/include/Types.hpp"
#include "libvgcode/include/Viewer.hpp"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using libvgcode::Color;
using libvgcode::ColorRange;
using libvgcode::EColorRangeType;
using libvgcode::EViewType;
using libvgcode::Palette;

namespace {

// ColorRange::update() is private: ViewerImpl fills the ranges once G-code is loaded, which takes an OpenGL context.
// The names in an explicit instantiation are not access checked, so RangeUpdater can take its address and define
// update_range() around it.
void update_range(ColorRange &range, float value);

template<void (ColorRange::*Update)(float)> struct RangeUpdater
{
    friend void update_range(ColorRange &range, float value) { (range.*Update)(value); }
};
template struct RangeUpdater<&ColorRange::update>;

// A range of the given type and palette over `values`, all distinct, as ViewerImpl builds it from the G-code.
ColorRange range_of(EColorRangeType type, std::initializer_list<float> values,
                    const Palette &palette = libvgcode::DEFAULT_RANGES_COLORS)
{
    ColorRange range(type);
    range.set_palette(palette);
    for (const float value : values)
        update_range(range, value);
    return range;
}

// Colours channel by channel. Interpolating truncates each channel, so a value a hair below a stop may lose one.
void check_color(const Color &actual, const Color &expected)
{
    for (size_t channel = 0; channel < actual.size(); ++channel)
        CHECK_THAT(double(actual[channel]), WithinAbs(double(expected[channel]), 1.0));
}

} // namespace

// LogarithmicWithZero grades by log(1 + x), so a range from zero, which a plain logarithm cannot take, still gives
// small values more colours. Its legend stops and its colours agree: stop i of n reads (1 + top)^(i / n) - 1 and takes
// the palette's colour i. The ends are kept exact rather than recomputed through the logarithm.
TEST_CASE("A zero-based logarithmic range spaces its stops by the logarithm of one more than the value", "[Libvgcode]")
{
    // the Overhang view's ranges: 0 to 90 degrees and 0 to 100 %
    const float top = GENERATE(90.0f, 100.0f);
    const ColorRange range = range_of(EColorRangeType::LogarithmicWithZero, {0.0f, 0.5f * top, top});
    const Palette &palette = range.get_palette();
    const std::vector<float> values = range.get_values();
    REQUIRE(palette.size() == 11);
    REQUIRE(values.size() == palette.size());
    CHECK_THAT(values.front(), WithinAbs(0.0, 1e-6));
    CHECK_THAT(values.back(), WithinAbs(top, 1e-6));
    const double steps = double(palette.size() - 1);
    for (size_t i = 0; i < values.size(); ++i) {
        INFO("stop " << i);
        CHECK_THAT(values[i], WithinRel(std::pow(1.0 + top, double(i) / steps) - 1.0, 1e-5));
        check_color(range.get_color_at(values[i]), palette[i]);
    }
    // the middle stop is sqrt(1 + top) - 1: 8.54 of 90 degrees, 9.05 of 100 %, where a linear range has 45 and 50
    CHECK_THAT(values[5], WithinAbs(std::sqrt(1.0 + top) - 1.0, 1e-3));
}

// The Overhang view's scale toggle retypes its ranges in place: the bounds the G-code gave and a palette the user
// chose stay, and switching back gives the linear stops again.
TEST_CASE("Changing a range's type keeps its bounds and palette", "[Libvgcode]")
{
    const Palette palette{{0, 0, 255}, {0, 255, 0}, {255, 0, 0}};
    ColorRange range = range_of(EColorRangeType::Linear, {0.0f, 50.0f, 100.0f}, palette);
    range.set_type(EColorRangeType::LogarithmicWithZero);
    CHECK(range.get_type() == EColorRangeType::LogarithmicWithZero);
    CHECK_THAT(range.get_range()[0], WithinAbs(0.0, 1e-6));
    CHECK_THAT(range.get_range()[1], WithinAbs(100.0, 1e-6));
    CHECK(range.get_palette() == palette);
    // three colours, two steps: the middle stop is sqrt(101) - 1
    std::vector<float> values = range.get_values();
    REQUIRE(values.size() == 3);
    CHECK_THAT(values[1], WithinRel(std::sqrt(101.0) - 1.0, 1e-5));
    check_color(range.get_color_at(values[1]), palette[1]);

    range.set_type(EColorRangeType::Linear);
    values = range.get_values();
    REQUIRE(values.size() == 3);
    CHECK_THAT(values[0], WithinAbs(0.0, 1e-6));
    CHECK_THAT(values[1], WithinAbs(50.0, 1e-6));
    CHECK_THAT(values[2], WithinAbs(100.0, 1e-6));
    CHECK(range.get_palette() == palette);
}

// The zero-based type is the Overhang view's alone: a linear range and Layer Time's logarithmic one keep their stops,
// and the viewer keeps one range of each type for Layer Time.
TEST_CASE("Linear and layer time logarithmic ranges keep their stops", "[Libvgcode]")
{
    const std::vector<float> linear = range_of(EColorRangeType::Linear, {0.0f, 45.0f, 90.0f}).get_values();
    REQUIRE(linear.size() == 11);
    for (size_t i = 0; i < linear.size(); ++i) {
        INFO("stop " << i);
        CHECK_THAT(linear[i], WithinAbs(9.0 * double(i), 1e-4));
    }
    // ten steps over two decades: stop i reads 10^(i / 5)
    const std::vector<float> logarithmic = range_of(EColorRangeType::Logarithmic, {1.0f, 10.0f, 100.0f}).get_values();
    REQUIRE(logarithmic.size() == 11);
    for (size_t i = 0; i < logarithmic.size(); ++i) {
        INFO("stop " << i);
        CHECK_THAT(logarithmic[i], WithinRel(std::pow(10.0, double(i) / 5.0), 1e-5));
    }
    // no OpenGL call is reached before init(), so a viewer needs no context to show its settings
    const libvgcode::Viewer viewer;
    CHECK(viewer.get_color_range(EViewType::LayerTimeLinear).get_type() == EColorRangeType::Linear);
    CHECK(viewer.get_color_range(EViewType::LayerTimeLogarithmic).get_type() == EColorRangeType::Logarithmic);
}

// The Overhang view greys a line without data, such as a support, in either unit and on either scale, and leaves a
// travel the travel colour, even one without data.
TEST_CASE("The overhang view greys lines without data and keeps the travel colour", "[Libvgcode][Overhang][Regression]")
{
    const bool percentage  = GENERATE(false, true);
    const bool logarithmic = GENERATE(false, true);
    libvgcode::Viewer viewer;
    viewer.set_view_type(EViewType::Overhang);
    viewer.set_overhang_percentage(percentage);
    viewer.set_overhang_logarithmic(logarithmic);

    libvgcode::PathVertex support;
    support.type                = libvgcode::EMoveType::Extrude;
    support.role                = libvgcode::EGCodeExtrusionRole::SupportMaterial;
    support.overhang_percentage = -1.0f;
    CHECK(viewer.get_vertex_color(support) == libvgcode::NO_OVERHANG_COLOR);

    libvgcode::PathVertex travel;
    travel.type                = libvgcode::EMoveType::Travel;
    travel.overhang_percentage = -1.0f;
    const Color &travel_color = viewer.get_option_color(libvgcode::EOptionType::Travels);
    REQUIRE(travel_color != libvgcode::NO_OVERHANG_COLOR);
    CHECK(viewer.get_vertex_color(travel) == travel_color);
}

// One toggle sets the scale of both overhang units and one palette colours both, so switching units keeps what the
// legend shows. Layer Time's ranges keep their own types.
TEST_CASE("The overhang scale and palette apply to both units", "[Libvgcode][Overhang][Regression]")
{
    libvgcode::Viewer viewer;
    // the angle is the default unit
    REQUIRE_FALSE(viewer.is_overhang_percentage());
    CHECK(viewer.get_color_range(EViewType::Overhang).get_type() == EColorRangeType::Linear);

    viewer.set_overhang_logarithmic(true);
    CHECK(viewer.get_color_range(EViewType::Overhang).get_type() == EColorRangeType::LogarithmicWithZero);
    viewer.set_overhang_percentage(true);
    CHECK(viewer.get_color_range(EViewType::Overhang).get_type() == EColorRangeType::LogarithmicWithZero);
    CHECK(viewer.get_color_range(EViewType::LayerTimeLinear).get_type() == EColorRangeType::Linear);
    CHECK(viewer.get_color_range(EViewType::LayerTimeLogarithmic).get_type() == EColorRangeType::Logarithmic);

    viewer.set_overhang_logarithmic(false);
    CHECK(viewer.get_color_range(EViewType::Overhang).get_type() == EColorRangeType::Linear);
    viewer.set_overhang_percentage(false);
    CHECK(viewer.get_color_range(EViewType::Overhang).get_type() == EColorRangeType::Linear);

    const Palette palette{{0, 0, 255}, {255, 0, 0}};
    viewer.set_color_range_palette(EViewType::Overhang, palette);
    CHECK(viewer.get_color_range(EViewType::Overhang).get_palette() == palette);
    viewer.set_overhang_percentage(true);
    CHECK(viewer.get_color_range(EViewType::Overhang).get_palette() == palette);
}
