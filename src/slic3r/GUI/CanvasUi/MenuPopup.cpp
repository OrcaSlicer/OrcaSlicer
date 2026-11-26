#include "MenuPopup.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <imgui/imgui.h>

#include "Context.hpp"
#include "ImGuiBridge.hpp"
#include "Font.hpp"
#include "FontMeasure.hpp"
#include "Icons.hpp"
#include "Menu.hpp"
#include "Metrics.hpp"
#include "Card.hpp"
#include "ComponentParts.hpp"
#include "ScrollArea.hpp"
#include "Tooltip.hpp"
#include "Theme.hpp"
#include "Tokens.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

using detail::metrics;
using detail::pop_popup_style;
using detail::push_popup_style;
using detail::theme;

namespace {

void draw_mark(ImDrawList& list, Indicator mark, bool on, bool enabled, bool hovered, const ImVec2& center)
{
    const Metrics& m = metrics();
    const Theme&   t = theme();
    // The brand colour loses contrast on the hover surface; the accent text keeps it.
    const Color    accent = hovered ? t.primary_strong : t.primary;
    switch (mark) {
    case Indicator::Radio:
        list.AddCircle(center, m.radio_outer, u32(!enabled ? t.disabled_foreground : on ? accent : t.muted_foreground), 20, m.radio_stroke);
        if (on)
            list.AddCircleFilled(center, m.radio_inner, u32(enabled ? accent : t.disabled_foreground), 16);
        break;
    case Indicator::Visibility:
        draw_icon(list, on ? Icon::Eye : Icon::EyeOff, center, m.menu_icon, u32(!enabled ? t.disabled_foreground : on ? accent : t.muted_foreground));
        break;
    case Indicator::Check:
        if (on)
            draw_icon(list, Icon::Check, center, m.menu_icon, u32(enabled ? accent : t.disabled_foreground));
        break;
    case Indicator::None: break;
    }
}

bool menu_row(const MenuItem& item, float width, float text_left, int index)
{
    const Metrics& m   = metrics();
    const Theme&   t   = theme();
    const float    h   = m.menu_row;
    const ImVec2   pos = ImGui::GetCursorScreenPos();
    ImGuiSelectableFlags flags = item.closes ? ImGuiSelectableFlags_None : ImGuiSelectableFlags_DontClosePopups;
    if (!item.availability.enabled)
        flags |= ImGuiSelectableFlags_Disabled;
    ImGui::PushID(index);
    const bool clicked = ImGui::Selectable("##row", false, flags, ImVec2(width, h)) && item.availability.enabled;
    ImGui::PopID();
    const bool  hovered = item.availability.enabled && ImGui::IsItemHovered();
    ImDrawList& list    = *ImGui::GetWindowDrawList();
    if (item.selected)
        fill_rounded(list, pos, ImVec2(pos.x + width, pos.y + h), u32(t.selection), m.menu_row_radius);
    if (hovered)
        fill_rounded(list, pos, ImVec2(pos.x + width, pos.y + h), u32(t.accent), m.menu_row_radius);
    const float line_y = pos.y + .5f * h;
    draw_mark(list, item.indicator, item.checked, item.availability.enabled, hovered, ImVec2(pos.x + m.menu_mark_center, line_y));
    const bool  tinted    = hovered || item.selected;
    const Color label_ink = !item.availability.enabled ? t.disabled_foreground : item.muted ? muted_text(t, tinted) : t.foreground;
    const float text_y    = line_y - .5f * ImGui::GetFontSize();
    list.AddText(ImVec2(pos.x + text_left, text_y), u32(label_ink), item.label.c_str());
    float right = pos.x + width - m.menu_key_right;
    for (const std::string* text : {&item.key, &item.value}) {
        if (text->empty())
            continue;
        const float w = FontMeasure().width(*text);
        list.AddText(ImVec2(right - w, text_y), u32(!item.availability.enabled ? t.disabled_foreground : muted_text(t, tinted)),
                     text->c_str());
        right -= w + m.menu_key_gap;
    }
    if (!item.availability.enabled && !item.availability.reason.empty())
        item_tooltip(item.availability.reason);
    return clicked;
}

void menu_group_label(const std::string& title)
{
    const Metrics& m   = metrics();
    const ImVec2   pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(1.f, m.menu_group_label));
    const Font     font = Context::current()->bold_font().resized(m.px(TextRole::label.size));
    ImGui::GetWindowDrawList()->AddText(face_of(font), font.size(),
                                        ImVec2(pos.x + m.menu_label_inset, pos.y + m.menu_group_label - font.size() - m.group_label_baseline),
                                        u32(theme().muted_foreground), title.c_str());
}

} // namespace

void open_popup(const char* id) { ImGui::OpenPopup(id); }

bool popup_open(const char* id) { return ImGui::IsPopupOpen(id); }

std::optional<int> show_menu_popup(const char* id, const Positioner& anchor, const std::vector<MenuItem>& entries, float min_width)
{
    const Metrics&              m       = metrics();
    const Size              size    = menu_size(m, FontMeasure(), entries, min_width);
    const ImVec2                screen  = ImGui::GetIO().DisplaySize;
    const float                 width   = size.width - 2.f * m.menu_padding_x;
    const float                 text_left = menu_text_left(m, entries);

    push_popup_style();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(m.menu_padding_x, m.menu_padding_y));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
    ImGui::SetNextWindowPos(ImVec2(std::round(position_popup_axis(anchor.x, size.width, m.card_gap, screen.x - m.card_gap)),
                                   std::round(position_popup_axis(anchor.y, size.height, m.card_gap, screen.y - m.card_gap))));
    ImGui::SetNextWindowSize(ImVec2(size.width, size.height));
    std::optional<int> picked;
    if (ImGui::BeginPopup(id, ImGuiWindowFlags_NoMove)) {
        if (ImGui::IsWindowAppearing())
            Context::current()->request_frame();
        draw_window_shadow(m.menu_radius, Elevation::overlay);
        fill_window(u32(theme().card), m.menu_radius);
        stroke_window(u32(theme().card_border), m.menu_radius, m.outline);
        for (size_t i = 0; i < entries.size(); ++i) {
            const MenuItem& item = entries[i];
            if (item.kind == MenuItem::Kind::GroupLabel)
                menu_group_label(item.label);
            else if (item.kind == MenuItem::Kind::Separator) {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(width, m.menu_separator));
                const float y = at.y + std::floor(.5f * m.menu_separator);
                ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, y), ImVec2(at.x + width, y), u32(theme().border), m.outline);
            } else if (menu_row(item, width, text_left, int(i)))
                picked = item.id;
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    pop_popup_style();
    if (picked)
        Context::current()->request_frame();
    return picked;
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
