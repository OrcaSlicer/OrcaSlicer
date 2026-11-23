#include "MovePanelLayout.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace Slic3r {
namespace GUI {

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
