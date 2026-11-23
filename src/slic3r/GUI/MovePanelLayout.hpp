#pragma once

#include <cstddef>
#include <vector>

namespace Slic3r {
namespace GUI {

struct MovePanelWidths
{
    float available{0.f};
    float fixed{0.f};
    float time{0.f};
    float position{0.f};
    float profile_button{0.f};
    float profile_chart{0.f};
    float more_button{0.f};
    // Between the items of the header row.
    float spacing{0.f};
    // Between the property groups, the chart and the more button.
    float group_spacing{0.f};
    std::vector<float> groups;
};

struct MovePanelVisibility
{
    bool   show_profile{false};
    bool   show_time{false};
    bool   show_position{false};
    size_t visible_groups{0};
};

MovePanelVisibility fit_move_panel(const MovePanelWidths &widths);

struct SpeedKeyPoints
{
    size_t first{0};
    size_t plateau_begin{0};
    size_t plateau_end{0};
    size_t last{0};
};

SpeedKeyPoints speed_profile_key_points(const std::vector<float> &speeds);

} // namespace GUI
} // namespace Slic3r
