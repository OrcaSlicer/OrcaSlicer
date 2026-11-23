#include "ComponentParts.hpp"

#include <algorithm>
#include <cfloat>
#include <string_view>
#include <string>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include "Availability.hpp"
#include "Context.hpp"
#include "Font.hpp"
#include "Metrics.hpp"
#include "Theme.hpp"
#include "ImGuiBridge.hpp"
#include "Tooltip.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {
namespace detail {

const Theme&   theme() { return Context::current()->theme(); }
const Metrics& metrics() { return Context::current()->metrics(); }

void reason_tooltip(const Availability& availability)
{
    if (!availability.enabled && !availability.reason.empty())
        item_tooltip(availability.reason);
}


// A disabled item still hovers, for its reason's tooltip, but never becomes ImGui's active item.
bool invisible_button(const char* id, const ImVec2& size, const Availability& availability)
{
    if (!availability.enabled)
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
    const bool clicked = ImGui::InvisibleButton(id, size) && availability.enabled;
    if (!availability.enabled)
        ImGui::PopItemFlag();
    return clicked;
}

Hit hit(const char* id, const ImVec2& size, const Availability& availability)
{
    Hit h;
    h.min     = ImGui::GetCursorScreenPos();
    h.max     = ImVec2(h.min.x + size.x, h.min.y + size.y);
    h.clicked = invisible_button(id, size, availability);
    h.hovered = availability.enabled && ImGui::IsItemHovered();
    h.held    = availability.enabled && ImGui::IsItemActive();
    if (h.clicked)
        Context::current()->request_frame();
    return h;
}

void choice_label(const ImVec2& min, const ImVec2& max, std::string_view text, bool chosen, ImU32 color)
{
    ImFont* const font  = chosen ? face_of(Context::current()->bold_font()) : ImGui::GetFont();
    const float   size  = ImGui::GetFontSize();
    const float   ink_w = font->CalcTextSizeA(size, FLT_MAX, 0.f, text.data(), text.data() + text.size()).x;
    ImGui::GetWindowDrawList()->AddText(font, size, ImVec2(0.5f * (min.x + max.x - ink_w), 0.5f * (min.y + max.y - size)), color, text.data(),
                                        text.data() + text.size());
}

void push_field_style()
{
    const Metrics& m = metrics();
    const Theme&   t = theme();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, im(t.secondary));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, im(t.secondary_hover));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, im(t.secondary));
    ImGui::PushStyleColor(ImGuiCol_Border, im(t.input));
    ImGui::PushStyleColor(ImGuiCol_Text, im(t.foreground));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, im(t.selection));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, m.outline);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, m.control_radius);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(m.control_padding, std::max(0.f, 0.5f * (m.control_height - ImGui::GetFontSize()))));
}

void pop_field_style()
{
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(6);
}

void push_popup_style()
{
    // The toolkit draws a popup's surface itself, with its own corners.
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_Text, im(theme().foreground));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
}

void pop_popup_style()
{
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
}

void focus_ring()
{
    if (!ImGui::IsItemActive())
        return;
    const Metrics& m = metrics();
    stroke_rounded(*ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), u32(theme().primary), m.control_radius, m.focus_ring);
}

} // namespace detail
} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
