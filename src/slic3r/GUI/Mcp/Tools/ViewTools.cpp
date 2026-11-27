// The place View and windows: view_set, pointing the view before looking: the main tab, the 3D view and its camera,
// the Preview's colour scheme, legend and layers, and the display switches; window_list and window_set, the app's
// windows listed, sized, moved and raised; and the window commands made from menu items, window_open and
// view_toggle_activity_panel.

#include "slic3r/GUI/Mcp/AutomationRuntime.hpp"
#include "slic3r/GUI/Mcp/McpCore.hpp"
#include "slic3r/GUI/Mcp/McpEndpoint.hpp"
#include "slic3r/GUI/Mcp/McpRecorder.hpp"
#include "slic3r/GUI/Mcp/McpTools.hpp"
#include "slic3r/GUI/Mcp/ModalTracker.hpp"
#include "slic3r/GUI/Mcp/PythonScriptsDialog.hpp"
#include "slic3r/GUI/Mcp/ScreenCapture.hpp"
#include "slic3r/GUI/Mcp/UiInspector.hpp"
#include "slic3r/GUI/Mcp/WebPages.hpp"
#include "slic3r/GUI/Mcp/WindowControl.hpp"
#include "slic3r/GUI/Mcp/WindowNames.hpp"
#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/LibVGCode/LibVGCodeWrapper.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/NativeCommandMenuItems.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Notebook.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Operations/PlateOps.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Point.hpp"
#include "libvgcode/include/Types.hpp"
#include "libvgcode/include/Viewer.hpp"

#include <wx/app.h>
#include <wx/defs.h>
#include <wx/dialog.h>
#include <wx/display.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/glcanvas.h>
#include <wx/toplevel.h>
#include <wx/weakref.h>
#include <wx/window.h>

#include <boost/beast/core/detail/base64.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifdef SLIC3R_CAD
#include "slic3r/GUI/Mcp/McpToolsDesign.hpp"
#include "slic3r/GUI/CAD/DesignCanvas.hpp"
#include "slic3r/GUI/CAD/DesignPanel.hpp"
#endif

#if defined(__WXGTK__)
#include "slic3r/GUI/LinuxDisplayBackend.hpp"
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

// The ids of the main window's notebook pages, in the notebook's order.
std::vector<std::string> notebook_pages(const Notebook& notebook)
{
    std::vector<std::string> pages;
    for (size_t i = 0; i < notebook.GetPageCount(); ++i)
        pages.push_back(into_u8(notebook.GetPageName(i)));
    return pages;
}

// A legend key in any case, with _ or a space between words: "Filament changes" is filament_changes,
// "sparse_infill" is Sparse infill.
bool same_legend_key(const std::string& given, const std::string& name)
{
    const auto fold = [](char c) { return c == '_' ? ' ' : char(std::tolower(static_cast<unsigned char>(c))); };
    return given.size() == name.size() && std::equal(given.begin(), given.end(), name.begin(), [&fold](char a, char b) { return fold(a) == fold(b); });
}

// Whether `key` names a legend row some print has, this one or not.
bool any_legend_row(const std::string& key)
{
    for (std::size_t r = 0; r < libvgcode::GCODE_EXTRUSION_ROLES_COUNT; ++r)
        if (same_legend_key(key, legend_feature_key(libvgcode::EGCodeExtrusionRole(r))))
            return true;
    for (std::size_t o = 0; o < libvgcode::OPTION_TYPES_COUNT; ++o)
        if (const char* name = legend_option_key(libvgcode::EOptionType(o)); name != nullptr && same_legend_key(key, name))
            return true;
    return false;
}

// view_set's legend argument: each named row shown or hidden as a click on it does, clicked only when its
// state differs. The rows are the feature types and options the print has, whatever the view type: the
// viewer keeps one visibility set, though the legend lists feature types only in Line Type. The keys are
// checked here, before the Preview changes; the returned function applies them and gives each row's
// state afterwards. UI thread.
std::function<json()> legend_change(GCodeViewer& viewer, const json& legend)
{
    const libvgcode::Viewer&                        toolpaths = viewer.get_viewer();
    const std::vector<libvgcode::EGCodeExtrusionRole> roles   = toolpaths.get_extrusion_roles();
    const std::vector<libvgcode::EOptionType>&      options   = toolpaths.get_options();
    std::vector<std::pair<libvgcode::EGCodeExtrusionRole, bool>> role_rows;
    std::vector<std::pair<libvgcode::EOptionType, bool>>         option_rows;
    for (auto item = legend.begin(); item != legend.end(); ++item) {
        const std::string& key = item.key();
        if (!item.value().is_boolean())
            throw ToolFailure("legend \"" + key + "\" must be true (show) or false (hide)");
        const auto role   = std::find_if(roles.begin(), roles.end(), [&key](libvgcode::EGCodeExtrusionRole r) { return same_legend_key(key, legend_feature_key(r)); });
        const auto option = std::find_if(options.begin(), options.end(), [&key](libvgcode::EOptionType o) {
            const char* name = legend_option_key(o);
            return name != nullptr && same_legend_key(key, name);
        });
        if (role != roles.end())
            role_rows.emplace_back(*role, item.value().get<bool>());
        else if (option != options.end())
            option_rows.emplace_back(*option, item.value().get<bool>());
        else if (!item.value().get<bool>() && any_legend_row(key))
            continue; // a row this print lacks shows nothing, so it is hidden already
        else {
            std::string features, keys;
            for (const libvgcode::EGCodeExtrusionRole r : roles)
                features += (features.empty() ? "" : ", ") + legend_feature_key(r);
            for (const libvgcode::EOptionType o : options)
                if (const char* name = legend_option_key(o))
                    keys += (keys.empty() ? "" : ", ") + std::string(name);
            throw ToolFailure("Unknown legend item \"" + key + "\"; this print has " + features +
                              (keys.empty() ? std::string() : " and the options " + keys));
        }
    }
    return [&viewer, role_rows, option_rows]() {
        const libvgcode::Viewer& toolpaths = viewer.get_viewer();
        json                     done      = json::object();
        for (const auto& [role, show] : role_rows) {
            if (toolpaths.is_extrusion_role_visible(role) != show)
                viewer.toggle_extrusion_role_visibility(role);
            done[legend_feature_key(role)] = toolpaths.is_extrusion_role_visible(role);
        }
        for (const auto& [option, show] : option_rows) {
            if (toolpaths.is_option_visible(option) != show)
                viewer.toggle_option_visibility(option);
            done[legend_option_key(option)] = toolpaths.is_option_visible(option);
        }
        return done;
    };
}

// Where the camera is seen from the point it looks at, in radians: {elevation above the bed's plane,
// azimuth (0 on the +X side, -pi/2 in front)}. Seen from straight above or below, the azimuth is that of
// the side the screen's bottom faces.
std::pair<double, double> camera_angles(Camera& camera)
{
    const Vec3d  offset   = camera.get_position() - camera.get_target();
    const double distance = offset.norm();
    Vec3d        flat(offset.x(), offset.y(), 0.);
    if (flat.norm() < 1e-6 * distance)
        flat = -camera.get_dir_up();
    return {std::asin(std::clamp(offset.z() / distance, -1., 1.)), std::atan2(flat.y(), flat.x())};
}

// Angles in radians as view_set replies with them: whole degrees, the azimuth from -180 to 180.
json angles_json(double elevation, double azimuth)
{
    return {{"elevation", std::lround(Geometry::rad2deg(elevation))}, {"azimuth", std::lround(std::remainder(Geometry::rad2deg(azimuth), 360.))}};
}

// view_set's orbit: the camera moved around the point it looks at, `yaw` degrees to its right
// (counterclockwise seen from above) and `pitch` degrees up, at the same distance. It is placed from its
// position rather than turned as a mouse drag turns it, so the result is the same whatever the user's
// mouse settings and free-camera preference. The elevation stops a degree short of straight up or down,
// where an upright camera has no up direction. Returns where it ends up, as camera_angles() reads it, in
// degrees.
json orbit_camera(Camera& camera, double yaw, double pitch)
{
    const Vec3d  target   = camera.get_target();
    const double distance = (camera.get_position() - target).norm();
    const auto [from_elevation, from_azimuth] = camera_angles(camera);
    const double limit     = Geometry::deg2rad(89.);
    const double azimuth   = from_azimuth + Geometry::deg2rad(yaw);
    const double elevation = std::clamp(from_elevation + Geometry::deg2rad(pitch), -limit, limit);
    const Vec3d  dir(std::cos(elevation) * std::cos(azimuth), std::cos(elevation) * std::sin(azimuth), std::sin(elevation));
    camera.look_at(target + distance * dir, target, Vec3d::UnitZ());
    return angles_json(elevation, azimuth);
}

// Whether the camera has a fit queued for the next frame of its 3D view (after a tab switch, a new project or
// a load), which would replace a look_at or zoom set before it.
bool fit_pending(const Camera& camera)
{
    return camera.requires_zoom_to_bed || camera.requires_zoom_to_volumes || camera.requires_zoom_to_plate != REQUIRES_ZOOM_TO_PLATE_IDLE;
}

// Why fit_bed does nothing on the Design tab, which a pick of Fit Bed to View shows there.
Ops::Refusal no_bed_in_design()
{
    return Ops::Refusal{Ops::RefusalKind::Precondition, "no_bed_in_design", L("The Design tab has no bed to fit the view to.")};
}

// view_set's camera, and a pick of a camera action: a named direction or a fit of the shown 3D view. On the Design tab
// it points the Design view as its own controls do, `default` being its Home view and `sketch_normal` the sketch's N
// key. The refusal is the one an action can reach, fit_bed on the Design tab; fit_selection and sketch_normal, which no
// action gives, throw. UI thread.
std::optional<Ops::Refusal> point_camera(Plater& plater, const std::string& cam)
{
#ifdef SLIC3R_CAD
    if (DesignPanel* design = wxGetApp().mainframe->shown_design_panel()) {
        DesignCanvas* view = design->mcp_viewport();
        if (view == nullptr)
            throw Mcp::no_design_view();
        if (cam == "fit_bed")
            return no_bed_in_design();
        if (cam == "fit_objects")
            view->fit_view();
        else if (cam == "fit_selection") {
            if (!view->zoom_to_box(view->selection_box()))
                throw ToolFailure("Nothing is selected in the Design tab");
        } else if (cam == "sketch_normal") {
            if (!view->view_normal_to_sketch())
                throw ToolFailure("No sketch is open in the Design tab");
        } else
            view->set_view(cam == "default" ? "iso" : cam);
        return std::nullopt;
    }
#endif
    GLCanvas3D* canvas = plater.get_current_canvas3D();
    if (cam == "default") {
        plater.select_view("plate");
        if (canvas)
            canvas->zoom_to_bed();
    } else if (cam == "fit_bed") {
        if (canvas)
            canvas->zoom_to_bed();
    } else if (cam == "fit_objects") {
        if (canvas)
            canvas->zoom_to_volumes();
    } else if (cam == "fit_selection") {
        if (canvas)
            canvas->zoom_to_selection();
    } else {
        plater.select_view(cam);
    }
    return std::nullopt;
}

// Asks the shown 3D view, the Design view on the Design tab, to draw again. UI thread.
void repaint_shown_canvas()
{
    if (GLCanvas3D* canvas = shown_canvas()) {
        canvas->set_as_dirty();
        if (wxGLCanvas* widget = canvas->get_wxglcanvas())
            widget->Refresh();
    }
}

// The 3D view of Prepare or the Preview whose axes switch app_state reads: the one shown, else Prepare's. UI thread.
GLCanvas3D* axes_canvas(Plater& plater)
{
    GLCanvas3D* preview = plater.get_preview_canvas3D();
    return plater.is_preview_shown() && preview != nullptr ? preview : plater.get_view3D_canvas3D();
}

// view_set's display switches of Prepare and the Preview, under the names it and app_state give them: the switches of
// the View menu and of the 3D view's own menu (the canvas toolbar's), each read as its check mark or eye shows it and set
// as its item sets it, where it differs. labels and overhangs are Prepare's 3D view's, which its items set while it is
// shown, and are set on it from any tab; Prepare's and the Preview's 3D views each draw the axes by a switch of their
// own, which the menu sets on the view shown, and axes sets both. The others are the app's. UI thread.
struct ViewSwitch
{
    const char* name;
    bool (*shown)(Plater&);
    void (*show)(Plater&, bool);
};

const ViewSwitch kViewSwitches[] = {
    {"labels", [](Plater& plater) { return plater.get_view3D_canvas3D()->are_labels_shown(); },
     [](Plater& plater, bool on) {
         plater.get_view3D_canvas3D()->show_labels(on);
         wxGetApp().app_config->set_bool("show_labels", on);
     }},
    {"overhangs", [](Plater& plater) { return plater.get_view3D_canvas3D()->is_overhang_shown(); },
     [](Plater& plater, bool on) {
         plater.get_view3D_canvas3D()->show_overhang(on);
         wxGetApp().app_config->set_bool("show_overhang", on);
     }},
    {"outline", [](Plater&) { return wxGetApp().show_outline(); },
     [](Plater&, bool on) {
         if (wxGetApp().show_outline() != on)
             wxGetApp().toggle_show_outline();
     }},
    {"wireframe", [](Plater& plater) { return plater.is_show_wireframe(); },
     [](Plater& plater, bool on) {
         if (plater.is_show_wireframe() != on)
             plater.toggle_show_wireframe();
     }},
    {"gridlines", [](Plater&) { return wxGetApp().show_plate_gridlines(); },
     [](Plater&, bool on) {
         if (wxGetApp().show_plate_gridlines() != on)
             wxGetApp().toggle_show_plate_gridlines();
     }},
    {"axes", [](Plater& plater) { return axes_canvas(plater)->are_world_axes_shown(); },
     [](Plater& plater, bool on) {
         for (GLCanvas3D* canvas : {plater.get_view3D_canvas3D(), plater.get_preview_canvas3D()})
             if (canvas != nullptr && canvas->are_world_axes_shown() != on)
                 canvas->toggle_world_axes_visibility(false);
     }},
    {"navigator", [](Plater&) { return wxGetApp().show_3d_navigator(); },
     [](Plater&, bool on) {
         if (wxGetApp().show_3d_navigator() != on)
             wxGetApp().toggle_show_3d_navigator();
     }},
};

// The section view of Prepare and the Preview, whose 3D views share it: on, as a right click on its toolbar button turns
// it on, at the cut it had or through the middle, and off. UI thread.
bool section_view_on(Plater& plater) { return plater.get_view3D_canvas3D()->is_section_view_active(); }

void show_section_view(Plater& plater, bool on)
{
    GLCanvas3D* canvas = plater.get_view3D_canvas3D();
    if (canvas->is_section_view_active() != on)
        canvas->toggle_section_view();
}

// view_set's tab, and a pick of Go to tab: shows the main window's page `tab` names (view_tab_page()). Refused when no
// page has that name, named as given, or when the page refuses to open, named by its id as the Speed Dial lists it.
// UI thread.
std::optional<Ops::Refusal> show_tab(MainFrame& frame, const std::string& tab)
{
    const std::optional<std::string> page = frame.m_tabpanel != nullptr ? view_tab_page(tab, notebook_pages(*frame.m_tabpanel)) :
                                                                           std::nullopt;
    if (!page)
        // TRN %1% is the tab id the command was given, such as "preview".
        return Ops::Refusal{Ops::RefusalKind::Precondition, "no_such_tab", L("No tab has the id \"%1%\"."), {tab}};
    frame.select_tab(from_u8(*page));
    // The tab can refuse to open: the Device tab of a Bambu Lab printer without its network plugin.
    if (frame.m_tabpanel->GetSelectedPageName() != from_u8(*page))
        // TRN %1% is the id of the tab that could not be opened, such as "monitor".
        return Ops::Refusal{Ops::RefusalKind::Failed, "tab_not_opened", L("The \"%1%\" tab could not be opened."), {*page}};
    return std::nullopt;
}

// The Preview tab, which a preview field given without tab or canvas, and a pick of Go to layer, show. UI thread.
void show_preview(Plater& plater, MainFrame& frame)
{
    plater.select_view_3D("Preview", false);
    frame.select_tab(wxString(TAB_ID_PREVIEW));
}

// view_set's layer_percent, and a pick of Go to layer: the top of the Preview's shown range moves to the layer that
// share of the plate's layers stands for, and its bottom stays. UI thread.
Ops::Outcome<Ops::ShownLayers> show_layer_percent(double percent)
{
    const Ops::Outcome<int> count = Ops::layer_count();
    if (!count)
        return count.refusal();
    Ops::ShowLayerRangeArgs range;
    range.last = int(std::lround(percent / 100.0 * double(count.value() - 1))) + 1;
    return Ops::show_layer_range(range);
}

// A person's pick of one of view_set's actions, in place on the UI thread as its menu item ran: for each argument the
// function a call's UI closure runs, and a refusal in the UI's language, Go to tab's as an error and the others, which say
// that the view has nothing to show it, as information.
AppActionRunResult picked_view(const json& action_args)
{
    Plater*    plater = wxGetApp().plater();
    MainFrame* frame  = wxGetApp().mainframe;
    if (plater == nullptr || frame == nullptr)
        return AppActionRunResult{};
    if (action_args.contains("layout"))
        return MenuItems::reset_window_layout();
    if (action_args.contains("projection"))
        return MenuItems::toggle_perspective();
    if (action_args.contains("tab")) {
        if (const std::optional<Ops::Refusal> refusal = show_tab(*frame, arg_string(action_args, "tab")))
            return NativeCommands::refused(*refusal);
        return AppActionRunResult{AppActionRunResult::Level::Succeeded};
    }
    if (action_args.contains("layer_percent")) {
        show_preview(*plater, *frame);
        const Ops::Outcome<Ops::ShownLayers> shown = show_layer_percent(action_args.at("layer_percent").get<double>());
        return shown ? AppActionRunResult{AppActionRunResult::Level::Succeeded} :
                       NativeCommands::refused(shown.refusal(), AppActionRunResult::Level::Info);
    }
    if (const std::optional<Ops::Refusal> refusal = point_camera(*plater, arg_string(action_args, "camera")))
        return NativeCommands::refused(*refusal, AppActionRunResult::Level::Info);
    repaint_shown_canvas();
    return AppActionRunResult{AppActionRunResult::Level::Succeeded};
}

} // namespace

json camera_state(Camera& camera, double canvas_scale)
{
    // Adding 0 turns a rounded -0 into 0.
    const auto point = [](const Vec3d& p) { return json{rounded(p.x(), 1e3) + 0., rounded(p.y(), 1e3) + 0., rounded(p.z(), 1e3) + 0.}; };
    const auto [elevation, azimuth] = camera_angles(camera);
    json state               = angles_json(elevation, azimuth);
    state["type"]            = camera.get_type() == Camera::EType::Ortho ? "ortho" : "perspective";
    state["look_at"]         = point(camera.get_target());
    state["position"]        = point(camera.get_position());
    state["zoom"]            = rounded(camera.get_zoom(), 1e4);
    state["click_px_per_mm"] = rounded(camera.get_zoom() / canvas_scale, 1e4);
    return state;
}

json view_state(Plater& plater)
{
    GLCanvas3D* preview = plater.get_preview_canvas3D();
    json        state{{"sidebar_collapsed", plater.is_sidebar_collapsed()},
                      {"sidebar_width", plater.sidebar_width()},
                      {"legend_collapsed", preview != nullptr && preview->get_gcode_viewer().is_legend_folded()},
                      {"gcode_window", wxGetApp().show_gcode_window()},
                      {"projection", plater.get_camera().get_type_as_string()},
                      {"section", {{"on", section_view_on(plater)}}}};
    for (const ViewSwitch& view_switch : kViewSwitches)
        state[view_switch.name] = view_switch.shown(plater);
    return state;
}

std::optional<std::string> view_tab_page(const std::string& tab, const std::vector<std::string>& pages)
{
    const auto has = [&pages](const std::string& page) { return std::find(pages.begin(), pages.end(), page) != pages.end(); };
    for (const TabWord& w : kTabWords) {
        if (tab == w.word)
            return has(w.page) ? std::optional<std::string>(w.page) : std::nullopt;
        // A page a word names is reached by its word: the Device tab is device, never monitor.
        if (tab == w.page)
            return std::nullopt;
    }
    return has(tab) ? std::optional<std::string>(tab) : std::nullopt;
}

std::string legend_feature_key(libvgcode::EGCodeExtrusionRole role) { return ExtrusionEntity::role_to_string(libvgcode::convert(role)); }

const char* legend_option_key(libvgcode::EOptionType type)
{
    switch (type) {
    case libvgcode::EOptionType::Travels: return "travel";
    case libvgcode::EOptionType::Wipes: return "wipe";
    case libvgcode::EOptionType::Retractions: return "retract";
    case libvgcode::EOptionType::Unretractions: return "unretract";
    case libvgcode::EOptionType::Seams: return "seams";
    case libvgcode::EOptionType::ToolChanges: return "filament_changes";
    case libvgcode::EOptionType::ColorChanges: return "color_changes";
    case libvgcode::EOptionType::PausePrints: return "pauses";
    case libvgcode::EOptionType::CustomGCodes: return "custom_gcode";
    default: return nullptr;
    }
}

namespace {

// Whether the shown 3D view has drawn at its canvas's size, asking for a frame when it hasn't: until it
// has, the camera's viewport is the old size, which a fit or a screen_rect would use. True when no 3D
// view is shown. UI thread.
bool view_drawn_at_size()
{
    GLCanvas3D* canvas = shown_canvas();
    if (canvas == nullptr || !canvas->get_wxglcanvas()->IsShownOnScreen())
        return true;
    // What render() gives the viewport: the canvas's size in GL pixels, at least 10.
    const Size                size     = canvas->get_canvas_size();
    const std::array<int, 4>& viewport = ui_plater().get_camera().get_viewport();
    if (viewport[2] == std::max(10, size.get_width()) && viewport[3] == std::max(10, size.get_height()))
        return true;
    canvas->set_as_dirty();
    return false;
}

// The window window_set's target names: the main window, the topmost modal dialog, or the window a ref or
// locator is in. Refuses one no tool may size or move. `locator` gets what the journal records for a
// ref. UI thread.
wxTopLevelWindow& window_target(AutomationRuntime& runtime, const json& target, json& locator)
{
    wxWindow* window = nullptr;
    if (target == "main") {
        window = wxGetApp().mainframe;
        if (window == nullptr)
            throw ToolFailure("The main window is not up yet");
    } else if (target == "dialog") {
        window = runtime.modals().top();
        if (window == nullptr)
            throw ToolFailure(no_modal_dialog(runtime.modals()));
    } else {
        UiTarget t;
        try {
            t = runtime.inspector().resolve(target);
        } catch (const ToolFailure& e) {
            // A window's ref goes stale once the window is destroyed, and window_list gives the open ones'.
            if (!target.is_string())
                throw;
            throw ToolFailure(std::string(e.what()) + ", or window_list for the open windows", e.details);
        }
        locator = runtime.inspector().locator_for(t);
        window  = t.window.get() != nullptr ? wxGetTopLevelParent(t.window.get()) : nullptr;
    }
    if (dynamic_cast<wxPopupWindow*>(window) != nullptr)
        throw ToolFailure("The target is in a popup, which the window it opened from places; give that window (window_list)");
    auto* tlw = dynamic_cast<wxTopLevelWindow*>(window);
    if (tlw == nullptr)
        throw ToolFailure("The target is in no window");
    if (tlw->GetName() == kHiddenFromAutomation)
        throw ToolFailure("The window is the person's own, which no tool reaches");
    if (auto* dlg = dynamic_cast<wxDialog*>(tlw); dlg != nullptr && is_native_dialog(dlg))
        throw ToolFailure("A native dialog is placed by the system; answer it with dialog_answer");
    // A closed dialog may be hidden rather than destroyed, so its ref still resolves.
    if (!tlw->IsShown())
        throw ToolFailure("The window is closed; window_list lists the open windows");
    return *tlw;
}

// `window` while it is still one of the app's top-level windows, else null: window_set keeps a plain
// pointer across its UI closures and checks it before following it. UI thread.
wxTopLevelWindow* live_window(wxTopLevelWindow* window)
{
    for (wxWindow* w : wxTopLevelWindows)
        if (w == window)
            return window;
    return nullptr;
}

// window_set: checks the request and applies it in one UI closure, waits for the window to take it,
// then replies with what the window became. A size or position applies to the normal window, so a
// maximized or minimized one is restored first. Where the window system restores it later (GTK), the call
// waits for that before sizing, as a size given meanwhile is lost: on Wayland the compositor's answer to
// the restore sizes the window again, and wx would size it by the maximized window's decorations. A window
// brought back from the minimized state is finished restoring once it is back (finish_restore(), macOS).
ToolResult set_window(AutomationRuntime& runtime, ToolContext& ctx, const json& args)
{
    const json target = args.value("target", json("main"));
    json       locator;
    // Never followed off the UI thread, and checked by live_window() on it: a wxWeakRef can't be made or
    // dropped on this thread.
    wxTopLevelWindow* window = nullptr;
    bool              main = false, modal = false, restoring = false, unminimizing = false;
    WindowSettling    settling;
    const bool        placing = args.contains("size") || args.contains("position");
    const std::string state   = arg_string(args, "state");
    const auto        minimized = [](const wxTopLevelWindow& w) { return std::string(window_state(w)) == "minimized"; };
    // The request, less the restore that comes before a size or position. UI thread.
    const auto apply = [&](wxTopLevelWindow& w) {
        if (args.contains("size"))
            settling.limited = resize_window(w, wxSize(args["size"][0].get<int>(), args["size"][1].get<int>()));
        // -1 is a place on a screen left of or above the primary one, not "keep this coordinate".
        if (args.contains("position"))
            w.Move(args["position"][0].get<int>(), args["position"][1].get<int>(), wxSIZE_ALLOW_MINUS_ONE);
        if (state == "maximized") {
            maximize_window(w);
        } else if (state == "minimized") {
            minimize_window(w);
        } else if (state == "normal") {
            settling.activation = settling.activation || (restore_needs_activation() && minimized(w));
            unminimizing        = unminimizing || minimized(w);
            restore_window(w);
        }
        if (arg_bool(args, "raise", false))
            raise_window(w);
    };
    ctx.run_ui([&] {
        wxTopLevelWindow& w   = window_target(runtime, target, locator);
        auto*             dlg = dynamic_cast<wxDialog*>(&w);
        main                  = &w == wxGetApp().mainframe;
        modal                 = dlg != nullptr && dlg->IsModal();
        WindowAbilities abilities;
        abilities.main            = main;
        abilities.modal           = modal;
        abilities.can_maximize    = w.HasFlag(wxMAXIMIZE_BOX);
        abilities.can_minimize    = w.HasFlag(wxMINIMIZE_BOX);
        abilities.positions_known = window_positions_known();
        if (const std::string refusal = window_request_refusal(args, abilities); !refusal.empty())
            throw ToolFailure(refusal);
        window = &w;
        if (placing && std::string(window_state(w)) != "normal") {
            settling.activation = restore_needs_activation() && minimized(w);
            unminimizing        = minimized(w);
            restoring           = restore_window(w);
        }
        if (!restoring)
            apply(w);
        return json();
    });
    journal_with_locator(ctx, args, locator);

    // The window manager may apply a request later (X11) or animate it (macOS): a wait ends at two equal
    // readings in a row in the state asked for. `holds` takes a reading and says whether the window held
    // still in `wanted` (any state when empty). UI thread.
    const char* closed = "The request was applied before the window closed";
    json        last;
    const auto  holds = [&](const std::string& wanted) {
        wxTopLevelWindow* w = live_window(window);
        if (w == nullptr)
            throw ToolFailure(closed);
        const wxPoint     pos  = w->GetScreenPosition();
        const wxSize      size = window_size(*w);
        const std::string now  = window_state(*w);
        const json        reading{pos.x, pos.y, size.x, size.y, now};
        return reading == std::exchange(last, reading) && (wanted.empty() || now == wanted);
    };
    const auto cancelled = [] { return ToolFailure("Cancelled: OrcaSlicer is closing or the replay or script was stopped"); };
    if (restoring) {
        // The rest of the request goes to the window the restore left, normal or not.
        poll_until(ctx, [&] { return holds("normal") ? json(true) : json(); }, state_change_timeout("normal"), std::chrono::milliseconds(50));
        if (ctx.cancelled())
            throw cancelled();
        ctx.run_ui([&] {
            wxTopLevelWindow* w = live_window(window);
            if (w == nullptr)
                throw ToolFailure(closed);
            apply(*w);
            return json();
        });
        last = json();
    }

    // For the main window the wait also lasts until its 3D view has drawn at the new size, which a
    // minimized window never does.
    std::string wanted = state;
    if (wanted.empty() && placing)
        wanted = "normal";
    const std::chrono::seconds timeout = state_change_timeout(wanted);
    settling.seconds                   = int(timeout.count());
    const json done                    = poll_until(
        ctx,
        [&]() -> json {
            if (wxTopLevelWindow* w = live_window(window); w != nullptr && unminimizing)
                unminimizing = !finish_restore(*w);
            settling.held = holds(wanted);
            if (!settling.held)
                return json();
            return !main || minimized(*live_window(window)) || view_drawn_at_size() ? json(true) : json();
        },
        timeout, std::chrono::milliseconds(50));
    if (done.is_null() && ctx.cancelled())
        throw cancelled();
    settling.drawn = !done.is_null();
    if (settling.held && !settling.drawn)
        settling.undrawn = undrawn_window(ctx);

    json out = ctx.observe_ui([&] {
        wxTopLevelWindow* w = live_window(window);
        if (w == nullptr)
            throw ToolFailure(closed);
        // window_list's own entry, so the two report a window alike.
        const json entry = runtime.inspector().window_entry(*w, modal);
        json       reply{{"title", entry["title"]}, {"kind", entry["kind"]}, {"ref", entry["ref"]}, {"size", {entry["width"], entry["height"]}}};
        if (entry.contains("x"))
            reply["position"] = {entry["x"], entry["y"]};
        reply["state"]     = entry["state"];
        reply["resizable"] = entry["resizable"];
        reply["scale"]     = w->GetContentScaleFactor();
        // A minimized window may be on no display; the primary one stands for it then.
        int index = wxDisplay::GetFromWindow(w);
        for (unsigned i = 0; index == wxNOT_FOUND && i < wxDisplay::GetCount(); ++i)
            if (wxDisplay(i).IsPrimary())
                index = int(i);
        reply["display"] = display_area(unsigned(std::max(index, 0)));
        if (arg_bool(args, "raise", false))
            reply["active"] = w->IsActive();
        return reply;
    });
    add_window_notes(args, settling, out);
    return ToolResult::ok(out);
}

} // namespace

void add_window_notes(const json& args, const WindowSettling& settling, json& reply)
{
    json adjusted = json::array();
    for (const std::string field : {"size", "position", "state"})
        if (args.contains(field) && reply.contains(field) && reply[field] != args[field])
            adjusted.push_back(field);
    std::string note = settling.limited ? "size is held to the window's minimum or maximum size" : "";
    const auto  add  = [&note](const std::string& why) { note += (note.empty() ? "" : "; ") + why; };
    if (!settling.held && settling.activation)
        add("the window system did not bring the window back: Wayland, and GNOME Shell on X11, let an app restore its "
            "minimized window only by activating it, which they allow only while the app may take the focus, so the "
            "person restores it from the taskbar or the dock");
    else if (!settling.held)
        add("the window manager did not apply the request within " + std::to_string(settling.seconds) + " s");
    else if (!settling.drawn)
        add("the 3D view did not draw at the window's size within " + std::to_string(settling.seconds) + " s" +
            (settling.undrawn.empty() ? "" : ": " + settling.undrawn));
    else if (note.empty() && !adjusted.empty())
        note = "the window manager adjusted the request";
    if (!adjusted.empty())
        reply["adjusted"] = std::move(adjusted);
    if (!note.empty())
        reply["note"] = note;
}

std::string window_request_refusal(const json& args, const WindowAbilities& window)
{
    const bool size = args.contains("size"), position = args.contains("position");
    if (!size && !position && !args.contains("state") && !args.contains("raise"))
        return "Give at least one of size, position, state or raise; window_list reads a window's geometry without changing it";
    // Two whole numbers that fit wx's int coordinates, both positive for a size. Whole floats pass, as
    // they pass the schema's integer type.
    const auto is_pair = [](const json& value, bool positive) {
        const auto whole = [positive](const json& n) {
            if (!n.is_number())
                return false;
            const double d = n.get<double>();
            return d == std::floor(d) && d >= (positive ? 1.0 : double(std::numeric_limits<int>::min())) &&
                   d <= double(std::numeric_limits<int>::max());
        };
        return value.is_array() && value.size() == 2 && std::all_of(value.begin(), value.end(), whole);
    };
    if (size && !is_pair(args.at("size"), true))
        return "Give size as [width, height], two positive integers";
    if (position && !is_pair(args.at("position"), false))
        return "Give position as [x, y], two integers";
    const json state = args.value("state", json());
    if ((state == "maximized" || state == "minimized") && (size || position))
        return "A " + state.get<std::string>() +
               " window's size and position are set by the system: give state alone, or size and position without state";
    if (state == "maximized" && !window.main && !window.can_maximize)
        return "This window's title bar has no maximize button: give it a size instead";
    if (state == "minimized" && window.modal)
        return "A modal dialog never minimizes, as the app would stay blocked with nothing showing: close the dialog instead";
    if (state == "minimized" && !window.main && !window.can_minimize)
        return "This window's title bar has no minimize button: move it aside with position, or close it, instead";
    if (position && !window.positions_known)
        return "position needs X11: Wayland gives an app no way to place its window or to read where it is. Leave position "
               "out, or start OrcaSlicer with GDK_BACKEND=x11 to run it under XWayland, where every field works";
    return {};
}

json display_area(unsigned index)
{
    const wxRect area = wxDisplay(index).GetClientArea();
    return {{"index", index}, {"x", area.x}, {"y", area.y}, {"width", area.width}, {"height", area.height}};
}

void register_view_tools(CommandCatalog& catalog)
{
    using NativeCommands::action;

    // The Agent Activity pane's menu item, which has no argument form.
    const std::string activity = "Show or hide the Agent Activity pane, where a person watches what agents, Action Replay, Run Script, "
                                 "the command line's run and plugins do in the app, call by call. It is the person's: it lists your "
                                 "calls, and you never need it.";
    NativeCommand toggle_activity = menu_command("view_toggle_activity_panel", L("Agent Activity"), activity, object_schema({}),
                                                 json::array({action("toggle_agent_activity_panel", L("Agent Activity"), L("View"), "",
                                                                     json::object(), activity)}),
                                                 [](const json&) { return MenuItems::toggle_agent_activity_panel(); });
    // A menu item with no argument form: tool_list lists it and tool_call runs it, the tool list does not show it.
    toggle_activity.listed = false;
    catalog.add(std::move(toggle_activity));

    // The legend combo's view types; the first-layer inspection type has no name and no combo entry.
    std::vector<std::string> view_types;
    for (int i = 0; i < int(libvgcode::EViewType::COUNT); ++i)
        if (std::string name = GCodeViewer::view_type_to_config_name(libvgcode::EViewType(i)); !name.empty())
            view_types.push_back(std::move(name));

    // The words of tab, as the description lists them.
    std::string tab_words;
    for (const TabWord& w : kTabWords)
        tab_words += (tab_words.empty() ? "" : ", ") + std::string(w.word);

    NativeCommand view = reaching({"show_layer_range"}, mutate_tool(
        "view_set", L("Set up the view"),
        "Set up what the window shows, for screenshots and GUI work: the main tab, the 3D view and its camera, the "
        "Preview's color scheme, legend, layers and time estimate, and the display switches of the View menu and the "
        "sidebar. Only the given fields change; a Preview field given without tab or canvas shows the Preview tab. The "
        "camera takes camera, then `look_at`, `orbit` and `zoom`. On the Design tab it turns the Design view (default is "
        "its Home view; no `fit_bed`), and section, `origin_planes`, `world_axes`, origin and bed set that view's "
        "switches. Reply: each field as the app reads it back, under the names of `app_state`'s view; Preview layers as "
        "`[first, last, count]` with z, the heights of first and last; with tab design, design: the section view and the "
        "four switches; with a camera field, `camera_state` as `app_state.camera` gives it, and adjusted: [\"zoom\"] when "
        "the zoom was held to its limits. A Speed Dial view action runs the same code, so its camera actions turn the "
        "Design tab's camera too.",
        object_schema({{"tab", prop("string", "Main window tab: " + tab_words + ", or another page's id (`monitor_web`, the "
                                              "web Device page where the app shows one, or a plugin's page).")},
                       {"canvas", enum_prop({"3d", "preview", "assemble"}, "3D view: Prepare's, the preview, or the assemble view "
                                                                           "(own gizmos; needs an object; leave it with 3d, as its "
                                                                           "Return button does).")},
                       {"camera", enum_prop({"iso", "top", "bottom", "front", "rear", "left", "right", "default", "fit_bed", "fit_objects", "fit_selection",
                                             "sketch_normal"},
                                            "Direction or fit; `sketch_normal` (Design tab) looks along the open sketch's normal.")},
                       {"section", {{"type", "object"},
                                    {"properties", {{"on", {{"type", "boolean"}, {"description", "Cut, or show it whole."}}},
                                                    {"z", {{"type", "number"}, {"description", "Cut height in mm, Design view coordinates; "
                                                                                               "default the model's middle."}}},
                                                    {"keep", enum_prop({"lower", "upper"}, "The half that stays (default lower).")}}},
                                    {"required", {"on"}},
                                    {"additionalProperties", false},
                                    {"description", "A cut that hides part of the model to show inside. With tab design, the Design "
                                                    "view's horizontal cut (its toolbar's Section View and Flip); otherwise the "
                                                    "section view of Prepare and the Preview, which takes on alone (a right click on "
                                                    "its toolbar button)."}}},
                       {"origin_planes", {{"type", "boolean"}, {"description", "Design tab: the origin helper planes (key P)."}}},
                       {"world_axes", {{"type", "boolean"}, {"description", "Design tab: the world axis triad (key A)."}}},
                       {"origin", {{"type", "boolean"}, {"description", "Design tab: the Origin row's eye, the XY, XZ and YZ planes a "
                                                                        "click can pick; off lets go of a picked plane."}}},
                       {"bed", {{"type", "boolean"}, {"description", "Design tab: the Bed row's eye."}}},
                       {"look_at", array_prop({{"type", "number"}}, "`[x, y, z]` mm to look at, in world coordinates as `object_list` "
                                                                   "gives positions; on the Design tab, those of the Design commands' "
                                                                   "world points (`design_describe`'s `modeling_origin` is the sketch "
                                                                   "planes' origin). The camera moves parallel, keeping direction and "
                                                                   "distance.")},
                       {"orbit", array_prop({{"type", "number"}}, "`[yaw, pitch]` degrees around the point looked at: yaw to the right, "
                                                                 "pitch up (negative from below). Unlike a mouse drag, the same on "
                                                                 "every install.")},
                       {"zoom", {{"type", "number"}, {"exclusiveMinimum", 0},
                                 {"description", "Screenshot pixels per mm at `look_at`'s depth (everywhere, if orthographic); at most "
                                                 "250, at least what the scene's size allows."}}},
                       {"layer", {{"type", "integer"}, {"minimum", 1}, {"description", "Preview: show layers up to this one (1-based)."}}},
                       {"layer_range", array_prop({{"type", "integer"}}, "Preview: `[first, last]` layers, 1-based.")},
                       {"layer_z", {{"type", "number"}, {"minimum", 0}, {"description", "Preview: show layers up to the first whose top "
                                                                                       "reaches this height (mm), as the slider places a "
                                                                                       "custom G-code."}}},
                       {"view_type", enum_prop(view_types, "Preview color scheme, as the legend's combo picks it, which shows every "
                                                           "feature type again.")},
                       {"legend", {{"type", "object"}, {"additionalProperties", {{"type", "boolean"}}},
                                   {"description", "Preview: show (true) or hide (false) legend rows: feature types as `plate_slice_stats` "
                                                   "names them (\"Sparse infill\"), travel, wipe, retract, unretract, seams, "
                                                   "`filament_changes`, `color_changes`, pauses, `custom_gcode`; any case, _ or a space "
                                                   "between words. A row this print lacks is ignored. After `view_type`."}}},
                       {"time_mode", enum_prop({"normal", "silent"}, "Preview: the time estimate shown; silent only when the print has its own.")},
                       {"layer_percent", {{"type", "number"}, {"minimum", 0}, {"maximum", 100},
                                          {"description", "Preview: show layers up to this share of the range, as Go to layer does, "
                                                          "keeping the bottom."}}},
                       {"projection", enum_prop({"perspective", "orthographic", "toggle"}, "The 3D camera's projection.")},
                       {"layout", enum_prop({"default"}, "Restore the default window layout (View > Reset Window Layout).")},
                       {"sidebar_collapsed", prop("boolean", "Collapse (true) or expand (false) the sidebar of Prepare and the Preview.")},
                       {"sidebar_width", {{"type", {"integer", "string"}},
                                          {"minimum", 1},
                                          {"description", "The sidebar's width in pixels, or \"default\". A sidebar floating in its "
                                                          "own window is sized with `window_set`."}}},
                       {"legend_collapsed", prop("boolean", "Preview: fold the legend to its title row (true) or unfold it (false).")},
                       {"gcode_window", prop("boolean", "Preview: the G-code window (View > Show G-code Window).")},
                       {"labels", prop("boolean", "Prepare: object labels (View > Show Labels).")},
                       {"overhangs", prop("boolean", "Prepare: the overhang highlight (View > Show Overhang).")},
                       {"outline", prop("boolean", "The selected objects' outline (View > Show Selected Outline).")},
                       {"wireframe", prop("boolean", "The objects' wireframe (the 3D view's own menu).")},
                       {"gridlines", prop("boolean", "The plate's gridlines (View > Show Gridlines).")},
                       {"axes", prop("boolean", "The axes on the bed of Prepare and the Preview (the 3D view's own menu); the Design "
                                                "view's are `world_axes`.")},
                       {"navigator", prop("boolean", "The 3D navigator of Prepare and the Preview (View > Show 3D Navigator).")}}),
        [](const json& args, ToolContext& ctx) {
            const bool preview_args = args.contains("layer") || args.contains("layer_range") || args.contains("layer_z") ||
                                      args.contains("layer_percent") || args.contains("view_type") || args.contains("legend") ||
                                      args.contains("time_mode") || args.contains("legend_collapsed") || args.contains("gcode_window");
            const bool design_tab = arg_string(args, "tab") == "design";
            if (design_tab && (args.contains("canvas") || preview_args))
                throw ToolFailure("canvas and the Preview fields show Prepare or Preview; give them without tab design");
            // The section view is the Design view's with tab design, else Prepare's and the Preview's.
            const bool design_args = (args.contains("section") && design_tab) || args.contains("origin_planes") ||
                                     args.contains("world_axes") || args.contains("origin") || args.contains("bed") ||
                                     arg_string(args, "camera") == "sketch_normal";
            if (design_args && !design_tab)
                throw ToolFailure("sketch_normal, origin_planes, world_axes, origin and bed are the Design tab's: give them with "
                                  "tab design");
            const bool view_section = args.contains("section") && !design_tab;
            if (view_section && (args["section"].contains("z") || args["section"].contains("keep")))
                throw ToolFailure("z and keep are the Design view's cut: give them with tab design. Without it, section is the "
                                  "section view of Prepare and the Preview, which takes on alone");
            if (args.contains("sidebar_width") && args["sidebar_width"].is_string() && args["sidebar_width"] != "default")
                throw ToolFailure("sidebar_width is a width in pixels or \"default\"");
            const bool sidebar_args = args.contains("sidebar_collapsed") || args.contains("sidebar_width");
            if (args.contains("orbit") && args["orbit"].size() != 2)
                throw ToolFailure("orbit is [yaw, pitch] in degrees");
            if (args.contains("look_at") && args["look_at"].size() != 3)
                throw ToolFailure("look_at is [x, y, z] in mm");
            const bool placed      = args.contains("look_at") || args.contains("orbit") || args.contains("zoom");
            const bool camera_args = placed || args.contains("camera");
            json out = ctx.run_ui([&]() -> json {
                Plater&    plater = ui_plater();
                MainFrame* frame  = wxGetApp().mainframe;
                json       done   = json::object();
                // The sidebar's state, refused before anything changes where it takes none.
                if (sidebar_args && !plater.is_sidebar_enabled())
                    throw ToolFailure("The sidebar is off while a G-code file is shown without its settings");
                if (args.contains("sidebar_width") && plater.is_sidebar_floating())
                    throw ToolFailure("The sidebar floats in a window of its own, whose size window_set sets");
#ifdef SLIC3R_CAD
                // The Design tab's panel, which its options set and the reply reads. They, and the cameras the
                // Design view refuses (point_camera()), are refused here, before the tab or anything else
                // changes; a rebuild refuses the options, its worker holding the document the reference planes
                // are read from.
                // With no Design panel to show or set, the refusal is design_gate()'s (Mcp::no_design_tab()).
                DesignPanel* design = nullptr;
                if (arg_string(args, "tab") == "design" && frame->m_design_page != nullptr)
                    design = DesignPanel::ensure();
                if (design != nullptr && design->mcp_viewport() == nullptr)
                    throw Mcp::no_design_view();
                if (design_args) {
                    if (design == nullptr)
                        throw Mcp::no_design_tab();
                    std::string why;
                    if (design->mcp_busy(true, false, why))
                        throw Mcp::design_busy(why);
                    if (arg_string(args, "camera") == "sketch_normal" && !design->mcp_viewport()->is_sketching())
                        throw ToolFailure("No sketch is open in the Design tab", {{"code", "precondition"}});
                }
                // The Design view the camera points once the tab is handled: the one asked for, or the one shown, which
                // a call with no tab, canvas or Preview field leaves shown. Its refusals (point_camera()) come here too.
                DesignPanel* aimed = design;
                if (aimed == nullptr && !args.contains("tab") && !args.contains("canvas") && !preview_args)
                    aimed = frame->shown_design_panel();
                if (aimed != nullptr && aimed->mcp_viewport() == nullptr)
                    throw Mcp::no_design_view();
                if (aimed != nullptr && arg_string(args, "camera") == "fit_bed")
                    throw refused(no_bed_in_design());
                if (aimed != nullptr && arg_string(args, "camera") == "fit_selection" && !aimed->mcp_viewport()->selection_box().defined)
                    throw ToolFailure("Nothing is selected in the Design tab", {{"code", "precondition"}});
#endif
                if (args.contains("tab")) {
                    const std::string tab = arg_string(args, "tab");
#ifdef SLIC3R_CAD
                    // A page whose panel cannot be built is refused too: showing it activates the panel.
                    if (tab == "design" && design == nullptr)
                        throw Mcp::no_design_tab();
#endif
                    if (const std::optional<Ops::Refusal> refusal = show_tab(*frame, tab))
                        throw refused(*refusal);
                    done["tab"] = tab;
                }
#ifdef SLIC3R_CAD
                if (design != nullptr) {
                    DesignCanvas* view = design->mcp_viewport();
                    if (args.contains("section")) {
                        const json& section = args["section"];
                        design->set_section_view(section.at("on").get<bool>(),
                                                 section.contains("z") ? std::optional<double>(section["z"].get<double>()) : std::nullopt,
                                                 section.value("keep", "lower") == "upper");
                    }
                    if (args.contains("origin_planes"))
                        view->set_show_planes(args["origin_planes"].get<bool>());
                    if (args.contains("world_axes"))
                        view->set_show_axes(args["world_axes"].get<bool>());
                    if (args.contains("origin"))
                        design->set_origin_shown(args["origin"].get<bool>());
                    if (args.contains("bed"))
                        design->set_bed_shown(args["bed"].get<bool>());
                    // With no cut, no height or half to report.
                    const DesignPanel::SectionView cut = design->section_view();
                    done["design"] = {{"section", {{"on", cut.on},
                                                   {"z", cut.on ? json(cut.z) : json()},
                                                   {"keep", cut.on ? json(cut.keep_upper ? "upper" : "lower") : json()}}},
                                      {"origin_planes", view->show_planes()},
                                      {"world_axes", view->show_axes()},
                                      {"origin", design->origin_shown()},
                                      {"bed", design->bed_shown()}};
                }
#endif
                if (args.contains("canvas")) {
                    const std::string view     = arg_string(args, "canvas");
                    GLCanvas3D*       assemble = plater.get_assmeble_canvas3D();
                    const bool        in_assemble = assemble != nullptr && plater.get_current_canvas3D() == assemble;
                    if (view == "assemble") {
                        if (!plater.has_assmeble_view())
                            throw ToolFailure("The assemble view needs an object on the plate");
                        frame->select_tab(wxString(TAB_ID_PREPARE));
                        plater.select_view_3D("Assemble");
                    } else if (in_assemble) {
                        // What the assemble view's Return button does.
                        assemble->get_gizmos_manager().reset_all_states();
                        plater.select_view_3D("3D");
                        if (GLCanvas3D* view3d = plater.get_view3D_canvas3D()) {
                            view3d->get_gizmos_manager().reset_all_states();
                            view3d->reload_scene(true);
                        }
                    }
                    if (view == "3d")
                        frame->select_tab(wxString(TAB_ID_PREPARE));
                    else if (view == "preview")
                        frame->select_tab(wxString(TAB_ID_PREVIEW));
                    done["canvas"] = view;
                }
                if (preview_args && !args.contains("tab") && !args.contains("canvas"))
                    show_preview(plater, *frame);
                if (preview_args) {
                    GLCanvas3D* canvas = plater.get_preview_canvas3D();
                    if (canvas == nullptr)
                        throw ToolFailure("The preview is not available");
                    GCodeViewer& viewer = canvas->get_gcode_viewer();
                    if (args.contains("layer_z") && (args.contains("layer") || args.contains("layer_range")))
                        throw ToolFailure("Give layer_z or layer/layer_range, not both");
                    if (args.contains("layer_percent") && (args.contains("layer") || args.contains("layer_range") || args.contains("layer_z")))
                        throw ToolFailure("Give layer_percent alone, without layer, layer_range or layer_z");
                    if ((args.contains("legend") || args.contains("time_mode")) && !viewer.has_data())
                        throw ToolFailure("legend and time_mode need the Preview's toolpaths; slice first");
                    const std::function<json()> show_legend = args.contains("legend") ? legend_change(viewer, args["legend"]) : nullptr;
                    const bool silent = arg_string(args, "time_mode") == "silent";
                    if (silent && !viewer.can_switch_time_mode())
                        throw ToolFailure("This print has no separate silent-mode estimate");
                    // In this order, as a person would: picking Line Type shows every feature type again.
                    if (args.contains("view_type")) {
                        libvgcode::EViewType type;
                        if (!GCodeViewer::view_type_from_config_name(arg_string(args, "view_type"), type))
                            throw ToolFailure("Unknown view_type");
                        // Without toolpaths there is no legend to pick from; the type then applies to the next ones.
                        if (viewer.has_data())
                            viewer.pick_view_type(type);
                        else
                            viewer.select_view_type(type);
                        done["view_type"] = arg_string(args, "view_type");
                    }
                    if (show_legend)
                        done["legend"] = show_legend();
                    if (args.contains("time_mode")) {
                        const libvgcode::ETimeMode mode = silent ? libvgcode::ETimeMode::Stealth : libvgcode::ETimeMode::Normal;
                        if (viewer.get_viewer().get_time_mode() != mode)
                            viewer.set_time_mode(mode);
                        done["time_mode"] = arg_string(args, "time_mode");
                    }
                    if (args.contains("layer") || args.contains("layer_range") || args.contains("layer_z") || args.contains("layer_percent")) {
                        const Ops::Outcome<int> count = Ops::layer_count();
                        if (!count)
                            throw refused(count.refusal());
                        IMSlider* layers = viewer.get_layers_slider();
                        const int max    = count.value() - 1;
                        // Every layer up to the one asked for, unless a range says where to start. The
                        // operation brings the numbers into the plate's layers.
                        Ops::ShowLayerRangeArgs range;
                        range.first = 1;
                        range.last  = max + 1;
                        if (args.contains("layer"))
                            range.last = args["layer"].get<int>();
                        if (args.contains("layer_range")) {
                            const json& r = args["layer_range"];
                            if (!r.is_array() || r.size() != 2)
                                throw ToolFailure("layer_range must be [first, last]");
                            range.first = r[0].get<int>();
                            range.last  = r[1].get<int>();
                        }
                        if (args.contains("layer_z")) {
                            // The slider's own rule for a height, which plate_set_custom_gcode's z follows too.
                            const double z    = args["layer_z"].get<double>();
                            const int    tick = layers->get_tick_from_value(z, true);
                            if (tick < 0 || tick > max)
                                throw ToolFailure("layer_z " + json(z).dump() + " is above the top layer (" +
                                                  json(rounded(layers->GetMaxValueD(), 1000.)).dump() + " mm)");
                            range.last = tick + 1;
                        }
                        const Ops::Outcome<Ops::ShownLayers> shown = args.contains("layer_percent") ?
                                                                         show_layer_percent(args["layer_percent"].get<double>()) :
                                                                         Ops::show_layer_range(range);
                        if (!shown)
                            throw refused(shown.refusal());
                        // The reply echoes the range asked for; z is of the layers the slider shows.
                        done["layers"] = {shown.value().asked_first, shown.value().asked_last, shown.value().count};
                        done["z"]      = {rounded(shown.value().z_first, 1000.), rounded(shown.value().z_last, 1000.)};
                    }
                    // After the layers, which can still refuse the call, and after view_type, whose pick unfolds the
                    // legend.
                    if (args.contains("legend_collapsed")) {
                        viewer.fold_legend(args["legend_collapsed"].get<bool>());
                        done["legend_collapsed"] = viewer.is_legend_folded();
                    }
                    if (args.contains("gcode_window")) {
                        if (wxGetApp().show_gcode_window() != args["gcode_window"].get<bool>())
                            wxGetApp().toggle_show_gcode_window();
                        done["gcode_window"] = wxGetApp().show_gcode_window();
                    }
                    canvas->set_as_dirty();
                }
                if (args.contains("projection")) {
                    Camera&           camera     = plater.get_camera();
                    const std::string projection = arg_string(args, "projection");
                    if (projection == "toggle")
                        camera.select_next_type();
                    else
                        camera.set_type(projection == "perspective" ? Camera::EType::Perspective : Camera::EType::Ortho);
                    done["projection"] = camera.get_type_as_string();
                }
                if (args.contains("layout")) {
                    plater.reset_window_layout();
                    done["layout"] = arg_string(args, "layout");
                }
                if (view_section) {
                    show_section_view(plater, args["section"].at("on").get<bool>());
                    done["section"] = {{"on", section_view_on(plater)}};
                }
                for (const ViewSwitch& view_switch : kViewSwitches)
                    if (args.contains(view_switch.name)) {
                        view_switch.show(plater, args[view_switch.name].get<bool>());
                        done[view_switch.name] = view_switch.shown(plater);
                    }
                // After layout, which expands the sidebar at its default width.
                if (args.contains("sidebar_collapsed") && plater.is_sidebar_collapsed() != args["sidebar_collapsed"].get<bool>())
                    plater.collapse_sidebar(args["sidebar_collapsed"].get<bool>());
                if (args.contains("sidebar_width"))
                    plater.set_sidebar_width(args["sidebar_width"].is_string() ? plater.default_sidebar_width() :
                                                                                  args["sidebar_width"].get<int>());
                if (GLCanvas3D* canvas = plater.get_current_canvas3D())
                    canvas->set_as_dirty();
                repaint_shown_canvas();
                return done;
            });
            if (camera_args) {
                // A tab's page shows, and the Design tab takes the shared camera, once the tab change is
                // handled; the camera then points the view on screen.
                settle(ctx);
                // A fit the view runs at its next frame would replace look_at, orbit and zoom, so they
                // wait for it. A camera also waits until the view has drawn at its canvas's size: a fit, as
                // default and the Design directions include, uses the viewport only a frame sets. The view
                // is asked to draw meanwhile.
                const bool aimed      = args.contains("camera");
                bool       fit_queued = false;
                const json ready      = poll_until(
                    ctx,
                    [&] {
                        fit_queued = placed && fit_pending(ui_plater().get_camera());
                        if (!fit_queued && (!aimed || view_drawn_at_size()))
                            return json(true);
                        repaint_shown_canvas();
                        return json();
                    },
                    std::chrono::seconds(2), std::chrono::milliseconds(20));
                if (ready.is_null()) {
                    if (ctx.cancelled())
                        throw ToolFailure("Cancelled: OrcaSlicer is closing or the replay or script was stopped");
                    out["note"] = fit_queued ? "The 3D view did not draw within 2 s; the fit it has queued replaces look_at, orbit and zoom when it draws"
                                             : "The 3D view did not draw at its size within 2 s; a fit used the size it last drew at";
                }
                // In this order: a fit sets the point looked at and the zoom, which the given ones then replace.
                out.update(ctx.run_ui([&]() -> json {
                    Plater& plater = ui_plater();
                    Camera& camera = plater.get_camera();
                    json    done   = json::object();
                    if (args.contains("camera")) {
                        if (const std::optional<Ops::Refusal> refusal = point_camera(plater, arg_string(args, "camera")))
                            throw refused(*refusal);
                        done["camera"] = arg_string(args, "camera");
                    }
                    if (args.contains("look_at")) {
                        const json& t = args["look_at"];
                        camera.set_target(Vec3d(t[0].get<double>(), t[1].get<double>(), t[2].get<double>()));
                    }
                    if (args.contains("orbit"))
                        done["orbit"] = orbit_camera(camera, args["orbit"][0].get<double>(), args["orbit"][1].get<double>());
                    if (args.contains("zoom"))
                        camera.set_zoom(args["zoom"].get<double>());
                    repaint_shown_canvas();
                    return done;
                }));
            }
            if (out.value("canvas", "") == "assemble")
                // A canvas shown for the first time loads its scene at its first frame, once its GL
                // surface exists (a moment later on Wayland), and the next call may act on its objects.
                poll_ui(
                    ctx,
                    []() -> json {
                        const GLCanvas3D* assemble = ui_plater().get_assmeble_canvas3D();
                        return assemble != nullptr && assemble->is_initialized() && !assemble->is_reload_delayed() ? json(true) : json();
                    },
                    std::chrono::seconds(15), "the assemble view to show its objects");
            settle(ctx);
            // The sidebar as the window lays it out once the change is handled.
            if (sidebar_args)
                out.update(ctx.observe_ui([&args] {
                    Plater& plater = ui_plater();
                    json    read   = json::object();
                    if (args.contains("sidebar_collapsed"))
                        read["sidebar_collapsed"] = plater.is_sidebar_collapsed();
                    if (args.contains("sidebar_width"))
                        read["sidebar_width"] = plater.sidebar_width();
                    return read;
                }));
            if (camera_args)
                out.update(ctx.observe_ui([&args] {
                    const GLCanvas3D* canvas = shown_canvas();
                    Camera&           camera = ui_plater().get_camera();
                    json              read{{"camera_state", camera_state(camera, canvas != nullptr ? canvas->get_scale() : 1.)}};
                    // The camera holds the zoom to its limits.
                    if (args.contains("zoom") && std::abs(camera.get_zoom() - args["zoom"].get<double>()) > 1e-6)
                        read["adjusted"] = json::array({"zoom"});
                    return read;
                }));
            return ToolResult::ok(out);
        }));
    // The View menu's items and the Speed Dial's Go to tab and Go to layer, whose value its second phase collects. Titles
    // are L("...")-marked here so xgettext extracts them.
    struct CameraMenuItem
    {
        const char* key;
        const char* title;
        const char* camera;
    };
    static const CameraMenuItem camera_menu_items[] = {{"view_top", L("View: Top"), "top"},
                                                       {"view_bottom", L("View: Bottom"), "bottom"},
                                                       {"view_front", L("View: Front"), "front"},
                                                       {"view_rear", L("View: Rear"), "rear"},
                                                       {"view_left", L("View: Left"), "left"},
                                                       {"view_right", L("View: Right"), "right"},
                                                       {"view_iso", L("View: Isometric"), "iso"},
                                                       {"view_default", L("View: Default"), "default"},
                                                       {"view_fit_bed", L("Fit Bed to View"), "fit_bed"}};
    view.actions = json::array();
    std::vector<json> camera_forms;
    for (const CameraMenuItem& item : camera_menu_items) {
        const std::string camera = item.camera;
        std::string       does   = "Turn the camera of the view shown, the Design tab's too, to the " + camera + " view.";
        if (camera == "default")
            does = "Turn the camera of the view shown to the default view and fit the plate; on the Design tab, its Home view.";
        else if (camera == "fit_bed")
            does = "Fit the build plate in the 3D view shown; refused on the Design tab, which has no bed to fit.";
        view.actions.push_back(NativeCommands::action(item.key, item.title, L("View"), "", {{"camera", camera}}, does));
        camera_forms.push_back({{"camera", camera}});
    }
    view.actions.push_back(NativeCommands::action("view_toggle_perspective", L("Toggle Perspective"), L("View"), "",
                                                  {{"projection", "toggle"}},
                                                  "Switch the 3D camera between perspective and orthographic."));
    view.actions.push_back(NativeCommands::action("reset_window_layout", L("Reset Window Layout"), L("View"), "toolbar_reset",
                                                  {{"layout", "default"}}, "Restore the default window layout."));
    view.actions.push_back(NativeCommands::action("go_to_tab", L("Go to tab..."), L("Commands"), "", json::object(),
                                                  "Switch to a main window tab.", "tab"));
    view.actions.push_back(NativeCommands::action("go_to_layer", L("Go to layer (percent)"), L("Commands"), "height_range_layer",
                                                  json::object(),
                                                  "Show the Preview tab at a layer chosen as a percentage of the layer range; an error "
                                                  "while the Preview has no layers (slice first).",
                                                  "percent", "layer_percent"));
    // A person's pick of any of these actions runs in place (picked_view()): Toggle Perspective and Reset Window Layout
    // their menu items, the camera actions, Go to tab and Go to layer the camera, tab and layer functions view_set's UI
    // closure runs for a call, so a camera action turns the Design tab's camera too. An automated call of a camera action,
    // Go to tab or Go to layer is view_set's own function.
    catalog.add(NativeCommands::with_menu_item_forms(std::move(view), picked_view, /*person_form_behind_dialogs=*/false,
                                               std::move(camera_forms)));

    // ---- The window commands made from menu items ----
    struct WindowMenuItem
    {
        const char* key;
        const char* title;
        const char* group;
        const char* icon;
        const char* window;
        const char* description;
    };
    // Titles are L("...")-marked here so xgettext extracts them.
    static const WindowMenuItem window_menu_items[] = {
        {"open_preferences", L("Preferences"), L("Commands"), "cog", "preferences",
         "Open the Preferences dialog. window_open with a tab, and a key on it, opens it on that tab with that preference's "
         "control scrolled into view and focused (shown on its own tab)."},
        {"open_plugins", L("Open Plugins"), L("Plugins"), "", "plugins", "Open the plugins dialog."},
        {"open_action_replay", L("Action Replay"), L("Commands"), "loop", "action_replay",
         "Open the Action Replay window, where a person opens a replay file, reads its steps and replays them. Agents run "
         "replays with the replay_run tool instead."},
        {"preset_bundle", L("Open Preset Bundle"), L("Presets"), "menu_edit_preset", "preset_bundle", "Open the preset bundle manager."},
        {"help_keyboard_shortcuts", L("Keyboard Shortcuts"), L("Help"), "", "keyboard_shortcuts", "Show the keyboard shortcuts dialog."},
        {"help_setup_wizard", L("Setup Wizard"), L("Help"), "", "setup_wizard", "Open the setup wizard."},
        {"help_network_test", L("Open Network Test"), L("Help"), "", "network_test", "Open the network test dialog."},
        {"help_troubleshoot", L("Troubleshoot Center"), L("Help"), "", "troubleshoot", "Open the troubleshooting center."},
        {"help_tip_of_the_day", L("Show Tip of the Day"), L("Help"), "info", "tip_of_the_day", "Show a tip of the day."},
        {"help_about", L("About OrcaSlicer"), L("Help"), "OrcaSlicer_gradient_circle", "about", "Show the About dialog."},
    };
    std::vector<std::string> windows;
    json                     window_actions = json::array();
    for (const WindowMenuItem& item : window_menu_items) {
        windows.emplace_back(item.window);
        window_actions.push_back(
            NativeCommands::action(item.key, item.title, item.group, item.icon, {{"window", item.window}}, item.description));
    }
    catalog.add(menu_command(
        "window_open", L("Open a window"),
        "Open one of OrcaSlicer's windows or dialogs for a person, as its menu item does: preferences, the Preferences dialog, "
        "with tab, and key on it, on that tab with that preference's control scrolled into view and focused (its controls are "
        "named after their preference keys: `preferences_get` reads them and `preferences_set` writes those it does not refuse, "
        "and `widget_set_value {\"target\": {\"name\": key, \"within\": \"Preferences\"}}` changes one through the "
        "dialog); plugins, the Plugins window; `action_replay`, the "
        "Action Replay window, where a person replays a replay file (agents run one with `replay_run`); `preset_bundle`, the preset "
        "bundle manager; `keyboard_shortcuts`; `setup_wizard`; `network_test`; troubleshoot, the troubleshooting center; "
        "`tip_of_the_day`; about. A modal one replies `blocked_by_dialog`.",
        object_schema({{"window", enum_prop(windows, "The window.")},
                       {"tab", enum_prop(MenuItems::preferences_tabs(), "preferences: the tab to show.")},
                       {"key", prop("string", "preferences, with tab: a preference on that tab, e.g. `auto_arrange` on behaviour: "
                                              "its control is scrolled into view and focused.")}},
                      {"window"}),
        std::move(window_actions), [](const json& args) {
            const std::string window = arg_string(args, "window");
            if (window != "preferences" && (args.contains("tab") || args.contains("key")))
                return AppActionRunResult{AppActionRunResult::Level::Error, from_u8("tab and key go with window preferences"), false};
            json preferences = args;
            preferences.erase("window");
            return MenuItems::open_window(window, preferences);
        }));

    catalog.add(inspect_tool(
        "window_list", L("List the windows"),
        "List OrcaSlicer's open windows: the main window, then the windows that don't block the app (Plugins, an "
        "object's settings, a dock panel floating on its own), then the modal dialogs in the order they opened, the "
        "topmost last. A window's ref goes to `window_screenshot` as target to capture that window, to `widget_list` as target to "
        "list it alone, and to `keyboard_press` as target; its title is the within of locators for its widgets.\n"
        "Reply: each window's title, kind (main, window, dialog, or panel: a dock panel floating on its own), modal, "
        "ref, web (it shows a web page), x and y on screen (not on Wayland, which doesn't say), width and height, state "
        "(normal, maximized or minimized) and resizable (it has a resize border); `window_set` changes them.\n"
        "A native dialog (a message box, a file picker) has `native: true` and no ref: `dialog_answer` answers it. `hidden: "
        "true` marks the person's own windows (the Run Script panel floating, its prompts and Action Replay's, the Python "
        "scripts setting), which no tool reaches and screenshots leave out.",
        object_schema({{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}, {"description", "Default 100."}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}}}}),
        [](const json& args, ToolContext& ctx) {
            AutomationRuntime& runtime = ctx.runtime();
            return ToolResult::ok(page(ctx.run_ui([&] { return runtime.inspector().windows(); }), args));
        }));

    {
        NativeCommand def = recover_input_tool(
            "window_set", L("Size, move or raise a window"),
            "Size, move, maximize, minimize or raise a window: the main window (default), the topmost modal dialog, or one "
            "`window_list` lists, also while a modal dialog is open. Size the main window before `view_set` fits the camera and "
            "before `recording_start`, as a recording keeps its first frame's size. size and position are in the units "
            "`window_list` reports, whose x/y and width/height are read as position and size too. A size or position restores "
            "a maximized or minimized window first. A window takes only the states its title bar offers; a modal dialog "
            "never minimizes. A refused call changes nothing. Replies once the window holds still and the 3D view has drawn "
            "at its new size (3 s at most): title, kind, ref, size, position, state, resizable, scale (screenshot pixels per "
            "window unit), display (its work area), active with raise, and adjusted with a note: what the OS or a size limit "
            "changed.",
            object_schema({{"target", {{"type", json::array({"string", "object"})},
                                       {"description", "main (default), dialog (the topmost modal "
                                                       "dialog), or a window's ref or locator from `window_list`; a widget's ref or "
                                                       "locator picks the window it is in."}}},
                           {"size", array_prop({{"type", "integer"}}, "`[width, height]`, held to the window's minimum and maximum size.")},
                           {"position", array_prop({{"type", "integer"}}, "`[x, y]` of the window's top-left corner on screen; not on Wayland.")},
                           {"state", enum_prop({"normal", "maximized", "minimized"},
                                               "normal restores a maximized or minimized window; maximized and minimized take no size "
                                               "or position.")},
                           {"raise", prop("boolean", "Bring the window in front of the app's other windows; active in the reply says "
                                                     "whether it became the active one.")}}),
            [](const json& args, ToolContext& ctx) { return set_window(ctx.runtime(), ctx, args); });
        def.allowed_during_operation = true;
        catalog.add(def);
    }
}

}}} // namespace Slic3r::GUI::Mcp
