#include "Field.hpp"

#include <string>
#include <string_view>
#include <vector>

#include <imgui/imgui.h>

#include "Availability.hpp"
#include "ComponentParts.hpp"
#include "Context.hpp"
#include "FontMeasure.hpp"
#include "ImGuiBridge.hpp"
#include "Measure.hpp"
#include "Metrics.hpp"
#include "Theme.hpp"
#include "Types.hpp"
#include "ToggleGroup.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

using namespace detail;

bool field_toggle_group(const char* id, int& selected, const std::vector<std::string>& options, const FieldOptions& row)
{
    const std::string& label = row.label;
    const float        width = row.width;
    const Availability& availability = row.availability;
    const Metrics&    m       = metrics();
    const FontMeasure   measure;
    const ChoiceMeasure choices;
    const std::string_view text = visible_label(label);
    const float       control = toggle_group_width(m, choices, options);
    const float       height  = field_toggle_group_height(m, measure, choices, label, options, width);
    const ImVec2      origin  = ImGui::GetCursorScreenPos();
    const ImU32       ink     = u32(availability.enabled ? theme().foreground : theme().disabled_foreground);
    ImDrawList&       list    = *ImGui::GetWindowDrawList();
    bool              changed = false;
    if (field_fits_inline(m, measure, label, control, width)) {
        list.AddText(ImVec2(origin.x, origin.y + .5f * (height - measure.line_height())), ink, text.data(), text.data() + text.size());
        ImGui::SetCursorScreenPos(ImVec2(origin.x + width - control, origin.y + .5f * (height - m.control_height)));
        changed = toggle_group(id, selected, options, {control, availability});
    } else {
        list.AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(origin.x, origin.y + m.field_label_top), ink, text.data(), text.data() + text.size(), width);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + measure.height(text, width) + m.field_label_gap));
        changed = toggle_group(id, selected, options, {width, availability});
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
    return changed;
}

bool field_switch(const char* id, bool& value, const FieldOptions& options)
{
    const std::string& label = options.label;
    const float        width = options.width;
    const Availability& availability = options.availability;
    const Metrics&    m      = metrics();
    const Theme&      t      = theme();
    const std::string_view text = visible_label(label);
    const ImVec2      track(m.switch_width, m.switch_height);
    const float       wrap   = switch_label_width(m, width);
    const float       row    = field_switch_height(m, FontMeasure(), label, width);
    const float       text_h = FontMeasure().height(text, wrap);
    const Hit         h      = hit(id, ImVec2(width, row), availability);
    ImDrawList&       list   = *ImGui::GetWindowDrawList();
    list.AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(h.min.x, h.min.y + .5f * (row - text_h)),
                 u32(availability.enabled ? t.foreground : t.disabled_foreground), text.data(), text.data() + text.size(), wrap);
    const ImVec2 min(h.max.x - track.x, h.min.y + .5f * (row - track.y));
    const ImVec2 max(h.max.x, min.y + track.y);
    const float  radius = .5f * track.y;
    fill_rounded(list, min, max, u32(value && availability.enabled ? t.primary : t.input), radius);
    const ImVec2 knob(value ? max.x - radius : min.x + radius, min.y + radius);
    list.AddCircleFilled(ImVec2(knob.x, knob.y + m.switch_thumb_drop), radius - m.switch_thumb_inset, u32(t.switch_thumb_shadow), 16);
    list.AddCircleFilled(knob, radius - m.switch_thumb_inset, u32(availability.enabled ? t.switch_thumb : t.secondary), 16);
    reason_tooltip(availability);
    if (h.clicked)
        value = !value;
    return h.clicked;
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
