#include "GLCanvas3D.hpp"

#include <optional>
#include <string>
#include <vector>

#include <Eigen/Geometry>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Navigator3D.hpp"
#include "slic3r/GUI/NavigatorGeometry.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Shortcuts.hpp"
#include "slic3r/GUI/CanvasUi/Menu.hpp"
#ifdef SLIC3R_CAD
#include "slic3r/GUI/CAD/DesignSketchTool.hpp"
#endif

namespace Slic3r {
namespace GUI {

static constexpr const char* NAVIGATOR_BACKGROUND_KEY = "navigator_background";

NavLayout GLCanvas3D::navigator_corner_layout() const
{
    const float bottom = float(get_canvas_size().get_height()) - preview_corner_offset();
    return navigator_layout(wxGetApp().app_config->get_bool(NAVIGATOR_BACKGROUND_KEY), get_ui_scale(), bottom);
}

float GLCanvas3D::navigator_corner_height() const
{
    return float(get_canvas_size().get_height()) - preview_corner_offset() - navigator_corner_layout().top();
}

void GLCanvas3D::_render_3d_navigator()
{
    static constexpr const char* HOME_VIEW_KEY        = "navigator_home_view";
    static constexpr const char* AUTO_PERSPECTIVE_KEY = "auto_perspective";
    AppConfig&                   config               = *wxGetApp().app_config;

    NavigatorFrame frame;
    frame.layout           = navigator_corner_layout();
    frame.blocked          = !m_navigator->holding() && (m_moving || m_rectangle_selection.is_dragging() || m_gizmos.is_dragging() ||
                                                        m_layers_editing.state == LayersEditing::Editing);
    frame.home_view        = navigator_view_from_string(config.get(HOME_VIEW_KEY));
    frame.auto_perspective = config.get_bool(AUTO_PERSPECTIVE_KEY);
    frame.shortcut         = [](Shortcut shortcut) { return wxGetApp().shortcuts().display(shortcut); };
    frame.display_toggles  = [this] { return _navigator_display_toggles(); };
    frame.fit              = [this] { _fit_camera(); };
    frame.frame_bed        = [this] { zoom_to_bed(); };
    frame.set_perspective  = [&config](bool perspective) {
        config.set_bool("use_perspective_camera", perspective);
        wxGetApp().update_ui_from_settings();
    };
    frame.toggle_auto_perspective = [&config] { config.set_bool(AUTO_PERSPECTIVE_KEY, !config.get_bool(AUTO_PERSPECTIVE_KEY)); };
    frame.set_home_view = [&config](const std::optional<Eigen::Quaterniond>& view) {
        config.set(HOME_VIEW_KEY, view ? navigator_view_to_string(*view) : std::string());
    };

    m_navigator->render(wxGetApp().plater()->get_camera(), frame);
}

void GLCanvas3D::_fit_camera()
{
#ifdef SLIC3R_CAD
    // The Design tab selects and sketches outside the canvas's selection and volumes, so
    // it names what to frame. Framed along the current view, which is often square to a
    // sketch plane; an empty tab falls through to the bed.
    const BoundingBoxf3 design_box = m_design_sketch_tool != nullptr ? m_design_sketch_tool->fit_box(m_volumes) : BoundingBoxf3();
    if (design_box.defined) {
        _zoom_to_box(design_box);
        return;
    }
#endif
    select_view("plate");
    if (m_selection.is_empty()) {
        if (m_canvas_type == ECanvasType::CanvasAssembleView)
            zoom_to_volumes();
        else
            zoom_to_bed();
    } else {
        zoom_to_selection();
    }
}

std::vector<NavigatorToggleGroup> GLCanvas3D::_navigator_display_toggles()
{
    using CanvasUi::Indicator;
    Plater*    p   = wxGetApp().plater();
    AppConfig* cfg = wxGetApp().app_config;

    const bool        prepare      = m_canvas_type == ECanvasType::CanvasView3D;
    const bool        preview      = m_canvas_type == ECanvasType::CanvasPreview;
    const bool        assembly     = m_canvas_type == ECanvasType::CanvasAssembleView;
    const std::string only_prepare = _u8L_CONTEXT("Available only in Prepare", "Navigator");
    const std::string not_preview  = _u8L_CONTEXT("Not available in Preview", "Navigator");
    const std::string not_assembly = _u8L_CONTEXT("Not available in Assembly View", "Navigator");
    const std::string not_design   = _u8L_CONTEXT("Not available in Design", "Navigator");

    NavigatorToggleGroup model{_u8L_CONTEXT("Model", "Navigator")};
    model.toggles = {
        {_u8L("Overhangs"), Indicator::Visibility, p->is_view3D_overhang_shown(), [p] { p->show_view3D_overhang(!p->is_view3D_overhang_shown()); },
         {prepare, only_prepare}},
        {_u8L("Outline"), Indicator::Visibility, wxGetApp().show_outline(), [this] { wxGetApp().toggle_show_outline(); m_dirty = true; }, {!preview, not_preview}},
        {_u8L("Wireframe"), Indicator::Visibility, p->is_show_wireframe(), [this, p] { p->toggle_show_wireframe(); m_dirty = true; }, {!preview, not_preview}},
        // TRN View toggle that draws models see-through, so hidden parts show.
        {_u8L("X-Ray"), Indicator::Visibility, p->is_show_xray(), [this, p] { p->toggle_show_xray(); m_dirty = true; }, {!preview, not_preview}},
        {_u8L_CONTEXT("Realistic view", "Navigator"), Indicator::Visibility, cfg->get_bool(SETTING_OPENGL_REALISTIC_MODE),
         [cfg] {
             cfg->set_bool(SETTING_OPENGL_REALISTIC_MODE, !cfg->get_bool(SETTING_OPENGL_REALISTIC_MODE));
             cfg->save();
         }},
    };

    NavigatorToggleGroup scene{_u8L_CONTEXT("Scene", "Navigator")};
    scene.toggles = {
        {_u8L("Axes"), Indicator::Visibility, m_show_world_axes, [this] { toggle_world_axes_visibility(false); }, {!assembly, not_assembly}},
        {_u8L("Gridlines"), Indicator::Visibility, wxGetApp().show_plate_gridlines(), [] { wxGetApp().toggle_show_plate_gridlines(); }, {!assembly, not_assembly}},
        {_u8L("Labels"), Indicator::Visibility, p->are_view3D_labels_shown(), [p] { p->show_view3D_labels(!p->are_view3D_labels_shown()); }, {prepare, only_prepare}},
        {_u8L("Center of mass"), Indicator::Visibility, wxGetApp().show_center_of_mass(),
         [this] { wxGetApp().toggle_show_center_of_mass(); m_dirty = true; }, {!assembly && !m_design_canvas, assembly ? not_assembly : not_design}},
        {_u8L("Focus mode"), Indicator::Check, wxGetApp().focus_mode(),
         [this] {
             wxGetApp().toggle_focus_mode();
             if (wxGetApp().focus_mode())
                 zoom_to_plate(REQUIRES_ZOOM_TO_CUR_PLATE);
             m_dirty = true;
         },
         {prepare && !m_design_canvas, only_prepare}},
        {_u8L_CONTEXT("Navigator background", "Navigator"), Indicator::Check, cfg->get_bool(NAVIGATOR_BACKGROUND_KEY),
         [this, cfg] {
             cfg->set_bool(NAVIGATOR_BACKGROUND_KEY, !cfg->get_bool(NAVIGATOR_BACKGROUND_KEY));
             m_dirty = true;
         }},
    };
    std::vector<NavigatorToggleGroup> groups = {model, scene};

    // Belt printers, G-code preview only: show the raw machine-frame G-code instead of the designed
    // (upright) view. This menu is the only place the toggle lives (plus its shortcut); the reload is
    // deferred (CallAfter) so the preview is not rebuilt mid-render.
    if (preview && m_gcode_viewer.is_belt_view()) {
        NavigatorToggleGroup belt{_u8L_CONTEXT("Belt printer", "Navigator")};
        belt.toggles = {{_u8L("Show raw G-code (belt only)"), Indicator::Visibility, !m_gcode_viewer.is_belt_show_designed(), [this, p] {
                             m_gcode_viewer.toggle_belt_show_designed();
                             p->CallAfter([p] { p->refresh_belt_view(); });
                         }}};
        groups.push_back(belt);
    }
    return groups;
}

} // namespace GUI
} // namespace Slic3r
