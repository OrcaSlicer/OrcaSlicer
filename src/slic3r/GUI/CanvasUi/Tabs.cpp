#include "Tabs.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <imgui/imgui.h>

#include "Availability.hpp"
#include "Button.hpp"
#include "ComponentParts.hpp"
#include "Icons.hpp"
#include "Context.hpp"
#include "Corners.hpp"
#include "FontMeasure.hpp"
#include "ImGuiBridge.hpp"
#include "Measure.hpp"
#include "Metrics.hpp"
#include "Theme.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

using namespace detail;

bool tabs_list(const char* id, int& selected, const std::vector<std::string>& labels)
{
    const Metrics&      m      = metrics();
    const Theme&        t      = theme();
    const ChoiceMeasure choices;
    const ImVec2        origin = ImGui::GetCursorScreenPos();
    const float         top    = origin.y + m.tabs_list_height - m.tab_height;
    const float         bottom = origin.y + m.tabs_list_height;
    ImDrawList&         list   = *ImGui::GetWindowDrawList();
    bool                changed = false;
    float               x       = origin.x;
    ImGui::PushID(id);
    for (size_t i = 0; i < labels.size(); ++i) {
        const std::string_view text = visible_label(labels[i]);
        const ImVec2           min(x, top);
        const ImVec2           max(x + tab_width(m, choices, text), bottom);
        const bool             on = int(i) == selected;
        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(int(i));
        const Hit h = hit("##tab", ImVec2(max.x - min.x, max.y - min.y), {});
        ImGui::PopID();
        if (on) {
            fill_rounded(list, min, ImVec2(max.x, max.y + m.outline), u32(t.card), m.tab_radius, Corners::Top);
            stroke_rounded(list, min, ImVec2(max.x, max.y + m.outline), u32(t.border), m.tab_radius, m.outline, Corners::Top);
            list.AddRectFilled(ImVec2(min.x + m.outline, max.y - m.outline), ImVec2(max.x - m.outline, max.y + m.outline), u32(t.card));
        } else if (h.hovered)
            fill_rounded(list, min, max, u32(t.tab_hover), m.tab_radius, Corners::Top);
        choice_label(min, max, text, on, u32(on || h.hovered ? t.foreground : t.inactive_foreground));
        if (h.clicked && !on) {
            selected = int(i);
            changed  = true;
        }
        x = max.x + m.tab_gap;
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(tabs_list_width(m, choices, labels), m.tabs_list_height));
    return changed;
}

TabsBarResult tabs_bar(const char* id, int& selected, const std::vector<std::string>& labels, const TabsBarOptions& options)
{
    const Metrics&      m       = metrics();
    const Theme&        t       = theme();
    const ImVec2        origin  = ImGui::GetCursorScreenPos();
    const float         left    = origin.x;
    const float         right   = origin.x + options.width;
    const float         row_end = origin.y + m.tabs_list_height;
    const float         tab_top = row_end - m.tab_height;
    const FontMeasure   measure;
    const ChoiceMeasure choices;
    ImDrawList&         list = *ImGui::GetWindowDrawList();
    list.AddRectFilled(ImVec2(left + m.outline, origin.y), ImVec2(right - m.outline, row_end), u32(t.tabs_list));
    list.AddLine(ImVec2(left, row_end - m.outline), ImVec2(right, row_end - m.outline), u32(t.border), m.outline);

    const std::vector<RowSpan> spans = row_layout(left + m.tabs_bar_left, right - m.tabs_bar_right, m.inline_gap,
                                                  {{tabs_list_width(m, choices, labels)}, {0.f, true}, {measure.width(options.changed_count), false, true},
                                                   {button_width(m, measure, options.reset_label, ButtonSize::Sm, true)}});
    TabsBarResult result;
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(ImVec2(spans[0].left, origin.y));
    result.changed = tabs_list("##tabs", selected, labels);
    if (!options.changed_count.empty()) {
        const RowSpan& count = spans[2];
        list.PushClipRect(ImVec2(count.left, origin.y), ImVec2(count.right(), row_end), true);
        list.AddText(ImVec2(count.left, tab_top + .5f * (m.tab_height - ImGui::GetFontSize())), u32(t.muted_foreground), options.changed_count.c_str());
        list.PopClipRect();
    }
    ButtonOptions reset;
    reset.variant = ButtonVariant::Ghost;
    reset.icon    = Icon::Reset;
    reset.size    = ButtonSize::Sm;
    reset.availability   = Availability{options.can_reset, ""};
    ImGui::SetCursorScreenPos(ImVec2(spans[3].left, tab_top + .5f * (m.tab_height - m.small_button)));
    result.reset = button("##reset", options.reset_label, reset);
    ImGui::PopID();
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(options.width, m.tabs_list_height));
    return result;
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
