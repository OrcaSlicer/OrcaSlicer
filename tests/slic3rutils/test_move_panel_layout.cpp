#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/MovePanelLayout.hpp"

#include <vector>

using namespace Slic3r::GUI;

namespace {

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

TEST_CASE("The move panel shows everything when it fits", "[MovePanelLayout][MovePanelFit]")
{
    const MovePanelVisibility v = fit_move_panel(panel_widths(900.f));

    CHECK(v.show_profile);
    CHECK(v.show_time);
    CHECK(v.show_position);
    CHECK(v.visible_groups == 4);
}

TEST_CASE("Groups that fit exactly are all shown", "[MovePanelLayout][MovePanelFit]")
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

TEST_CASE("The move panel drops the speed profile first", "[MovePanelLayout][MovePanelFit]")
{
    const MovePanelVisibility v = fit_move_panel(panel_widths(800.f));

    CHECK_FALSE(v.show_profile);
    CHECK(v.show_time);
    CHECK(v.show_position);
    CHECK(v.visible_groups == 4);
}

TEST_CASE("The move row drops the time before the position", "[MovePanelLayout][MovePanelFit]")
{
    const MovePanelVisibility v = fit_move_panel(panel_widths(590.f));

    CHECK_FALSE(v.show_time);
    CHECK(v.show_position);
}

TEST_CASE("Groups that do not fit go behind the more button", "[MovePanelLayout][MovePanelFit]")
{
    const MovePanelVisibility v = fit_move_panel(panel_widths(500.f));

    CHECK(v.visible_groups == 2);
    CHECK_FALSE(v.show_position);
    CHECK_FALSE(v.show_time);
}

TEST_CASE("The speed profile key points bracket the plateau", "[MovePanelLayout][SpeedProfile]")
{
    const SpeedKeyPoints p = speed_profile_key_points({0.f, 50.f, 100.f, 100.f, 100.f, 40.f, 0.f});

    CHECK(p.first == 0);
    CHECK(p.plateau_begin == 2);
    CHECK(p.plateau_end == 4);
    CHECK(p.last == 6);
}

TEST_CASE("A profile without a plateau peaks at a single point", "[MovePanelLayout][SpeedProfile]")
{
    const SpeedKeyPoints p = speed_profile_key_points({10.f, 60.f, 20.f});

    CHECK(p.plateau_begin == 1);
    CHECK(p.plateau_end == 1);
    CHECK(p.last == 2);
}
