#include "PreviewOverlayLayout.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace Slic3r {
namespace GUI {

static constexpr float RAIL_BAND       = 28.f;
static constexpr float HANDLE_RADIUS   = 9.f;
static constexpr float TRACK_THICKNESS = 5.f;
static constexpr float GAP             = 8.f;
static constexpr float OVERLAY_GAP     = 8.f;
static constexpr float RAIL_MAX_WIDTH  = 1600.f;

float PreviewOverlayLayout::panel_width(float wanted) const
{
    return std::max(0.f, std::min(wanted, band_right - band_left));
}

float PreviewOverlayLayout::panel_left(float width) const
{
    return std::round(std::clamp(band_center - 0.5f * width, band_left, std::max(band_left, band_right - width)));
}

PreviewOverlayLayout compute_preview_overlay_layout(const PreviewOverlayMetrics &m)
{
    const float s = m.scale;

    PreviewOverlayLayout l;
    l.scale           = s;
    l.band            = std::max(std::round(RAIL_BAND * s), std::ceil(m.pill_height));
    l.handle_radius   = HANDLE_RADIUS * s;
    l.track_thickness = TRACK_THICKNESS * s;
    l.gap             = GAP * s;

    const float W = m.canvas_width;
    const float H = m.canvas_height;

    l.right_band  = {W - l.band, 0.f, W, H};
    const float pill_w = std::ceil(m.pill_width);
    l.top_pill         = {W - l.band - pill_w, 0.f, W - l.band, l.band};
    l.bottom_pill      = {W - l.band - pill_w, H - l.band, W - l.band, H};

    const float rail_span = l.bottom_pill.left;
    const float rail_w    = std::min(rail_span, std::round(RAIL_MAX_WIDTH * s));
    l.band_center         = 0.5f * rail_span;
    l.bottom_band         = {std::round(l.band_center - 0.5f * rail_w), H - l.band, 0.f, H};
    l.bottom_band.right   = l.bottom_band.left + rail_w;

    l.side_rail_x       = W - 0.5f * l.band;
    l.bottom_rail_y     = H - 0.5f * l.band;
    l.bottom_rail_left  = l.bottom_band.left + 0.5f * l.band;
    l.bottom_rail_right = l.bottom_band.right - 0.5f * l.band;

    l.top_corner       = {W - l.band, 0.f, W, l.band};
    l.bottom_corner    = {W - l.band, H - l.band, W, H};
    l.side_rail_top    = l.top_corner.bottom + 0.5f * l.band;
    l.side_rail_bottom = l.bottom_corner.top - 0.5f * l.band;

    l.band_bottom     = H - l.band;
    l.band_left       = m.corner_right + l.gap;
    l.band_right      = std::max(l.band_left, l.bottom_pill.left - l.gap);
    l.overlay_right   = l.right_band.left - OVERLAY_GAP * s;
    l.legend_top      = l.top_pill.bottom + OVERLAY_GAP * s;
    l.overlays_bottom = l.bottom_pill.top - OVERLAY_GAP * s;
    return l;
}

std::vector<TickGroup> group_ticks_by_position(const std::vector<float> &positions, float min_distance)
{
    std::vector<TickGroup> groups;
    size_t i = 0;
    while (i < positions.size()) {
        size_t j = i + 1;
        while (j < positions.size() && std::abs(positions[j] - positions[j - 1]) < min_distance)
            ++j;
        groups.push_back({i, j - i, 0.5f * (positions[i] + positions[j - 1])});
        i = j;
    }
    return groups;
}

MovePanelVisibility fit_move_panel(const MovePanelWidths &w)
{
    const float  avail  = w.available;
    const size_t groups = w.groups.size();

    auto row_width = [&w](bool profile, bool time, bool position) {
        return w.fixed + (time ? w.spacing + w.time : 0.f) + (position ? w.spacing + w.position : 0.f) +
               (profile ? w.spacing + w.profile_button : 0.f);
    };
    auto body_width = [&w, groups](size_t shown, bool profile) {
        float width = 0.f;
        for (size_t i = 0; i < shown; ++i)
            width += (i > 0 ? w.group_spacing : 0.f) + w.groups[i];
        if (profile)
            width += (shown > 0 ? w.group_spacing : 0.f) + w.profile_chart;
        if (shown < groups)
            width += (shown > 0 || profile ? w.group_spacing : 0.f) + w.more_button;
        return width;
    };

    MovePanelVisibility v;
    v.show_profile = w.profile_chart > 0.f && row_width(true, true, true) <= avail && body_width(groups, true) <= avail;
    if (v.show_profile) {
        v.show_time      = true;
        v.show_position  = true;
        v.visible_groups = groups;
        return v;
    }

    v.visible_groups = groups;
    while (v.visible_groups > 0 && body_width(v.visible_groups, false) > avail)
        --v.visible_groups;

    v.show_position = row_width(false, false, true) <= avail;
    v.show_time     = v.show_position && row_width(false, true, true) <= avail;
    return v;
}

SpeedKeyPoints speed_profile_key_points(const std::vector<float> &speeds)
{
    SpeedKeyPoints p;
    if (speeds.empty())
        return p;
    p.last                = speeds.size() - 1;
    const float max_speed = *std::max_element(speeds.begin(), speeds.end());
    auto        at_peak   = [max_speed](float speed) { return speed >= max_speed * 0.999f; };
    p.plateau_begin       = size_t(std::find_if(speeds.begin(), speeds.end(), at_peak) - speeds.begin());
    p.plateau_end         = speeds.size() - 1 - size_t(std::find_if(speeds.rbegin(), speeds.rend(), at_peak) - speeds.rbegin());
    return p;
}

} // namespace GUI
} // namespace Slic3r
