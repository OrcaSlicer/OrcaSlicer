#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/LegendAxis.hpp"
#include "slic3r/GUI/LegendText.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

namespace {

// Ten bins of 10 over 0..100: print time in 20..30 and 60..70, a trace in 80..90, none elsewhere.
LegendBins axis_bins()
{
    LegendBins bins;
    for (int i = 0; i <= 10; ++i)
        bins.edges.push_back(10.0 * i);
    bins.amounts = {0.0, 0.0, 600.0, 0.0, 0.0, 0.0, 400.0, 0.0, 1.0, 0.0};
    return bins;
}

float width(const LegendAxisPiece& piece) { return piece.x1 - piece.x0; }

void check_contiguous(const std::vector<LegendAxisPiece>& pieces, float left)
{
    REQUIRE_FALSE(pieces.empty());
    CHECK_THAT(pieces.front().x0, WithinAbs(left, 1e-6));
    for (size_t i = 1; i < pieces.size(); ++i)
        CHECK_THAT(pieces[i].x0, WithinAbs(pieces[i - 1].x1, 1e-6));
}

void check_range(const std::optional<std::array<float, 2>>& range, float lo, float hi)
{
    REQUIRE(range.has_value());
    CHECK_THAT((*range)[0], WithinAbs(lo, 1e-4));
    CHECK_THAT((*range)[1], WithinAbs(hi, 1e-4));
}

class Gesture
{
public:
    explicit Gesture(std::optional<std::array<float, 2>> filter = std::nullopt)
        : m_bins(axis_bins()), m_axis(0.f, 100.f, false, m_bins, 0.f, 300.f, 1000.f), m_filter(filter)
    {}

    const LegendAxis&                          axis() const { return m_axis; }
    const std::optional<std::array<float, 2>>& filter() const { return m_filter; }
    const std::optional<std::array<float, 2>>& popover() const { return m_popover; }

    LegendBrushStep step(float x, bool pressed, bool held, bool double_clicked = false, bool shift = false)
    {
        LegendBrushInput input;
        input.filter           = m_filter;
        input.x                = x;
        input.hovered          = true;
        input.pressed          = pressed;
        input.held             = held;
        input.double_clicked   = double_clicked;
        input.pressed_anywhere = pressed;
        input.shift            = shift;
        input.left             = 0.f;
        input.right            = 300.f;
        input.edge_grab        = 12.f;
        input.drag_threshold   = 4.f;
        const LegendBrushStep step = legend_brush_step(m_brush, input, m_axis);
        if (step.range_set)
            m_filter = step.range;
        if (step.popover)
            m_popover = step.popover;
        return step;
    }
    LegendBrushStep press(float x, bool double_clicked = false, bool shift = false) { return step(x, true, true, double_clicked, shift); }
    LegendBrushStep move(float x) { return step(x, false, true); }
    LegendBrushStep release(float x, bool shift = false) { return step(x, false, false, false, shift); }
    void            click(float x, bool shift = false)
    {
        press(x, false, shift);
        release(x, shift);
    }

private:
    LegendBins                          m_bins;
    LegendAxis                          m_axis;
    LegendBrush                         m_brush;
    std::optional<std::array<float, 2>> m_filter;
    std::optional<std::array<float, 2>> m_popover;
};

} // namespace

TEST_CASE("The axis gives the bins with print time the width and shrinks the rest to slivers", "[LegendAxis]")
{
    const LegendBins bins = axis_bins();
    const LegendAxis axis(0.f, 100.f, false, bins, 0.f, 300.f, 1000.f);
    const auto&      pieces = axis.pieces();
    REQUIRE(pieces.size() == 5);
    CHECK(pieces[0].empty);
    CHECK_FALSE(pieces[1].empty);
    CHECK(pieces[2].empty);
    CHECK_THAT(pieces[2].lo, WithinAbs(30.0, 1e-9));
    CHECK_THAT(pieces[2].hi, WithinAbs(60.0, 1e-9));
    CHECK(pieces[4].empty);
    CHECK_THAT(pieces[4].lo, WithinAbs(70.0, 1e-9));
    CHECK_THAT(pieces[4].hi, WithinAbs(100.0, 1e-9));
    CHECK_THAT(width(pieces[1]), WithinAbs(width(pieces[3]), 1e-4));
    CHECK(width(pieces[2]) < .2f * width(pieces[1]));
    check_contiguous(pieces, 0.f);
    CHECK_THAT(width(pieces[4]), WithinAbs(width(pieces[0]), 1e-3));
    CHECK_THAT(width(pieces[2]), WithinAbs(width(pieces[0]), 1e-3));
}

TEST_CASE("A full bin of the axis is at most as wide as it may be, the slivers share the rest", "[LegendAxis]")
{
    const LegendBins bins = axis_bins();
    const LegendAxis axis(0.f, 100.f, false, bins, 0.f, 300.f, 24.f);
    const auto&      pieces = axis.pieces();
    REQUIRE(pieces.size() == 5);
    CHECK_THAT(width(pieces[1]), WithinAbs(24.0, 1e-4));
    CHECK_THAT(width(pieces[3]), WithinAbs(24.0, 1e-4));
    check_contiguous(pieces, 0.f);
    CHECK(width(pieces[0]) > 24.f);
    CHECK_THAT(width(pieces[2]), WithinAbs(width(pieces[0]), 1e-3));
    CHECK_THAT(width(pieces[4]), WithinAbs(width(pieces[0]), 1e-3));
}

TEST_CASE("A place on the axis and its value map to each other", "[LegendAxis]")
{
    const LegendBins bins = axis_bins();
    const LegendAxis axis(0.f, 100.f, false, bins, 0.f, 300.f, 1000.f);
    for (const double value : {20.0, 25.0, 45.0, 64.0, 85.0}) {
        CAPTURE(value);
        CHECK_THAT(axis.value_on(axis.x_on(value)), WithinAbs(value, 1e-3));
    }
    CHECK(axis.x_on(25.0) < axis.x_on(64.0));
}

TEST_CASE("A bin with print time is found under its place, an empty one is not", "[LegendAxis]")
{
    const LegendBins bins = axis_bins();
    const LegendAxis axis(0.f, 100.f, false, bins, 0.f, 300.f, 1000.f);
    CHECK(axis.bin_at(axis.x_on(25.0)) == std::optional<size_t>(2));
    CHECK_FALSE(axis.bin_at(axis.x_on(45.0)).has_value());
    const std::array<float, 2> range = axis.bin_range(6);
    CHECK_THAT(range[0], WithinAbs(60.0, 1e-6));
    CHECK_THAT(range[1], WithinAbs(70.0, 1e-6));
}

TEST_CASE("A brush keeps the values it spans out to the edges of the bins at its ends", "[LegendAxis]")
{
    const LegendBins           bins  = axis_bins();
    const LegendAxis           axis(0.f, 100.f, false, bins, 0.f, 300.f, 1000.f);
    const std::array<float, 2> range = axis.brushed(axis.x_on(64.0), axis.x_on(25.0));
    CHECK_THAT(range[0], WithinAbs(20.0, 1e-4));
    CHECK_THAT(range[1], WithinAbs(70.0, 1e-4));
}

TEST_CASE("A range over the whole scale filters nothing", "[LegendAxis]")
{
    CHECK(legend_range_whole({0.f, 100.f}, 0.f, 100.f));
    CHECK(legend_range_whole({-1.f, 120.f}, 0.f, 100.f));
    CHECK_FALSE(legend_range_whole({0.f, 99.f}, 0.f, 100.f));
}

TEST_CASE("Hovering a bar shows its values, and a press keeps nothing until it is let go as a click on that bar", "[LegendAxis]")
{
    Gesture     gesture;
    const float x = gesture.axis().x_on(64.0);
    check_range(gesture.step(x, false, false).hover, 60.f, 70.f);

    const LegendBrushStep pressed = gesture.press(x);
    CHECK_FALSE(pressed.range_set);
    CHECK_FALSE(gesture.filter().has_value());

    const LegendBrushStep released = gesture.release(x);
    CHECK(released.range_set);
    check_range(gesture.filter(), 60.f, 70.f);
}

TEST_CASE("A click on the only bar kept clears the filter", "[LegendAxis]")
{
    Gesture gesture;
    gesture.click(gesture.axis().x_on(25.0));
    check_range(gesture.filter(), 20.f, 30.f);
    gesture.click(gesture.axis().x_on(25.0));
    CHECK_FALSE(gesture.filter().has_value());
}

TEST_CASE("A Shift+click adds the bar to the values kept", "[LegendAxis]")
{
    Gesture gesture;
    gesture.click(gesture.axis().x_on(25.0));
    gesture.click(gesture.axis().x_on(64.0), true);
    check_range(gesture.filter(), 20.f, 70.f);
}

TEST_CASE("A drag across bars keeps the values it spans out to the edges of the bins at its ends", "[LegendAxis]")
{
    Gesture gesture;
    gesture.press(gesture.axis().x_on(25.0));
    gesture.move(gesture.axis().x_on(64.0));
    check_range(gesture.filter(), 20.f, 70.f);
    const LegendBrushStep released = gesture.release(gesture.axis().x_on(64.0));
    CHECK_FALSE(released.range_set);
    check_range(gesture.filter(), 20.f, 70.f);
}

TEST_CASE("An edge of the range drags on its own, and dragged to the end of the scale clears the filter", "[LegendAxis]")
{
    Gesture     gesture(std::array<float, 2>{0.f, 70.f});
    const float edge = gesture.axis().x_on(70.0);
    CHECK(gesture.step(edge + 2.f, false, false).hovered_edge == 1);
    gesture.press(edge + 2.f);
    const LegendBrushStep inside = gesture.move(gesture.axis().x_on(85.0));
    CHECK(inside.dragged_edge == 1);
    check_range(gesture.filter(), 0.f, 85.f);
    const LegendBrushStep end = gesture.move(400.f);
    CHECK(end.range_set);
    CHECK_FALSE(gesture.filter().has_value());
}

TEST_CASE("A double click on an edge of a range of one bar keeps the range and opens the exact range", "[LegendAxis]")
{
    Gesture gesture;
    gesture.click(gesture.axis().x_on(25.0));
    check_range(gesture.filter(), 20.f, 30.f);

    const float edge = gesture.axis().x_on(20.0);
    gesture.click(edge);
    gesture.press(edge, true);
    gesture.release(edge);
    check_range(gesture.filter(), 20.f, 30.f);
    check_range(gesture.popover(), 20.f, 30.f);
}
