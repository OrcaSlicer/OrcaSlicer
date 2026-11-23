#include "GCodeViewer.hpp"
#include "PreviewPanels.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PreviewOverlayLayout.hpp"
#include "slic3r/GUI/PreviewPanelSlots.hpp"
#include "slic3r/GUI/CanvasUi/Context.hpp"
#include "slic3r/GUI/CanvasUi/Types.hpp"

namespace Slic3r {
namespace GUI {

bool GCodeViewer::is_legend_shown() const { return m_legend_enabled && wxGetApp().show_legend(); }

bool GCodeViewer::is_summary_shown() const { return m_legend_enabled && wxGetApp().show_print_summary(); }

bool GCodeViewer::is_panel_shown(PreviewPanel panel) const
{
    switch (panel) {
    case PreviewPanel::Legend: return is_legend_shown();
    case PreviewPanel::Summary: return is_summary_shown();
    case PreviewPanel::GCode: return !m_no_render_path && wxGetApp().show_gcode_window();
    case PreviewPanel::Count: break;
    }
    return false;
}

PreviewPanelPlacement GCodeViewer::panel_place(PreviewPanel panel, const PreviewOverlayLayout& layout) const
{
    PreviewPanelHeights heights{};
    PreviewPanelShown   shown{};
    for (size_t i = 0; i < PREVIEW_PANEL_COUNT; ++i) {
        heights[i] = m_panel_rects[i].height();
        shown[i]   = is_panel_shown(PreviewPanel(i));
    }
    PreviewPanelPlacement place = layout.place_panels(m_panel_slots, heights, shown)[size_t(panel)];
    // The G-code panel keeps clear of the move panel itself, as the move panel is drawn just before it.
    const CanvasUi::Rect& last = m_panel_rects[size_t(panel)];
    if (panel != PreviewPanel::GCode && last.width() > 0.f)
        place.bottom = std::max(place.top, layout.bottom_clear_of(m_sequential_view.marker.panel_rect(), last.left, last.right, place.bottom));
    return place;
}

void GCodeViewer::panel_drawn(PreviewPanel panel, const CanvasUi::Rect& rect)
{
    CanvasUi::Rect& last = m_panel_rects[size_t(panel)];
    // Placing the panels takes these heights, so a change lays them out again in the next frame.
    if (rect.height() != last.height())
        CanvasUi::Context::current()->request_frame();
    last = rect;
}

void GCodeViewer::render_all_plates_stats(const std::vector<const GCodeProcessorResult*>& gcode_result_list, bool show) const
{
    m_panels->render_all_plates_stats(gcode_result_list, show);
}

} // namespace GUI
} // namespace Slic3r
