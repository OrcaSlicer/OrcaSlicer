#include "PanelCard.hpp"

#include <algorithm>
#include <functional>
#include <string>

#include "Button.hpp"
#include "Card.hpp"
#include "Context.hpp"
#include "Icons.hpp"
#include "Layout.hpp"
#include "Measure.hpp"
#include "Metrics.hpp"
#include "Paint.hpp"
#include "ScrollArea.hpp"
#include "Types.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

void PanelCollapse::set(bool collapsed)
{
    if (collapsed == m_collapsed)
        return;
    m_collapsed = collapsed;
    if (save)
        save(collapsed);
}

bool PanelCollapse::take_pending()
{
    if (!m_pending)
        return false;
    m_pending = false;
    set(!m_collapsed);
    return true;
}

float PanelCardOptions::card_width(const Metrics& metrics) const { return width > 0.f ? width : metrics.panel_width; }

PanelCard::PanelCard(const char* id, const PanelCardOptions& options, float& body_content) : m_options(options), m_content(body_content)
{
    const Metrics& m = Context::current()->metrics();
    m_width          = options.card_width(m);
    m_body           = options.collapsed ? 0.f : std::min(body_content, std::max(0.f, options.room - m.panel_header - options.extra));
    CardStyle style;
    style.pivot   = Point(options.right ? 1.f : 0.f, 0.f);
    style.padding = 0.f;
    m_card.emplace(id, options.at, Size(m_width, m.panel_header + (options.collapsed ? 0.f : options.extra) + m_body), style);
}

float PanelCard::header() const { return Context::current()->metrics().panel_header; }

bool PanelCard::collapse_button(const char* id, bool collapsed, const std::string& tooltip) const
{
    const Metrics& m = Context::current()->metrics();
    ButtonOptions  options;
    options.size    = ButtonSize::Icon;
    options.width   = m.panel_header_button;
    options.icon    = collapsed ? Icon::ChevronsDown : Icon::ChevronsUp;
    options.tooltip = tooltip;
    place(Point(origin().x + m.inline_gap, origin().y + .5f * (m.panel_header - m.panel_header_button)));
    return button(id, "", options);
}

float PanelCard::title_left() const
{
    const Metrics& m = Context::current()->metrics();
    return origin().x + m.inline_gap + m.panel_header_button + m.inline_gap;
}

void PanelCard::body(const char* id, float inset, const std::function<float(const Painter&, float, float, float)>& draw) const
{
    if (m_options.collapsed)
        return;
    const Metrics& m   = Context::current()->metrics();
    const Point    top = origin();
    m_card->painter().divider(top.x, top.x + m_width, top.y + m.panel_header);
    place(Point(top.x + m.outline, top.y + m.panel_header + m_options.extra));
    const ScrollArea scroll(id, Size(m_width - 2.f * m.outline, m_body), m_content);
    const float      height = draw(scroll.painter(), scroll.origin().x - m.outline + inset, scroll.origin().y, m_width - 2.f * inset);
    if (height != m_content) {
        m_content = height;
        Context::current()->request_frame();
    }
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
