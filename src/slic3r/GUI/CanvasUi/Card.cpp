#include "Card.hpp"

#include <algorithm>

#include <imgui/imgui.h>

#include "Context.hpp"
#include "Corners.hpp"
#include "ImGuiBridge.hpp"
#include "Metrics.hpp"
#include "Paint.hpp"
#include "Theme.hpp"
#include "Tokens.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

namespace {

// The card draws its own fill and outline; the ImGui window under it is only the place, the clip and the input.
void push_panel_style(const Theme& theme)
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_Text, im(theme.foreground));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
}

void pop_panel_style()
{
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

} // namespace

Card::Card(const char* id, const Point& pos, const Size& size, const CardStyle& style)
{
    const Context& ui      = *Context::current();
    const Metrics& m       = ui.metrics();
    const float    padding = style.padding < 0.f ? m.card_padding : style.padding;
    m_radius               = m.card_radius;

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::SetNextWindowPos(im(pos), ImGuiCond_Always, im(style.pivot));
    ImGui::SetNextWindowSize(ImVec2(std::max(size.width, 0.f), std::max(size.height, 0.f)), ImGuiCond_Always);

    push_panel_style(ui.theme());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
    ImGui::Begin(id, nullptr, flags);
    ImGui::PopStyleVar();
    pop_panel_style();
    m_rect = Rect(point(ImGui::GetWindowPos()), Size(ImGui::GetWindowWidth(), ImGui::GetWindowHeight()));
    if (style.shadow)
        draw_window_shadow(m_radius, Elevation::card);
    fill_window(u32(ui.theme().card), m_radius);
    m_list    = ImGui::GetWindowDrawList();
    m_hovered = ImGui::IsWindowHovered();
    ImGui::PushStyleColor(ImGuiCol_Text, im(ui.theme().foreground));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(m.control_padding, m.item_gap));
    ImGui::PushID(id);
}

Card::~Card()
{
    // The outline goes over the content, so fills across the card never hide it.
    const Context& ui = *Context::current();
    stroke_window(u32(ui.theme().card_border), m_radius, ui.metrics().outline);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PopID();
    ImGui::End();
}

Painter Card::painter() const { return Painter(*m_list); }

bool Card::hovered() const { return m_hovered; }

void Card::row_background(float top, float bottom, const Color& color) const
{
    const Metrics& m      = Context::current()->metrics();
    const float    inset  = m.outline;
    top                   = std::max(top, m_rect.top + inset);
    bottom                = std::min(bottom, m_rect.bottom - inset);
    if (bottom <= top)
        return;
    // The card's own inner shape cut to the row, so rows next to each other meet without notches at the corners.
    m_list->PushClipRect(ImVec2(m_rect.left + inset, top), ImVec2(m_rect.right - inset, bottom), true);
    fill_rounded(*m_list, ImVec2(m_rect.left + inset, m_rect.top + inset), ImVec2(m_rect.right - inset, m_rect.bottom - inset), u32(color),
                 nested_radius(m_radius, inset));
    m_list->PopClipRect();
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
