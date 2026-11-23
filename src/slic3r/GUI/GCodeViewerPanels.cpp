#include "GCodeViewer.hpp"
#include "PreviewPanels.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libvgcode/include/PathVertex.hpp"
#include "libvgcode/include/Types.hpp"
#include "libvgcode/include/Viewer.hpp"
#include "slic3r/GUI/GCodePanelText.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/IMSlider.hpp"
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

// The first vertex of the line or, if the line moves nothing, of the next line that does; the vertex count past the last.
static size_t first_vertex_from_line(const libvgcode::Viewer& viewer, uint64_t line)
{
    size_t lo = 0;
    size_t hi = viewer.get_vertices_count();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (viewer.get_vertex_at(mid).gcode_id < line)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

void GCodeViewer::bind_gcode_panel()
{
    m_sequential_view.gcode_window.load_settings(*wxGetApp().app_config);
    m_sequential_view.gcode_window.save_settings = [](const GCodePanelSettings& settings) {
        save_gcode_panel_settings(*wxGetApp().app_config, settings);
    };
    m_sequential_view.gcode_window.collapse.save = [](bool collapsed) {
        save_preview_panel_collapsed(*wxGetApp().app_config, PreviewPanel::GCode, collapsed);
    };
    m_sequential_view.gcode_window.line_color = [this](uint64_t line) -> std::optional<CanvasUi::Color> {
        const size_t vertex = first_vertex_from_line(m_viewer, line);
        if (vertex == m_viewer.get_vertices_count())
            return std::nullopt;
        const libvgcode::PathVertex& v = m_viewer.get_vertex_at(vertex);
        if (v.gcode_id != line)
            return std::nullopt;
        const libvgcode::EOptionType option = libvgcode::move_type_to_option(v.type);
        if (!v.is_extrusion() && option == libvgcode::EOptionType::COUNT)
            return std::nullopt;
        const libvgcode::Color& c = v.is_extrusion() && !v.is_wipe() ? m_viewer.get_extrusion_role_color(v.role) : m_viewer.get_option_color(option);
        return CanvasUi::Color::from_bytes(c[0], c[1], c[2]);
    };
    m_sequential_view.gcode_window.go_to_line = [this](uint64_t line) {
        const size_t vertex = first_vertex_from_line(m_viewer, line);
        if (vertex == m_viewer.get_vertices_count())
            return;
        const libvgcode::PathVertex& v = m_viewer.get_vertex_at(vertex);
        const int                    layer = int(v.layer_id);
        if (m_layers_slider->GetLowerValue() > layer)
            m_layers_slider->SetLowerValue(layer);
        m_layers_slider->SetHigherValue(layer);
        m_layers_slider->set_as_dirty();
        m_pending_gcode_id = v.gcode_id;
        CanvasUi::Context::current()->request_frame();
    };
}

// The move slider follows a jump to a line once the layer slider has taken the layer, which loads that layer's moves.
void GCodeViewer::apply_pending_gcode_jump()
{
    if (!m_pending_gcode_id || m_layers_slider->is_dirty())
        return;
    const std::vector<double>& ids = m_moves_slider->GetSliderAlternateValues();
    const auto                 it  = std::lower_bound(ids.begin(), ids.end(), double(*m_pending_gcode_id));
    if (it != ids.end()) {
        m_moves_slider->SetHigherValue(int(it - ids.begin()));
        m_moves_slider->set_as_dirty();
    }
    m_pending_gcode_id.reset();
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
