#include "ToggleGroup.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <imgui/imgui.h>

#include "Availability.hpp"
#include "ComponentParts.hpp"
#include "Context.hpp"
#include "Corners.hpp"
#include "FontMeasure.hpp"
#include "ImGuiBridge.hpp"
#include "Measure.hpp"
#include "Metrics.hpp"
#include "Theme.hpp"
#include "Types.hpp"
#include "Tooltip.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

using namespace detail;

bool toggle_group(const char* id, int& selected, const std::vector<std::string>& choices, const ToggleGroupOptions& options)
{
    const Availability& availability = options.availability;
    const Metrics&      m            = metrics();
    const Theme&        t            = theme();
    const ChoiceMeasure measure;
    const float         gap          = m.toggle_gap;
    const float         natural      = toggle_group_width(m, measure, choices);
    const float         width        = options.width > 0.f ? options.width : natural;
    const bool          inline_row   = natural <= width;
    const float         segment_h    = m.control_height - 2.f * gap;
    const ImVec2        origin       = ImGui::GetCursorScreenPos();
    const ImVec2        frame(width, toggle_group_height(m, measure, choices, width));
    const float         radius       = m.toggle_radius;
    ImDrawList&         list         = *ImGui::GetWindowDrawList();
    fill_rounded(list, origin, ImVec2(origin.x + frame.x, origin.y + frame.y), u32(t.secondary), radius);
    stroke_rounded(list, origin, ImVec2(origin.x + frame.x, origin.y + frame.y), u32(t.input), radius, m.outline);

    const float stretch = inline_row ? (width - natural) / float(std::max<size_t>(1, choices.size())) : 0.f;
    bool        changed = false;
    float       x       = origin.x + gap;
    ImGui::PushID(id);
    for (size_t i = 0; i < choices.size(); ++i) {
        const std::string_view text = visible_label(choices[i]);
        const ImVec2      min = inline_row ? ImVec2(x, origin.y + gap) : ImVec2(origin.x + gap, origin.y + gap + float(i) * (segment_h + gap));
        const ImVec2      size = inline_row ? ImVec2(toggle_width(m, measure, text) + stretch, segment_h) : ImVec2(width - 2.f * gap, segment_h);
        const ImVec2      max(min.x + size.x, min.y + size.y);
        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(int(i));
        const Hit h = hit("##segment", size, availability);
        ImGui::PopID();
        const bool on = int(i) == selected;
        if (on) {
            fill_rounded(list, ImVec2(min.x, min.y + m.toggle_pressed_drop), ImVec2(max.x, max.y + m.toggle_pressed_drop), u32(t.toggle_pressed_shadow), nested_radius(radius, gap));
            fill_rounded(list, min, max, u32(t.toggle_pressed), nested_radius(radius, gap));
            stroke_rounded(list, min, max, u32(t.toggle_pressed_border), nested_radius(radius, gap), m.outline);
        } else if (h.hovered)
            fill_rounded(list, min, max, u32(t.accent), nested_radius(radius, gap));
        choice_label(min, max, text, on, u32(!availability.enabled ? t.disabled_foreground : on ? t.foreground : t.inactive_foreground));
        if (h.clicked && !on) {
            selected = int(i);
            changed  = true;
        }
        x += size.x + gap;
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(frame);
    if (!availability.enabled && !availability.reason.empty() && ImGui::IsWindowHovered() &&
        ImGui::IsMouseHoveringRect(origin, ImVec2(origin.x + frame.x, origin.y + frame.y)))
        tooltip_for(id, availability.reason);
    return changed;
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
