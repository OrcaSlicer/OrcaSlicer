#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/PreviewOverlayLayout.hpp"

#include <cstddef>
#include <vector>

using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

namespace {

PreviewOverlayMetrics metrics(float width, float pill_height = 20.f)
{
    PreviewOverlayMetrics m;
    m.canvas_width      = width;
    m.canvas_height     = 1000.f;
    m.scale             = 1.f;
    m.corner_right      = 100.f;
    m.pill_height       = pill_height;
    m.pill_width        = 150.f;
    return m;
}

MovePanelWidths panel_widths(float available)
{
    MovePanelWidths w;
    w.available      = available;
    w.fixed          = 300.f;
    w.time           = 80.f;
    w.position       = 200.f;
    w.spacing        = 10.f;
    w.group_spacing  = 10.f;
    w.profile_button = 100.f;
    w.profile_chart  = 250.f;
    w.more_button    = 60.f;
    w.groups         = {150.f, 150.f, 150.f, 150.f};
    return w;
}

} // namespace

TEST_CASE("The layer rail band hugs the right edge of the canvas", "[PreviewOverlayLayout]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(metrics(1400.f));

    CHECK_THAT(l.right_band.right, WithinAbs(1400.f, 0.01f));
    CHECK_THAT(l.right_band.width(), WithinAbs(l.band, 0.01f));
    CHECK_THAT(l.right_band.height(), WithinAbs(1000.f, 0.01f));
}

TEST_CASE("The layer rail handles stay clear of the corner squares", "[PreviewOverlayLayout]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(metrics(1400.f));

    CHECK_THAT(l.top_corner.height(), WithinAbs(l.band, 0.01f));
    CHECK_THAT(l.bottom_corner.bottom, WithinAbs(1000.f, 0.01f));
    CHECK(l.side_rail_top - l.handle_radius > l.top_corner.bottom);
    CHECK(l.side_rail_bottom + l.handle_radius < l.bottom_corner.top);
}

TEST_CASE("The band grows to fit a pill taller than the default band", "[PreviewOverlayLayout]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(metrics(1400.f, 31.2f));

    CHECK_THAT(l.band, WithinAbs(32.f, 0.01f));
    CHECK_THAT(l.top_pill.height(), WithinAbs(l.band, 0.01f));
    CHECK_THAT(l.bottom_pill.height(), WithinAbs(l.band, 0.01f));
}

TEST_CASE("The bottom band ends at the bottom pill on a normal canvas", "[PreviewOverlayLayout]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(metrics(1400.f));

    CHECK_THAT(l.bottom_band.left, WithinAbs(0.f, 0.01f));
    CHECK_THAT(l.bottom_band.right, WithinAbs(l.bottom_pill.left, 0.01f));
    CHECK_THAT(l.bottom_pill.right, WithinAbs(l.right_band.left, 0.01f));
}

TEST_CASE("The bottom band is capped and centered on a wide canvas", "[PreviewOverlayLayout]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(metrics(3000.f));

    CHECK_THAT(l.bottom_band.width(), WithinAbs(1600.f, 0.01f));
    CHECK_THAT(l.bottom_band.left + l.bottom_band.right, WithinAbs(l.bottom_pill.left, 1.f));
    CHECK_THAT(l.band_center, WithinAbs(0.5f * l.bottom_pill.left, 0.01f));
}

TEST_CASE("The bottom-left corner sits on top of the bottom band", "[PreviewOverlayLayout]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(metrics(1400.f));

    CHECK_THAT(l.corner_offset(1000.f), WithinAbs(l.band, 0.01f));
    CHECK_THAT(l.band_left, WithinAbs(100.f + l.gap, 0.01f));
    CHECK_THAT(l.band_right, WithinAbs(l.bottom_pill.left - l.gap, 0.01f));
}

TEST_CASE("The move panel stays between the corner and the bottom pill", "[PreviewOverlayLayout]")
{
    const PreviewOverlayLayout l = compute_preview_overlay_layout(metrics(900.f));
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

TEST_CASE("Marks closer than the minimum distance chain into one group", "[PreviewOverlayLayout][MarkGroups]")
{
    const std::vector<TickGroup> groups = group_ticks_by_position({100.f, 105.f, 110.f, 200.f}, 6.f);

    REQUIRE(groups.size() == 2);
    CHECK(groups[0].first == 0);
    CHECK(groups[0].count == 3);
    CHECK(groups[1].first == 3);
    CHECK(groups[1].count == 1);
    CHECK_THAT(groups[1].pos, WithinAbs(200.f, 0.01f));
}

TEST_CASE("Marks far enough apart stay separate", "[PreviewOverlayLayout][MarkGroups]")
{
    const std::vector<TickGroup> groups = group_ticks_by_position({100.f, 120.f, 140.f}, 6.f);

    CHECK(groups.size() == 3);
    CHECK(group_ticks_by_position({}, 6.f).empty());
}

TEST_CASE("The move panel shows everything when it fits", "[PreviewOverlayLayout][MovePanelFit]")
{
    const MovePanelVisibility v = fit_move_panel(panel_widths(900.f));

    CHECK(v.show_profile);
    CHECK(v.show_time);
    CHECK(v.show_position);
    CHECK(v.visible_groups == 4);
}

TEST_CASE("Groups that fit exactly are all shown", "[PreviewOverlayLayout][MovePanelFit]")
{
    MovePanelWidths w;
    w.fixed         = 100.f;
    w.spacing       = 10.f;
    w.group_spacing = 30.f;
    w.more_button   = 60.f;
    w.groups        = {100.f, 100.f};
    w.available     = 230.f; // 100 + 30 + 100

    CHECK(fit_move_panel(w).visible_groups == 2);
}

TEST_CASE("The move panel drops the speed profile first", "[PreviewOverlayLayout][MovePanelFit]")
{
    const MovePanelVisibility v = fit_move_panel(panel_widths(800.f));

    CHECK_FALSE(v.show_profile);
    CHECK(v.show_time);
    CHECK(v.show_position);
    CHECK(v.visible_groups == 4);
}

TEST_CASE("The move row drops the time before the position", "[PreviewOverlayLayout][MovePanelFit]")
{
    const MovePanelVisibility v = fit_move_panel(panel_widths(590.f));

    CHECK_FALSE(v.show_time);
    CHECK(v.show_position);
}

TEST_CASE("Groups that do not fit go behind the more button", "[PreviewOverlayLayout][MovePanelFit]")
{
    const MovePanelVisibility v = fit_move_panel(panel_widths(500.f));

    CHECK(v.visible_groups == 2);
    CHECK_FALSE(v.show_position);
    CHECK_FALSE(v.show_time);
}

TEST_CASE("The speed profile key points bracket the plateau", "[PreviewOverlayLayout][SpeedProfile]")
{
    const SpeedKeyPoints p = speed_profile_key_points({0.f, 50.f, 100.f, 100.f, 100.f, 40.f, 0.f});

    CHECK(p.first == 0);
    CHECK(p.plateau_begin == 2);
    CHECK(p.plateau_end == 4);
    CHECK(p.last == 6);
}

TEST_CASE("A profile without a plateau peaks at a single point", "[PreviewOverlayLayout][SpeedProfile]")
{
    const SpeedKeyPoints p = speed_profile_key_points({10.f, 60.f, 20.f});

    CHECK(p.plateau_begin == 1);
    CHECK(p.plateau_end == 1);
    CHECK(p.last == 2);
}
