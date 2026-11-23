#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/CanvasUi/Types.hpp"
#include "slic3r/GUI/PreviewOverlayLayout.hpp"

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
    return input;
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
