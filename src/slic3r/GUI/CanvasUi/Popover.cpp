#include "Popover.hpp"

#include <algorithm>
#include <cmath>

#include <imgui/imgui.h>

#include "ComponentParts.hpp"
#include "Context.hpp"
#include "Font.hpp"
#include "FontMeasure.hpp"
#include "ImGuiBridge.hpp"
#include "Input.hpp"
#include "Measure.hpp"
#include "Menu.hpp"
#include "MenuPopup.hpp"
#include "Metrics.hpp"
#include "Paint.hpp"
#include "Theme.hpp"
#include "Tokens.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

using namespace detail;

Popover::Popover(const char* id, const Positioner& anchor, const PopoverOptions& options)
{
    if (!popup_open(id))
        return;
    Context&       ui     = *Context::current();
    const Metrics& m      = ui.metrics();
    const Theme&   t      = ui.theme();
    const Font     bold   = ui.bold_font();
    const Size     size   = popover_size(m, FontMeasure(bold), options);
    const ImVec2   screen = ImGui::GetIO().DisplaySize;
    push_popup_style();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(m.popover_padding, m.popover_padding));
    ImGui::SetNextWindowPos(ImVec2(std::round(position_popup_axis(anchor.x, size.width, m.card_gap, screen.x - m.card_gap)),
                                   std::round(position_popup_axis(anchor.y, size.height, m.card_gap, screen.y - m.card_gap))));
    ImGui::SetNextWindowSize(im(size));
    m_open = ImGui::BeginPopup(id, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    pop_popup_style();
    if (!m_open)
        return;
    if (ImGui::IsWindowAppearing())
        ui.request_frame();
    if (escape_pressed())
        close();
    draw_window_shadow(m.popover_radius, Elevation::overlay);
    fill_window(u32(t.card), m.popover_radius);
    const Point origin = point(ImGui::GetWindowPos());
    const float left   = origin.x + m.popover_padding;
    const float top    = origin.y + m.popover_padding;
    Painter::current().text(bold, Rect(left, top, origin.x + size.width - m.popover_padding, top + m.popover_title), t.foreground, options.title);
    m_content = Rect(Point(left, top + m.popover_title + m.popover_title_gap), options.content);
    ImGui::PushStyleColor(ImGuiCol_Text, im(t.foreground));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(m.control_padding, m.item_gap));
    ImGui::SetCursorScreenPos(im(m_content.min()));
}

Popover::~Popover()
{
    if (!m_open)
        return;
    const Context& ui = *Context::current();
    stroke_window(u32(ui.theme().card_border), ui.metrics().popover_radius, ui.metrics().outline);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::EndPopup();
}

void Popover::close()
{
    if (!m_open)
        return;
    ImGui::CloseCurrentPopup();
    Context::current()->request_frame();
}

Size popover_size(const Metrics& metrics, const TextMeasure& title, const PopoverOptions& options)
{
    return {std::ceil(std::max(options.content.width, title.width(options.title)) + 2.f * metrics.popover_padding),
            std::ceil(metrics.popover_title + metrics.popover_title_gap + options.content.height + 2.f * metrics.popover_padding)};
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
