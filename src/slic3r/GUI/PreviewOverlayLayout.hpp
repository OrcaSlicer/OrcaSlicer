#pragma once

#include <cstddef>
#include <vector>

namespace Slic3r {
namespace GUI {

struct PreviewRect
{
    float left{0.f};
    float top{0.f};
    float right{0.f};
    float bottom{0.f};

    float width() const { return right - left; }
    float height() const { return bottom - top; }
    bool  contains(float x, float y) const { return x >= left && x <= right && y >= top && y <= bottom; }
};

struct PreviewOverlayMetrics
{
    float canvas_width{0.f};
    float canvas_height{0.f};
    float scale{1.f};
    float corner_right{0.f};
    float pill_height{0.f};
    float pill_width{0.f};
};

struct PreviewOverlayLayout
{
    float scale{1.f};
    float band{0.f};
    float handle_radius{0.f};
    float track_thickness{0.f};
    float gap{0.f};

    PreviewRect right_band;
    PreviewRect bottom_band;

    float       side_rail_x{0.f};
    float       side_rail_top{0.f};
    float       side_rail_bottom{0.f};
    PreviewRect top_pill;
    PreviewRect bottom_pill;
    // The squares at both ends of the right band, above and below the layer rail.
    PreviewRect top_corner;
    PreviewRect bottom_corner;

    float bottom_rail_y{0.f};
    float bottom_rail_left{0.f};
    float bottom_rail_right{0.f};

    float band_left{0.f};
    float band_right{0.f};
    float band_bottom{0.f};
    float band_center{0.f};

    float overlay_right{0.f};
    float legend_top{0.f};
    float overlays_bottom{0.f};

    float corner_offset(float canvas_height) const { return canvas_height - band_bottom; }
    float panel_width(float wanted) const;
    float panel_left(float width) const;
};

PreviewOverlayLayout compute_preview_overlay_layout(const PreviewOverlayMetrics &metrics);

struct TickGroup
{
    size_t first{0};
    size_t count{0};
    float  pos{0.f};
};

std::vector<TickGroup> group_ticks_by_position(const std::vector<float> &positions, float min_distance);

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
