#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/CanvasUi/Types.hpp"
#include "slic3r/GUI/PreviewOverlayLayout.hpp"
#include "slic3r/GUI/PreviewPanelSlots.hpp"

#include <array>
#include <cstddef>

using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

namespace {

PreviewOverlayInput overlay_input(float width, float pill_height = 20.f)
{
    PreviewOverlayInput input;
    input.canvas_width      = width;
    input.canvas_height     = 1000.f;
    input.scale             = 1.f;
    input.corner_right      = 100.f;
    input.pill_height       = pill_height;
    input.pill_width        = 150.f;
    input.plates_right      = 60.f;
    input.corner_height     = 128.f;
    return input;
}

constexpr size_t LEGEND  = size_t(PreviewPanel::Legend);
constexpr size_t GCODE   = size_t(PreviewPanel::GCode);
constexpr size_t SUMMARY = size_t(PreviewPanel::Summary);

constexpr PreviewPanelShown ALL_SHOWN{true, true, true};

constexpr PreviewPanelSlot LEFT_FIRST{PreviewColumn::Left, 0};
constexpr PreviewPanelSlot LEFT_SECOND{PreviewColumn::Left, 1};
constexpr PreviewPanelSlot LEFT_THIRD{PreviewColumn::Left, 2};
constexpr PreviewPanelSlot RIGHT_FIRST{PreviewColumn::Right, 0};
constexpr PreviewPanelSlot RIGHT_SECOND{PreviewColumn::Right, 1};
constexpr PreviewPanelSlot RIGHT_THIRD{PreviewColumn::Right, 2};

PreviewPanelSlots slots(PreviewPanelSlot legend, PreviewPanelSlot gcode, PreviewPanelSlot summary)
{
    PreviewPanelSlots s;
    s[LEGEND]  = legend;
    s[GCODE]   = gcode;
    s[SUMMARY] = summary;
    return s;
}

PreviewPanelHeights heights(float legend, float gcode, float summary)
{
    PreviewPanelHeights h;
    h[LEGEND]  = legend;
    h[GCODE]   = gcode;
    h[SUMMARY] = summary;
    return h;
}

PreviewOverlayInput varied_input(float width, float pill_height, float scale, float height)
{
    PreviewOverlayInput input = overlay_input(width, pill_height);
    input.scale               = scale;
    input.canvas_height       = height;
    return input;
}

} // namespace

TEST_CASE("The layer rail band runs down the right edge of the canvas, and nothing else reaches into it", "[PreviewOverlayLayout]")
{
    const float                width       = GENERATE(900.f, 1400.f, 3000.f);
    const float                pill_height = GENERATE(20.f, 31.2f);
    const float                scale       = GENERATE(1.f, 1.5f);
    const float                height      = GENERATE(600.f, 1000.f);
    const PreviewOverlayInput  input       = varied_input(width, pill_height, scale, height);
    const PreviewOverlayLayout l           = compute_preview_overlay_layout(input);

    CHECK_THAT(l.right_band.right, WithinAbs(input.canvas_width, 0.01f));
    CHECK_THAT(l.right_band.top, WithinAbs(0.f, 0.01f));
    CHECK_THAT(l.right_band.bottom, WithinAbs(input.canvas_height, 0.01f));
    CHECK(l.side_rail_x > l.right_band.left);
    CHECK(l.side_rail_x < l.right_band.right);
    CHECK(l.top_pill.right <= l.right_band.left + 0.01f);
    CHECK(l.bottom_pill.right <= l.right_band.left + 0.01f);
    CHECK(l.bottom_band.right <= l.right_band.left + 0.01f);
    CHECK(l.overlay_right < l.right_band.left);
    const auto p = l.place_panels(DEFAULT_PREVIEW_PANEL_SLOTS, heights(300.f, 200.f, 120.f), ALL_SHOWN);
    for (const PreviewPanelPlacement& placement : p)
        CHECK(placement.left(300.f) + 300.f < l.right_band.left);
}

TEST_CASE("The layer rail handles stay clear of the toggle squares", "[PreviewOverlayLayout]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(overlay_input(1400.f));

    CHECK_THAT(l.top_toggle_square.height(), WithinAbs(l.band, 0.01f));
    CHECK_THAT(l.bottom_toggle_square.bottom, WithinAbs(1000.f, 0.01f));
    CHECK(l.side_rail_top - l.handle_radius > l.top_toggle_square.bottom);
    CHECK(l.side_rail_bottom + l.handle_radius < l.bottom_toggle_square.top);
}

TEST_CASE("The band grows to fit a pill taller than the default band", "[PreviewOverlayLayout]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(overlay_input(1400.f, 31.2f));

    CHECK_THAT(l.band, WithinAbs(32.f, 0.01f));
    CHECK_THAT(l.top_pill.height(), WithinAbs(l.band, 0.01f));
    CHECK_THAT(l.bottom_pill.height(), WithinAbs(l.band, 0.01f));
}

TEST_CASE("The bottom band ends at the bottom pill on a normal canvas", "[PreviewOverlayLayout]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(overlay_input(1400.f));

    CHECK_THAT(l.bottom_band.left, WithinAbs(0.f, 0.01f));
    CHECK_THAT(l.bottom_band.right, WithinAbs(l.bottom_pill.left, 0.01f));
    CHECK_THAT(l.bottom_pill.right, WithinAbs(l.right_band.left, 0.01f));
}

TEST_CASE("The bottom band is capped and centered on a wide canvas", "[PreviewOverlayLayout]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(overlay_input(3000.f));

    CHECK_THAT(l.bottom_band.width(), WithinAbs(1600.f, 0.01f));
    CHECK_THAT(l.bottom_band.left + l.bottom_band.right, WithinAbs(l.bottom_pill.left, 1.f));
}

TEST_CASE("The bottom-left corner sits on the bottom band with no gap, and the move panel's room is clear of both ends", "[PreviewOverlayLayout]")
{
    const float                width       = GENERATE(900.f, 1400.f, 3000.f);
    const float                pill_height = GENERATE(20.f, 31.2f);
    const float                scale       = GENERATE(1.f, 1.5f);
    const float                height      = GENERATE(600.f, 1000.f);
    const PreviewOverlayInput  input       = varied_input(width, pill_height, scale, height);
    const PreviewOverlayLayout l           = compute_preview_overlay_layout(input);
    const float                corner_bottom = input.canvas_height - l.corner_offset(input.canvas_height);

    CHECK_THAT(corner_bottom, WithinAbs(l.bottom_band.top, 0.01f));
    CHECK_THAT(l.bottom_pill.top, WithinAbs(l.bottom_band.top, 0.01f));
    CHECK(l.band_left > input.corner_right);
    CHECK(l.band_right <= l.bottom_pill.left);
}

TEST_CASE("The move panel stays between the corner and the bottom pill", "[PreviewOverlayLayout]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(overlay_input(900.f));
    const float                gap_width = l.band_right - l.band_left;

    SECTION("a narrow panel is centered on the rail")
    {
        const float w = l.panel_width(200.f);
        CHECK_THAT(w, WithinAbs(200.f, 0.01f));
        CHECK_THAT(l.panel_left(w) + 0.5f * w, WithinAbs(l.band_center, 1.f));
    }
    SECTION("a panel wider than the gap is shrunk to it")
    {
        const float w = l.panel_width(gap_width + 300.f);
        CHECK_THAT(w, WithinAbs(gap_width, 0.01f));
        CHECK_THAT(l.panel_left(w), WithinAbs(l.band_left, 1.f));
    }
    SECTION("a panel that would reach the corner is pushed right")
    {
        const float w = l.panel_width(gap_width - 10.f);
        CHECK(l.panel_left(w) >= l.band_left);
        CHECK(l.panel_left(w) + w <= l.band_right + 1.f);
    }
}

TEST_CASE("The order inside a column decides which panel comes first", "[PreviewOverlayLayout][PreviewPanelSlots]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(overlay_input(1400.f));

    SECTION("right column, G-code first")
    {
        const auto p = l.place_panels(slots(RIGHT_SECOND, RIGHT_FIRST, LEFT_FIRST), heights(300.f, 200.f, 120.f), ALL_SHOWN);
        CHECK_THAT(p[GCODE].top, WithinAbs(l.column_top, 0.01f));
        CHECK_THAT(p[LEGEND].top, WithinAbs(l.column_top + 200.f + l.gap, 0.01f));
    }
    SECTION("left column, legend first")
    {
        const auto p = l.place_panels(slots(LEFT_FIRST, LEFT_SECOND, RIGHT_FIRST), heights(300.f, 200.f, 120.f), ALL_SHOWN);
        CHECK_THAT(p[LEGEND].top, WithinAbs(l.column_top, 0.01f));
        CHECK_THAT(p[GCODE].top, WithinAbs(l.column_top + 300.f + l.gap, 0.01f));
    }
    SECTION("left column, G-code first")
    {
        const auto p = l.place_panels(slots(LEFT_SECOND, LEFT_FIRST, RIGHT_FIRST), heights(300.f, 200.f, 120.f), ALL_SHOWN);
        CHECK_THAT(p[GCODE].top, WithinAbs(l.column_top, 0.01f));
        CHECK_THAT(p[LEGEND].top, WithinAbs(l.column_top + 200.f + l.gap, 0.01f));
    }
}

TEST_CASE("Panels in different columns both start at the top", "[PreviewOverlayLayout][PreviewPanelSlots]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(overlay_input(1400.f));

    SECTION("legend left, G-code right")
    {
        const auto p = l.place_panels(slots(LEFT_FIRST, RIGHT_FIRST, RIGHT_SECOND), heights(300.f, 200.f, 120.f), ALL_SHOWN);
        CHECK_FALSE(p[LEGEND].is_right());
        CHECK(p[GCODE].is_right());
        CHECK_THAT(p[LEGEND].top, WithinAbs(l.column_top, 0.01f));
        CHECK_THAT(p[GCODE].top, WithinAbs(l.column_top, 0.01f));
    }
    SECTION("legend right, G-code left")
    {
        const auto p = l.place_panels(slots(RIGHT_FIRST, LEFT_FIRST, RIGHT_SECOND), heights(300.f, 200.f, 120.f), ALL_SHOWN);
        CHECK(p[LEGEND].is_right());
        CHECK_FALSE(p[GCODE].is_right());
        CHECK_THAT(p[LEGEND].top, WithinAbs(l.column_top, 0.01f));
        CHECK_THAT(p[GCODE].top, WithinAbs(l.column_top, 0.01f));
    }
}

TEST_CASE("The left column stays clear of the plate list and the corner", "[PreviewOverlayLayout][PreviewPanelSlots]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(overlay_input(1400.f));
    const auto p = l.place_panels(slots(LEFT_FIRST, LEFT_SECOND, RIGHT_FIRST), heights(300.f, 200.f, 120.f), ALL_SHOWN);

    CHECK(p[LEGEND].x > 60.f);
    CHECK_THAT(p[LEGEND].left(250.f), WithinAbs(p[LEGEND].x, 0.01f));
    CHECK(p[GCODE].bottom < l.band_bottom - 128.f);
    CHECK_THAT(p[GCODE].bottom, WithinAbs(l.left_column_bottom, 0.01f));
}

TEST_CASE("A hidden panel leaves no gap in its column", "[PreviewOverlayLayout][PreviewPanelSlots]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(overlay_input(1400.f));
    const auto p = l.place_panels(slots(RIGHT_FIRST, RIGHT_SECOND, LEFT_FIRST), heights(0.f, 200.f, 120.f), ALL_SHOWN);

    CHECK_THAT(p[GCODE].top, WithinAbs(l.column_top, 0.01f));
}

TEST_CASE("A panel pushed past the column bottom keeps its header in the column", "[PreviewOverlayLayout][PreviewPanelSlots]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(overlay_input(1400.f));
    const auto p = l.place_panels(slots(RIGHT_FIRST, RIGHT_SECOND, LEFT_FIRST), heights(5000.f, 200.f, 120.f), ALL_SHOWN);

    CHECK_THAT(p[GCODE].top, WithinAbs(l.overlays_bottom - l.panel_header, 0.01f));
    CHECK_THAT(p[GCODE].bottom, WithinAbs(l.overlays_bottom, 0.01f));
}

TEST_CASE("A panel leaves room for the headers of the panels under it", "[PreviewOverlayLayout][PreviewPanelSlots]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(overlay_input(1400.f));
    const auto p = l.place_panels(DEFAULT_PREVIEW_PANEL_SLOTS, heights(900.f, 900.f, 200.f), ALL_SHOWN);

    CHECK_THAT(p[LEGEND].bottom, WithinAbs(l.overlays_bottom - 2.f * (l.panel_header + l.gap), 0.01f));
    CHECK_THAT(p[SUMMARY].bottom, WithinAbs(l.overlays_bottom - (l.panel_header + l.gap), 0.01f));
    CHECK(p[GCODE].top <= l.overlays_bottom - l.panel_header);
}

TEST_CASE("The left column starts under the sidebar collapse button", "[PreviewOverlayLayout][PreviewPanelSlots]")
{
    PreviewOverlayInput input = overlay_input(1400.f);
    input.top_left_reserved = 120.f;
    const PreviewOverlayLayout l = compute_preview_overlay_layout(input);
    const auto p = l.place_panels(slots(LEFT_FIRST, RIGHT_FIRST, RIGHT_SECOND), heights(300.f, 200.f, 120.f), ALL_SHOWN);

    CHECK(p[LEGEND].top > 120.f);
    CHECK_THAT(p[GCODE].top, WithinAbs(l.column_top, 0.01f));
}

TEST_CASE("By default the legend, the print summary and the G-code panel stand in this order on the right", "[PreviewOverlayLayout][PreviewPanelSlots]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(overlay_input(1400.f));
    const auto p = l.place_panels(DEFAULT_PREVIEW_PANEL_SLOTS, heights(300.f, 200.f, 120.f), ALL_SHOWN);

    for (const PreviewPanelPlacement& placement : p) {
        CHECK(placement.is_right());
        CHECK_THAT(placement.x, WithinAbs(l.overlay_right, 0.01f));
    }
    CHECK_THAT(p[LEGEND].top, WithinAbs(l.column_top, 0.01f));
    CHECK_THAT(p[SUMMARY].top, WithinAbs(l.column_top + 300.f + l.gap, 0.01f));
    CHECK_THAT(p[GCODE].top, WithinAbs(l.column_top + 300.f + l.gap + 120.f + l.gap, 0.01f));
    CHECK_THAT(p[GCODE].bottom, WithinAbs(l.overlays_bottom, 0.01f));
}

TEST_CASE("The G-code panel leaves room for a panel under it", "[PreviewOverlayLayout][PreviewPanelSlots]")
{
    const PreviewOverlayLayout l    = compute_preview_overlay_layout(overlay_input(1400.f));
    const float                half = l.column_top + 0.5f * (l.overlays_bottom - l.column_top);

    SECTION("G-code first takes half of its column")
    {
        const auto p = l.place_panels(slots(RIGHT_SECOND, RIGHT_FIRST, LEFT_FIRST), heights(300.f, 0.f, 120.f), ALL_SHOWN);
        CHECK_THAT(p[GCODE].bottom, WithinAbs(half, 0.01f));
        CHECK_THAT(p[LEGEND].bottom, WithinAbs(l.overlays_bottom, 0.01f));
    }
    SECTION("G-code first over a hidden legend reaches the bottom of its column")
    {
        const auto p = l.place_panels(slots(RIGHT_SECOND, RIGHT_FIRST, LEFT_FIRST), heights(0.f, 0.f, 120.f), {false, true, true});
        CHECK_THAT(p[GCODE].bottom, WithinAbs(l.overlays_bottom, 0.01f));
    }
    SECTION("G-code first gives way to a legend just shown, which has no height yet")
    {
        const auto p = l.place_panels(slots(RIGHT_SECOND, RIGHT_FIRST, LEFT_FIRST), heights(0.f, 0.f, 120.f), ALL_SHOWN);
        CHECK_THAT(p[GCODE].bottom, WithinAbs(half, 0.01f));
    }
    SECTION("G-code first gives way to the print summary under it when the legend is hidden")
    {
        const auto p = l.place_panels(put_gcode_panel_first(DEFAULT_PREVIEW_PANEL_SLOTS, true), heights(0.f, 0.f, 0.f), {false, true, true});
        CHECK_THAT(p[GCODE].top, WithinAbs(l.column_top, 0.01f));
        CHECK_THAT(p[GCODE].bottom, WithinAbs(half, 0.01f));
    }
    SECTION("G-code first over panels in the other column reaches the bottom of its column")
    {
        const auto p = l.place_panels(slots(LEFT_SECOND, RIGHT_FIRST, LEFT_THIRD), heights(300.f, 200.f, 120.f), ALL_SHOWN);
        CHECK_THAT(p[GCODE].bottom, WithinAbs(l.overlays_bottom, 0.01f));
    }
    SECTION("G-code last reaches the bottom of its column")
    {
        const auto p = l.place_panels(slots(RIGHT_FIRST, RIGHT_THIRD, RIGHT_SECOND), heights(300.f, 0.f, 120.f), ALL_SHOWN);
        CHECK_THAT(p[GCODE].bottom, WithinAbs(l.overlays_bottom, 0.01f));
    }
}

TEST_CASE("A panel stops a gap above the move panel once that comes within a gap of it, however far it reaches in", "[PreviewOverlayLayout]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(overlay_input(1400.f));
    const float                left = 900.f, right = 1300.f, bottom = 950.f;
    const auto                 move_panel = [](float panel_right) { return CanvasUi::Rect(300.f, 900.f, panel_right, 940.f); };

    const float clear = 900.f - l.gap;
    CHECK_THAT(l.bottom_clear_of(move_panel(left + 12.f), left, right, bottom), WithinAbs(clear, 0.01f));
    CHECK_THAT(l.bottom_clear_of(move_panel(left - 1.f), left, right, bottom), WithinAbs(clear, 0.01f));
    CHECK_THAT(l.bottom_clear_of(move_panel(left - 0.5f * l.gap), left, right, bottom), WithinAbs(clear, 0.01f));
    CHECK_THAT(l.bottom_clear_of(move_panel(left - 2.f * l.gap), left, right, bottom), WithinAbs(bottom, 0.01f));
    CHECK_THAT(l.bottom_clear_of(CanvasUi::Rect(), left, right, bottom), WithinAbs(bottom, 0.01f));

    const auto right_of = [](float panel_left) { return CanvasUi::Rect(panel_left, 900.f, panel_left + 300.f, 940.f); };
    CHECK_THAT(l.bottom_clear_of(right_of(right + 0.5f * l.gap), left, right, bottom), WithinAbs(clear, 0.01f));
    CHECK_THAT(l.bottom_clear_of(right_of(right + 2.f * l.gap), left, right, bottom), WithinAbs(bottom, 0.01f));
}
