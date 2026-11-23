#include "CanvasFrame.hpp"

#include "Context.hpp"
#include "Font.hpp"
#include "Metrics.hpp"
#include "Theme.hpp"

namespace Slic3r {
namespace GUI {
namespace CanvasUi {

void CanvasFrame::begin(bool dark, float scale, Context::Clock::time_point now, int frame, IconCache* icons, const Fonts& fonts)
{
    Context& c = m_context;
    c.m_theme = &CanvasUi::theme(dark);
    c.m_metrics         = Metrics(scale);
    c.m_now             = now;
    c.m_frame           = frame;
    c.m_frame_requested = false;
    c.m_cursor          = Cursor::Default;
    c.m_wake_at.reset();
    c.m_tooltip_asked = false;
    c.m_icons         = icons;
    c.m_fonts         = fonts;
}

void CanvasFrame::end()
{
    if (!m_context.m_tooltip_asked)
        m_context.m_tooltip_id = 0;
}

} // namespace CanvasUi
} // namespace GUI
} // namespace Slic3r
