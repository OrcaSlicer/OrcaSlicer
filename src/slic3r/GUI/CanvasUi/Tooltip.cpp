#include "Tooltip.hpp"

#include <algorithm>
#include <string>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include "Context.hpp"
#include "FontMeasure.hpp"
#include "ImGuiBridge.hpp"
#include "Measure.hpp"
#include "Metrics.hpp"
#include "Theme.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

static void show_tooltip(const std::string& text, const std::string& key)
{
    const Metrics& m = Context::current()->metrics();
    const Theme&   t = Context::current()->theme();
    // The toolkit draws the tooltip's surface itself, with its own corners.
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_Text, im(t.tooltip_foreground));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(m.tooltip_padding_x, m.tooltip_padding_y));
    ImGui::BeginTooltip();
    ImGui::SetWindowFontScale(m.tooltip_text / ImGui::GetFontSize());
    fill_window(u32(t.tooltip), m.tooltip_radius);
    // ImGui hides a window in the frame it appears, so the frame that shows it has to be asked for.
    if (ImGui::IsWindowAppearing())
        Context::current()->request_frame();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + m.tooltip_max_width - 2.f * m.tooltip_padding_x);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    if (!key.empty()) {
        ImGui::SameLine(0.f, m.tooltip_key_gap);
        ImGui::TextColored(im(t.tooltip_muted_foreground), "%s", key.c_str());
    }
    // ImGui's other tooltips open in the same window and keep its scale.
    ImGui::SetWindowFontScale(1.f);
    ImGui::EndTooltip();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

void item_tooltip(const std::string& text, const std::string& key)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        return;
    const ImGuiID item = ImGui::GetCurrentWindowRead()->DC.LastItemId;
    IM_ASSERT(item != 0);
    if (item != 0 && Context::current()->tooltip_due(item))
        show_tooltip(text, key);
}

void tooltip_for(const char* id, const std::string& text, const std::string& key)
{
    if (Context::current()->tooltip_due(ImGui::GetID(id)))
        show_tooltip(text, key);
}

Size tooltip_size(const Metrics& metrics, const TextMeasure& measure, const std::string& text)
{
    const float scale = metrics.tooltip_text / measure.line_height();
    return {std::min(scale * measure.width(text), metrics.tooltip_max_width - 2.f * metrics.tooltip_padding_x) + 2.f * metrics.tooltip_padding_x, metrics.tooltip_text + 2.f * metrics.tooltip_padding_y};
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
