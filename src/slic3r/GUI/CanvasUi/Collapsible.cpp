#include "Collapsible.hpp"

#include <optional>
#include <string>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include "Button.hpp"
#include "ComponentParts.hpp"
#include "Input.hpp"
#include "Layout.hpp"
#include "Metrics.hpp"
#include "Paint.hpp"
#include "ScrollArea.hpp"
#include "Tabs.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

using detail::metrics;

namespace {

// The canvas closes the popup on top when Esc goes down, before the frame that sees the key.
bool popup_shown_last_frame()
{
    for (const ImGuiWindow* window : GImGui->Windows)
        if ((window->Flags & ImGuiWindowFlags_Popup) != 0 && window->WasActive)
            return true;
    return false;
}

} // namespace

bool collapsible_trigger(const char* id, const std::string& label, bool& open, ButtonOptions options)
{
    options.pressed = open;
    if (!button(id, label, options))
        return false;
    open = !open;
    return true;
}

void collapsible_close_on_escape(bool& open)
{
    if (open && escape_pressed() && !input_active() && !popup_shown_last_frame())
        open = false;
}

float collapsible_panel_height(const Metrics& metrics, float content_height)
{
    return metrics.tabs_list_height + metrics.collapsible_content_top + content_height + metrics.collapsible_content_bottom;
}

CollapsiblePanel::CollapsiblePanel(const char* id, const Point& pos, const CollapsiblePanelOptions& options)
    : m_rect(pos, Size(options.width, options.height)), m_below(options.below)
{
    const Metrics& m = metrics();
    m_content_width  = options.width - 2.f * m.card_padding;
    place(Point(pos.x + m.outline, pos.y));
    m_scroll.emplace(id, Size(options.width - 2.f * m.outline, options.height), collapsible_panel_height(m, options.content_height));
    const float    top = m_scroll->origin().y;
    TabsBarOptions bar;
    bar.width         = options.width;
    bar.changed_count = options.changed_count;
    bar.reset_label   = options.reset_label;
    bar.can_reset     = options.can_reset;
    int  no_tab       = 0;
    place(Point(pos.x, top));
    m_reset = tabs_bar("##tabs", options.tab != nullptr ? *options.tab : no_tab, options.tabs, bar).reset;
    place(Point(pos.x + m.card_padding, top + m.tabs_list_height + m.collapsible_content_top));
    m_group.emplace();
}

CollapsiblePanel::~CollapsiblePanel()
{
    m_group.reset();
    m_scroll.reset();
    const Painter paint = Painter::current();
    paint.divider(m_rect.left, m_rect.right, m_rect.bottom);
    if (m_below > 0.f) {
        const float         outline = metrics().outline;
        const Painter::Clip clip(paint, Rect(m_rect.left + outline, m_rect.bottom, m_rect.right - outline, m_rect.bottom + m_below));
        paint.shadow_below(m_rect.left + outline, m_rect.right - outline, m_rect.bottom);
    }
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
