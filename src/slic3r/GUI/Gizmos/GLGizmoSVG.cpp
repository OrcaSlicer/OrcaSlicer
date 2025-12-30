#include "GLGizmoSVG.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GizmoObjectManipulation.hpp"
#include "slic3r/GUI/MainFrame.hpp" // to update title when add text
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "slic3r/GUI/Jobs/EmbossJob.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/Point.hpp"      
#include "libslic3r/SVG.hpp"      // debug store
#include "libslic3r/Geometry.hpp" // covex hull 2d
#include "libslic3r/Timer.hpp" // covex hull 2d
#include "libslic3r/Emboss.hpp" // heal_shape

#include "libslic3r/NSVGUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ClipperUtils.hpp" // union_ex

#include "imgui/imgui_stdlib.h" // using std::string for inputs
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>
#include "nanosvg/nanosvg.h"    // load SVG file

#include <wx/display.h> // detection of change DPI
#include <boost/log/trivial.hpp>

#include <GL/glew.h>
#include <chrono> // measure enumeration of fonts
#include <sstream> // save for svg
#include <array>
#include <fstream>

using namespace Slic3r;
using namespace Slic3r::Emboss;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::Emboss;

GLGizmoSVG::GLGizmoSVG(GLCanvas3D &parent)
    : GLGizmoBase(parent, M_ICON_FILENAME, -3)
    , m_gui_cfg(nullptr)
    , m_rotate_gizmo(parent, GLGizmoRotate::Axis::Z) // grab id = 2 (Z axis)
{
    m_rotate_gizmo.set_group_id(0);
    m_rotate_gizmo.set_force_local_coordinate(true);
}

// Private functions to create emboss volume
namespace{

// TRN - Title in Undo/Redo stack after rotate with SVG around emboss axe
const std::string rotation_snapshot_name = L("SVG rotate");
// NOTE: Translation is made in "m_parent.do_rotate()"

// TRN - Title in Undo/Redo stack after move with SVG along emboss axe - From surface
const std::string move_snapshot_name = L("SVG move");
// NOTE: Translation is made in "m_parent.do_translate()"

// Variable keep limits for variables
const struct Limits
{
    MinMax<double> depth{0.01, 1e4}; // in mm
    MinMax<float> size{0.01f, 1e4f}; // in mm (width + height)
    MinMax<float> ui_size{5.f, 100.f}; // in mm (width + height) - only slider values
    MinMax<float> ui_size_in{.1f, 4.f};// in inches (width + height) - only slider values
    MinMax<double> relative_scale_ratio{1e-5, 1e4}; // change size
    // distance text object from surface
    MinMax<float> angle{-180.f, 180.f}; // in degrees
} limits;

// Store path to directory with svg for import and export svg's
wxString last_used_directory = wxEmptyString;

/// <summary>
/// Open file dialog with svg files
/// </summary>
/// <returns>File path to svg</returns>
std::string choose_svg_file();

double get_tesselation_tolerance(double scale){ 
    double tesselation_tolerance_in_mm = .1; //8e-2;
    double tesselation_tolerance_scaled = (tesselation_tolerance_in_mm*tesselation_tolerance_in_mm) / SCALING_FACTOR / SCALING_FACTOR;
    return tesselation_tolerance_scaled / scale / scale;
}

/// <summary>
/// Let user to choose file with (S)calable (V)ector (G)raphics - SVG.
/// Than let select contour
/// </summary>
/// <param name="filepath">SVG file path, when empty promt user to select one</param>
/// <returns>EmbossShape to create</returns>
EmbossShape select_shape(std::string_view filepath = "", double tesselation_tolerance_in_mm = get_tesselation_tolerance(1.));

/// <summary>
/// Create new embos data
/// </summary>
/// <param name="cancel">Cancel for previous job</param>
/// <param name="volume_type">To distiquish whether it is outside of model</param>
/// <param name="filepath">SVG file path</param>
/// <returns>Base data for emboss SVG</returns>
DataBasePtr create_emboss_data_base(std::shared_ptr<std::atomic<bool>> &cancel, ModelVolumeType volume_type, std::string_view filepath = "");

/// <summary>
/// Separate file name from file path.
/// String after last delimiter and before last point 
/// </summary>
/// <param name="file_path">path return by file dialog</param>
/// <returns>File name without directory path</returns>
std::string get_file_name(const std::string &file_path);

/// <summary>
/// Create volume name from shape information
/// </summary>
/// <param name="shape">File path</param>
/// <returns>Name for volume</returns>
std::string volume_name(const EmbossShape& shape);

/// <summary>
/// Create input for volume creation
/// </summary>
/// <param name="canvas">parent of gizmo</param>
/// <param name="raycaster">Keep scene</param>
/// <param name="volume_type">Type of volume to be created</param>
/// <returns>Params</returns>
CreateVolumeParams create_input(GLCanvas3D &canvas, RaycastManager &raycaster, ModelVolumeType volume_type);

enum class IconType : unsigned {
    reset_value,
    refresh,
    change_file,
    bake,
    save,
    exclamation,
    lock,
    unlock,
    reflection_x,
    reflection_y,
    // automatic calc of icon's count
    _count
};
// Do not forgot add loading of file in funtion:
// IconManager::Icons init_icons(

// Define rendered version of icon
enum class IconState : unsigned { activable = 0, hovered /*1*/, disabled /*2*/ };
// selector for icon by enum
const IconManager::Icon &get_icon(const IconManager::VIcons &icons, IconType type, IconState state) { 
    return *icons[(unsigned) type][(unsigned) state]; }

// This configs holds GUI layout size given by translated texts.
// etc. When language changes, GUI is recreated and this class constructed again,
// so the change takes effect. (info by GLGizmoFdmSupports.hpp)
struct GuiCfg
{
    // Detect invalid config values when change monitor DPI
    double screen_scale = -1.;
    bool   dark_mode = false;

    // Define bigger size(width or height)
    unsigned texture_max_size_px = 256;

    float input_width  = 0.f;
    float input_offset = 0.f;

    float icon_width   = 0.f;
    
    float max_tooltip_width = 0.f;

    // offset for checbox for lock up vector
    float lock_offset = 0.f;
    // Only translations needed for calc GUI size
    struct Translations
    {
        std::string depth;
        std::string size;
        std::string use_surface;
        std::string rotation;
        std::string distance; // from surface
        std::string mirror;
    };
    Translations translations;
};
GuiCfg create_gui_configuration();

} // namespace 

// use private definition
struct GLGizmoSVG::GuiCfg: public ::GuiCfg{};

bool GLGizmoSVG::create_volume(ModelVolumeType volume_type, const Vec2d &mouse_pos)
{
    CreateVolumeParams input = create_input(m_parent, m_raycast_manager, volume_type);
    DataBasePtr base = create_emboss_data_base(m_job_cancel, volume_type);
    if (!base) return false; // Uninterpretable svg
    return start_create_volume(input, std::move(base), mouse_pos);
}

bool GLGizmoSVG::create_volume(ModelVolumeType volume_type) 
{
    CreateVolumeParams input = create_input(m_parent, m_raycast_manager, volume_type);
    DataBasePtr base = create_emboss_data_base(m_job_cancel,volume_type);
    if (!base) return false; // Uninterpretable svg
    return start_create_volume_without_position(input, std::move(base));
}

bool GLGizmoSVG::create_volume(std::string_view svg_file, ModelVolumeType volume_type){
    CreateVolumeParams input = create_input(m_parent, m_raycast_manager, volume_type);
    DataBasePtr base = create_emboss_data_base(m_job_cancel, volume_type, svg_file);
    if (!base) return false; // Uninterpretable svg
    return start_create_volume_without_position(input, std::move(base));
}

bool GLGizmoSVG::create_volume(std::string_view svg_file, const Vec2d &mouse_pos, ModelVolumeType volume_type)
{
    CreateVolumeParams input = create_input(m_parent, m_raycast_manager, volume_type);
    DataBasePtr base = create_emboss_data_base(m_job_cancel, volume_type, svg_file);
    if (!base) return false; // Uninterpretable svg
    return start_create_volume(input, std::move(base), mouse_pos);
}

bool GLGizmoSVG::is_svg(const ModelVolume &volume) {
    return volume.emboss_shape.has_value() && volume.emboss_shape->svg_file.has_value();
}

bool GLGizmoSVG::is_svg_object(const ModelVolume &volume) {
    if (!volume.emboss_shape.has_value()) return false;
    if (volume.type() != ModelVolumeType::MODEL_PART) return false;
    for (const ModelVolume *v : volume.get_object()->volumes) {
        if (v->id() == volume.id()) continue;
        if (v->type() == ModelVolumeType::MODEL_PART) return false;
    }
    return true;
}

bool GLGizmoSVG::on_mouse_for_rotation(const wxMouseEvent &mouse_event)
{
    if (mouse_event.Moving()) return false;

    bool used = use_grabbers(mouse_event);
    if (!m_dragging) return used;

    if (mouse_event.Dragging())
        dragging_rotate_gizmo(m_rotate_gizmo.get_angle(), m_angle, m_rotate_start_angle, m_parent.get_selection());
    
    return used;
}

bool GLGizmoSVG::on_mouse_for_translate(const wxMouseEvent &mouse_event)
{
    // exist selected volume?
    if (m_volume == nullptr)
        return false;

    auto up_limit = m_keep_up ? std::optional<double>(UP_LIMIT) : std::optional<double>{};
    const Camera &camera = wxGetApp().plater()->get_camera();

    bool was_dragging = m_surface_drag.has_value();
    bool res = on_mouse_surface_drag(mouse_event, camera, m_surface_drag, m_parent, m_raycast_manager, up_limit);
    bool is_dragging  = m_surface_drag.has_value();

    // End with surface dragging?
    if (was_dragging && !is_dragging) {
        // Update surface by new position
        if (m_volume->emboss_shape->projection.use_surface)
            process();

        // TODO: Remove it when it will be stable
        // Distance should not change during dragging
        const GLVolume *gl_volume = m_parent.get_selection().get_first_volume();
        m_distance = calc_distance(*gl_volume, m_raycast_manager, m_parent);

        // Show correct value of height & depth inside of inputs
        calculate_scale();
    }

    // Start with dragging
    else if (!was_dragging && is_dragging) {
        // Cancel job to prevent interuption of dragging (duplicit result)
        if (m_job_cancel != nullptr)
            m_job_cancel->store(true);
    }

    // during drag
    else if (was_dragging && is_dragging) {
        // update scale of selected volume --> should be approx the same
        calculate_scale();

        // Recalculate angle for GUI
        if (!m_keep_up)
            m_angle = calc_angle(m_parent.get_selection());
    }
    return res;
}

void GLGizmoSVG::volume_transformation_changed()
{
    if (m_volume == nullptr || 
        !m_volume->emboss_shape.has_value()) {
        assert(false);
        return;
    }

    if (!m_keep_up) {
        // update current style
        m_angle = calc_angle(m_parent.get_selection());
    } else {
        // angle should be the same
        assert(is_approx(m_angle, calc_angle(m_parent.get_selection())));
    }

    // Update surface by new position
    if (m_volume->emboss_shape->projection.use_surface) {
        process();
    } else {
        // inform slicing process that model changed
        // SLA supports, processing
        // ensure on bed
        wxGetApp().plater()->changed_object(*m_volume->get_object());
    }

    // Show correct value of height & depth inside of inputs
    calculate_scale();
}

bool GLGizmoSVG::on_mouse(const wxMouseEvent &mouse_event)
{
    // not selected volume
    if (m_volume == nullptr ||
        get_model_volume(m_volume_id, m_parent.get_selection().get_model()->objects) == nullptr ||
        !m_volume->emboss_shape.has_value()) return false;

    if (on_mouse_for_rotation(mouse_event)) return true;
    if (on_mouse_for_translate(mouse_event)) return true;

    return false;
}

bool GLGizmoSVG::wants_enter_leave_snapshots() const { return true; }
std::string GLGizmoSVG::get_gizmo_entering_text() const { return _u8L("Enter SVG gizmo"); }
std::string GLGizmoSVG::get_gizmo_leaving_text() const { return _u8L("Leave SVG gizmo"); }
std::string GLGizmoSVG::get_action_snapshot_name() const { return _u8L("SVG actions"); }

bool GLGizmoSVG::on_init()
{
    m_rotate_gizmo.init();
    ColorRGBA gray_color(.6f, .6f, .6f, .3f);
    m_rotate_gizmo.set_highlight_color(gray_color);
    // Set rotation gizmo upwardrotate
    m_rotate_gizmo.set_angle(PI / 2);
    return true;
}

std::string GLGizmoSVG::on_get_name() const { return _u8L("SVG"); }

void GLGizmoSVG::on_render() {
    if (const Selection &selection = m_parent.get_selection(); 
        selection.volumes_count() != 1 || // only one selected volume
        m_volume == nullptr || // already selected volume in gizmo
        get_model_volume(m_volume_id, selection.get_model()->objects) == nullptr) // still exist model
        return;

    bool is_surface_dragging = m_surface_drag.has_value();
    bool is_parent_dragging = m_parent.is_mouse_dragging();
    // Do NOT render rotation grabbers when dragging object
    bool is_rotate_by_grabbers = m_dragging;
    if (is_rotate_by_grabbers || 
        (!is_surface_dragging && !is_parent_dragging)) {
        glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
        m_rotate_gizmo.render();
    }
}

void GLGizmoSVG::on_register_raycasters_for_picking(){
    m_rotate_gizmo.register_raycasters_for_picking();
}
void GLGizmoSVG::on_unregister_raycasters_for_picking(){
    m_rotate_gizmo.unregister_raycasters_for_picking();
}

namespace{
IconManager::VIcons init_icons(IconManager &mng, const GuiCfg &cfg)
{ 
    mng.release();
    
    ImVec2 size(cfg.icon_width, cfg.icon_width);
    // icon order has to match the enum IconType
    std::vector<std::string> filenames{
        "undo.svg",          // reset_value           
        "refresh.svg",       // refresh           
        "open.svg",          // changhe_file
        "burn.svg",          // bake
        "save.svg",          // save
        "obj_warning.svg",   // exclamation // ORCA: use obj_warning instead exclamation. exclamation is not compatible with low res
        "lock_closed.svg",   // lock
        "lock_open.svg",     // unlock
        "reflection_x.svg",  // reflection_x
        "reflection_y.svg",  // reflection_y
    };

    assert(filenames.size() == static_cast<size_t>(IconType::_count));
    std::string path = resources_dir() + "/images/";
    for (std::string &filename : filenames) filename = path + filename;

    auto type = IconManager::RasterType::color_wite_gray;
    return mng.init(filenames, size, type);

    //IconManager::VIcons vicons = mng.init(init_types);
    //
    //// flatten icons
    //IconManager::Icons  icons;
    //icons.reserve(vicons.size());
    //for (IconManager::Icons &i : vicons)
    //    icons.push_back(i.front());
    //return icons;
}
bool draw_clickable(const IconManager::VIcons &icons, IconType type)
{
    return clickable(get_icon(icons, type, IconState::activable), get_icon(icons, type, IconState::hovered));
}

bool reset_button(const IconManager::VIcons &icons)
{
    float reset_offset = ImGui::GetStyle().WindowPadding.x;
    ImGui::SameLine(reset_offset);

    // from GLGizmoCut
    //std::string label_id = "neco";
    //std::string btn_label;
    //btn_label += ImGui::RevertButton;
    //return ImGui::Button((btn_label + "##" + label_id).c_str());

    auto icon = get_icon(icons, IconType::reset_value, IconState::hovered);
    return clickable(icon, icon); // ORCA use orange color for both states
}

} // namespace 

void GLGizmoSVG::on_render_input_window(float x, float y, float bottom_limit)
{
    set_volume_by_selection();

    double screen_scale = wxDisplay(wxGetApp().plater()).GetScaleFactor();

    // Orca
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0, 5.0) * screen_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4.0f * screen_scale);

    // Configuration creation
    if (m_gui_cfg == nullptr || // Exist configuration - first run
        m_gui_cfg->screen_scale != screen_scale || // change of DPI
        m_gui_cfg->dark_mode != m_is_dark_mode // change of dark mode
        ) {
        // Create cache for gui offsets
        ::GuiCfg cfg = create_gui_configuration();
        cfg.screen_scale = screen_scale;
        cfg.dark_mode    = m_is_dark_mode;

        GuiCfg gui_cfg{std::move(cfg)};
        m_gui_cfg = std::make_unique<const GuiCfg>(std::move(gui_cfg));

        m_icons = init_icons(m_icon_manager, *m_gui_cfg); // need regeneration when change resolution(move between monitors)
    }

    // Draw origin position of text during dragging
    if (m_surface_drag.has_value()) {
        ImVec2 mouse_pos = ImGui::GetMousePos();
        ImVec2 center(
            mouse_pos.x + m_surface_drag->mouse_offset.x(),
            mouse_pos.y + m_surface_drag->mouse_offset.y());
        ImU32 color = ImGui::GetColorU32(
            m_surface_drag->exist_hit ? 
                ImVec4(1.f, 1.f, 1.f, .75f) : // transparent white
                ImVec4(1.f, .3f, .3f, .75f)
        ); // Warning color
        const float radius = 16.f;
        ImGuiWrapper::draw_cross_hair(center, radius, color);
    }
    
    static float last_y = 0.0f;
    static float last_h = 0.0f;

    // adjust window position to avoid overlap the view toolbar
    const float win_h = ImGui::GetWindowHeight();
    y = std::min(y, bottom_limit - win_h);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    if (last_h != win_h || last_y != y) {
        // ask canvas for another frame to render the window in the correct position
        m_imgui->set_requires_extra_frame();
        if (last_h != win_h)
            last_h = win_h;
        if (last_y != y)
            last_y = y;
    }

    GizmoImguiBegin(on_get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    draw_window();

    GizmoImguiEnd();

    // Orca
    ImGui::PopStyleVar(2);
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoSVG::on_set_state()
{
    // enable / disable bed from picking
    // Rotation gizmo must work through bed
    m_parent.set_raycaster_gizmos_on_top(GLGizmoBase::m_state == GLGizmoBase::On);

    m_rotate_gizmo.set_state(GLGizmoBase::m_state);

    // Closing gizmo. e.g. selecting another one
    if (GLGizmoBase::m_state == GLGizmoBase::Off) {
        reset_volume();
    } else if (GLGizmoBase::m_state == GLGizmoBase::On) {
        // Try(when exist) set text configuration by volume 
        set_volume_by_selection();
    }
}

void GLGizmoSVG::data_changed(bool is_serializing) { 
    set_volume_by_selection();
    if (!is_serializing && m_volume == nullptr)
        close();
}

void GLGizmoSVG::on_start_dragging() { m_rotate_gizmo.start_dragging(); }
void GLGizmoSVG::on_stop_dragging()
{
    m_rotate_gizmo.stop_dragging();

    // TODO: when start second rotatiton previous rotation rotate draggers
    // This is fast fix for second try to rotate
    // When fixing, move grabber above text (not on side)
    m_rotate_gizmo.set_angle(PI/2);

    // apply rotation
    // TRN This is an item label in the undo-redo stack.
    m_parent.do_rotate(rotation_snapshot_name);
    m_rotate_start_angle.reset();
    volume_transformation_changed();

    // recalculate for surface cut
    if (m_volume != nullptr && 
        m_volume->emboss_shape.has_value() &&
        m_volume->emboss_shape->projection.use_surface)
        process();
}
void GLGizmoSVG::on_dragging(const UpdateData &data) { m_rotate_gizmo.dragging(data); }

#include "slic3r/GUI/BitmapCache.hpp"
#include "nanosvg/nanosvgrast.h"
#include "libslic3r/AABBTreeLines.hpp" // aabb lines for draw filled expolygon

namespace{

// inspired by Xiaolin Wu's line algorithm - https://en.wikipedia.org/wiki/Xiaolin_Wu's_line_algorithm
// Draw inner part of polygon CCW line as full brightness(edge of expolygon)
void wu_draw_line_side(Linef line,
                const std::function<void(int x, int y, float brightess)>& plot) {
    auto ipart = [](float x) -> int {return static_cast<int>(std::floor(x));};
    auto round = [](float x) -> float {return std::round(x);};
    auto fpart = [](float x) -> float {return x - std::floor(x);};
    auto rfpart = [=](float x) -> float {return 1 - fpart(x);};
    
    Vec2d d = line.b - line.a;
    const bool steep = abs(d.y()) > abs(d.x());
    bool is_full; // identify full brightness pixel
    if (steep) {
        is_full = d.y() >= 0;
        std::swap(line.a.x(), line.a.y());
        std::swap(line.b.x(), line.b.y());
        std::swap(d.x(), d.y());
    }else
        is_full = d.x() < 0; // opposit direction of y

    if (line.a.x() > line.b.x()) {
        std::swap(line.a.x(), line.b.x());
        std::swap(line.a.y(), line.b.y());
        d *= -1;
    }
    const float gradient = (d.x() == 0) ? 1. : d.y() / d.x();
        
    int xpx11;
    float intery;
    {
        const float xend = round(line.a.x());
        const float yend = line.a.y() + gradient * (xend - line.a.x());
        const float xgap = rfpart(line.a.x() + 0.5f);
        xpx11 = int(xend);
        const int ypx11 = ipart(yend);
        if (steep) {
            plot(ypx11,     xpx11,  is_full? 1.f : (rfpart(yend) * xgap));
            plot(ypx11 + 1, xpx11, !is_full? 1.f : ( fpart(yend) * xgap));
        } else {
            plot(xpx11, ypx11,     is_full? 1.f : (rfpart(yend) * xgap));
            plot(xpx11, ypx11 + 1,!is_full? 1.f : ( fpart(yend) * xgap));
        }
        intery = yend + gradient;
    }
    
    int xpx12;
    {
        const float xend = round(line.b.x());
        const float yend = line.b.y() + gradient * (xend - line.b.x());
        const float xgap = rfpart(line.b.x() + 0.5f);
        xpx12 = int(xend);
        const int ypx12 = ipart(yend);
        if (steep) {
            plot(ypx12,     xpx12,  is_full? 1.f : (rfpart(yend) * xgap));
            plot(ypx12 + 1, xpx12, !is_full? 1.f : ( fpart(yend) * xgap));
        } else {
            plot(xpx12, ypx12,      is_full? 1.f : (rfpart(yend) * xgap));
            plot(xpx12, ypx12 + 1, !is_full? 1.f : ( fpart(yend) * xgap));
        }
    }
        
    if (steep) {
        if (is_full){
            for (int x = xpx11 + 1; x < xpx12; x++) {
                plot(ipart(intery),     x, 1.f);
                plot(ipart(intery) + 1, x, fpart(intery));
                intery += gradient;
            }
        } else {
            for (int x = xpx11 + 1; x < xpx12; x++) {
                plot(ipart(intery),     x, rfpart(intery));
                plot(ipart(intery) + 1, x, 1.f );
                intery += gradient;
            }
        }
    } else {
        if (is_full){
            for (int x = xpx11 + 1; x < xpx12; x++) {
                plot(x, ipart(intery),     1.f);
                plot(x, ipart(intery) + 1, fpart(intery));
                intery += gradient;
            }
        } else {
            for (int x = xpx11 + 1; x < xpx12; x++) {
                plot(x, ipart(intery),     rfpart(intery));
                plot(x, ipart(intery) + 1, 1.f);
                intery += gradient;
            }
        }
    }
}

#ifdef MORE_DRAWING
// Wu's line algorithm - https://en.wikipedia.org/wiki/Xiaolin_Wu's_line_algorithm
void wu_draw_line(Linef line,
                const std::function<void(int x, int y, float brightess)>& plot) {
    auto ipart = [](float x) -> int {return int(std::floor(x));};
    auto round = [](float x) -> float {return std::round(x);};
    auto fpart = [](float x) -> float {return x - std::floor(x);};
    auto rfpart = [=](float x) -> float {return 1 - fpart(x);};
    
    Vec2d d = line.b - line.a;
    const bool steep = abs(d.y()) > abs(d.x());
    if (steep) {
        std::swap(line.a.x(), line.a.y());
        std::swap(line.b.x(), line.b.y());
    }
    if (line.a.x() > line.b.x()) {
        std::swap(line.a.x(), line.b.x());
        std::swap(line.a.y(), line.b.y());
    }
    d = line.b - line.a;
    const float gradient = (d.x() == 0) ? 1 : d.y() / d.x();
        
    int xpx11;
    float intery;
    {
        const float xend = round(line.a.x());
        const float yend = line.a.y() + gradient * (xend - line.a.x());
        const float xgap = rfpart(line.a.x() + 0.5);
        xpx11 = int(xend);
        const int ypx11 = ipart(yend);
        if (steep) {
            plot(ypx11,     xpx11,  rfpart(yend) * xgap);
            plot(ypx11 + 1, xpx11,   fpart(yend) * xgap);
        } else {
            plot(xpx11, ypx11,    rfpart(yend) * xgap);
            plot(xpx11, ypx11 + 1, fpart(yend) * xgap);
        }
        intery = yend + gradient;
    }
    
    int xpx12;
    {
        const float xend = round(line.b.x());
        const float yend = line.b.y() + gradient * (xend - line.b.x());
        const float xgap = rfpart(line.b.x() + 0.5);
        xpx12 = int(xend);
        const int ypx12 = ipart(yend);
        if (steep) {
            plot(ypx12,     xpx12, rfpart(yend) * xgap);
            plot(ypx12 + 1, xpx12,  fpart(yend) * xgap);
        } else {
            plot(xpx12, ypx12,     rfpart(yend) * xgap);
            plot(xpx12, ypx12 + 1,  fpart(yend) * xgap);
        }
    }
        
    if (steep) {
        for (int x = xpx11 + 1; x < xpx12; x++) {
            plot(ipart(intery),     x, rfpart(intery));
            plot(ipart(intery) + 1, x,  fpart(intery));
            intery += gradient;
        }
    } else {
        for (int x = xpx11 + 1; x < xpx12; x++) {
            plot(x, ipart(intery),     rfpart(intery));
            plot(x, ipart(intery) + 1,  fpart(intery));
            intery += gradient;
        }
    }
}

void draw(const ExPolygonsWithIds &shapes_with_ids, unsigned max_size)
{
    ImVec2 actual_pos = ImGui::GetCursorPos();
    // draw shapes
    BoundingBox bb;
    for (const ExPolygonsWithId &shape : shapes_with_ids)
        bb.merge(get_extents(shape.expoly));

    Point  bb_size    = bb.size();
    double scale      = max_size / (double) std::max(bb_size.x(), bb_size.y());
    ImVec2 win_offset = ImGui::GetWindowPos();
    Point  offset(win_offset.x + actual_pos.x, win_offset.y + actual_pos.y);
    offset += bb_size / 2 * scale;
    auto draw_polygon = [&scale, offset](Slic3r::Polygon p) {
        p.scale(scale, -scale); // Y mirror
        p.translate(offset);
        ImGuiWrapper::draw(p);
    };

    for (const ExPolygonsWithId &shape : shapes_with_ids) {
        for (const ExPolygon &expoly : shape.expoly) {
            draw_polygon(expoly.contour);
            for (const Slic3r::Polygon &hole : expoly.holes)
                draw_polygon(hole);
        }
    }
}

#endif // MORE_DRAWING

template<unsigned int N> // N .. count of channels per pixel
void draw_side_outline(const ExPolygons &shape, const std::array<unsigned char, N> &color, std::vector<unsigned char> &data, size_t data_width, double scale)
{
    int count_lines = data.size() / (N * data_width);
    size_t data_line  = N * data_width;
    auto get_offset  = [count_lines, data_line](int x, int y) {
        // NOTE: y has opposit direction in texture
        return (count_lines - y - 1) * data_line + x * N;
    };

    // overlap color
    auto draw = [&data, data_width, count_lines, get_offset, &color](int x, int y, float brightess) {
        if (x < 0 || y < 0 || static_cast<size_t>(x) >= data_width || y >= count_lines)
            return; // out of image
        size_t offset = get_offset(x, y);
        bool change_color = false;
        for (size_t i = 0; i < N - 1; ++i) {
            if(data[offset + i] != color[i]){
                data[offset + i] = color[i];        
                change_color = true;
            }
        }

        unsigned char &alpha = data[offset + N - 1];
        if (alpha == 0 || change_color){
            alpha = static_cast<unsigned char>(std::round(brightess * 255));
        } else if (alpha != 255){
            alpha = static_cast<unsigned char>(std::min(255, int(alpha) + static_cast<int>(std::round(brightess * 255))));
        }
    };

    BoundingBox bb_unscaled = get_extents(shape);
    Linesf lines = to_linesf(shape);
    BoundingBoxf bb(bb_unscaled.min.cast<double>(), bb_unscaled.max.cast<double>());

    // scale lines to pixels
    if (!is_approx(scale, 1.)) {
        for (Linef &line : lines) {
            line.a *= scale;
            line.b *= scale;
        }
        bb.min *= scale;
        bb.max *= scale;
    }

    for (const Linef &line : lines)
        wu_draw_line_side(line, draw);
}

/// <summary>
/// Draw filled ExPolygon into data
/// line by line inspired by: http://alienryderflex.com/polygon_fill/
/// </summary>
/// <typeparam name="N">Count channels for one pixel(RGBA = 4)</typeparam>
/// <param name="shape">Shape to draw</param>
/// <param name="color">Color of shape contain count of channels(N)</param>
/// <param name="data">Image(2d) stored in 1d array</param>
/// <param name="data_width">Count of pixel on one line(size in data = N x data_width)</param>
/// <param name="scale">Shape scale for conversion to pixels</param>
template<unsigned int N> // N .. count of channels per pixel
void draw_filled(const ExPolygons &shape, const std::array<unsigned char, N>& color, std::vector<unsigned char> &data, size_t data_width, double scale){
    assert(data.size() % N == 0);
    assert(data.size() % data_width == 0);
    assert((data.size() % (N*data_width)) == 0);

    BoundingBox bb_unscaled = get_extents(shape);
    
    Linesf lines = to_linesf(shape);
    BoundingBoxf bb(
        bb_unscaled.min.cast<double>(), 
        bb_unscaled.max.cast<double>());

    // scale lines to pixels
    if (!is_approx(scale, 1.)) {
        for (Linef &line : lines) {
            line.a *= scale;
            line.b *= scale;
        }
        bb.min *= scale;
        bb.max *= scale;
    }

    int count_lines = data.size() / (N * data_width);
    size_t data_line = N * data_width;
    auto get_offset = [count_lines, data_line](int x, int y) {
        // NOTE: y has opposit direction in texture
        return (count_lines - y - 1) * data_line + x * N;
    };
    auto set_color = [&data, &color, get_offset](int x, int y) {
        size_t offset = get_offset(x, y);
        if (data[offset + N - 1] != 0)
            return; // already setted by line
        for (unsigned i = 0; i < N; ++i)
            data[offset + i] = color[i];
    };

    // anti aliased drawing of lines
    auto draw = [&data, width = static_cast<int>(data_width), count_lines, get_offset, &color](int x, int y, float brightess) {
        if (x < 0 || y < 0 || x >= width || y >= count_lines)
            return; // out of image
        size_t offset = get_offset(x, y);
        unsigned char &alpha = data[offset + N - 1];
        if (alpha == 0){
            alpha = static_cast<unsigned char>(std::round(brightess * 255));
            for (size_t i = 0; i < N-1; ++i)
                data[offset + i] = color[i];
        } else if (alpha != 255){
            alpha = static_cast<unsigned char>(std::min(255, int(alpha) + static_cast<int>(std::round(brightess * 255))));
        }
    };

    for (const Linef& line: lines) 
        wu_draw_line_side(line, draw);
    
    auto tree = Slic3r::AABBTreeLines::build_aabb_tree_over_indexed_lines(lines);

    // range for intersection line
    double x1 = bb.min.x() - 1.f;
    double x2 = bb.max.x() + 1.f;

    int max_y = std::min(count_lines, static_cast<int>(std::round(bb.max.y())));
    for (int y = std::max(0, static_cast<int>(std::round(bb.min.y()))); y < max_y; ++y){
        double y_f = y + .5; // 0.5 ... intersection in center of pixel of pixel
        Linef line(Vec2d(x1, y_f), Vec2d(x2, y_f));
        using Intersection = std::pair<Vec2d, size_t>;
        using Intersections = std::vector<Intersection>;
        // sorted .. false
        // <false, Vec2d, Linef, decltype(tree)>
        Intersections intersections = Slic3r::AABBTreeLines::get_intersections_with_line<false, Vec2d, Linef>(lines, tree, line);
        if (intersections.empty())
            continue;

        assert((intersections.size() % 2) == 0);

        // sort intersections by x
        std::sort(intersections.begin(), intersections.end(), 
            [](const Intersection &i1, const Intersection &i2) { return i1.first.x() < i2.first.x(); });

        // draw lines
        for (size_t i = 0; i < intersections.size(); i+=2) {
            const Vec2d& p2 = intersections[i+1].first;
            if (p2.x() < 0)
                continue; // out of data

            const Vec2d& p1 = intersections[i].first;
            if (p1.x() > data_width)
                break; // out of data

            // clamp to data
            int max_x = std::min(static_cast<int>(data_width-1), static_cast<int>(std::round(p2.x())));
            for (int x = std::max(0, static_cast<int>(std::round(p1.x()))); x <= max_x; ++x)
                set_color(x, y);
        }
    }  
}

/// Union shape defined by glyphs
ExPolygons union_ex(const ExPolygonsWithIds &shapes)
{
    // unify to one expolygon
    ExPolygons result;
    for (const ExPolygonsWithId &shape : shapes) {
        if (shape.expoly.empty())
            continue;
        expolygons_append(result, shape.expoly);
    }
    return union_ex(result);
}

// init texture by draw expolygons into texture
bool init_texture(Texture &texture, const ExPolygonsWithIds& shapes_with_ids, unsigned max_size_px, const std::vector<std::string>& shape_warnings){
    BoundingBox bb = get_extents(shapes_with_ids);
    Point  bb_size   = bb.size();
    double bb_width  = bb_size.x(); // [in mm]
    double bb_height = bb_size.y(); // [in mm]

    bool is_widder = bb_size.x() > bb_size.y();
    double scale = 0.f;
    if (is_widder) {
        scale          = max_size_px / bb_width;
        texture.width  = max_size_px;
        texture.height = static_cast<unsigned>(std::ceil(bb_height * scale));
    } else {
        scale          = max_size_px / bb_height;
        texture.width  = static_cast<unsigned>(std::ceil(bb_width * scale));
        texture.height = max_size_px;
    }
    const int n_pixels = texture.width * texture.height;
    if (n_pixels <= 0)
        return false;

    constexpr int channels_count = 4;
    std::vector<unsigned char> data(n_pixels * channels_count, {0});

    // Union All shapes
    ExPolygons shape = union_ex(shapes_with_ids);

    // align to texture
    translate(shape, -bb.min);
    size_t texture_width = static_cast<size_t>(texture.width);
    unsigned char alpha = 255; // without transparency
    std::array<unsigned char, 4> color_shape{201, 201, 201, alpha}; // from degin by @JosefZachar
    std::array<unsigned char, 4> color_error{237, 28, 36, alpha}; // from icon: resources/icons/flag_red.svg
    std::array<unsigned char, 4> color_warning{237, 107, 33, alpha}; // icons orange
    // draw unhealedable shape
    for (const ExPolygonsWithId &shapes_with_id : shapes_with_ids)
        if (!shapes_with_id.is_healed) {
            ExPolygons bad_shape = shapes_with_id.expoly; // copy
            translate(bad_shape, -bb.min); // align to texture
            draw_side_outline<4>(bad_shape, color_error, data, texture_width, scale);
        }
    // Draw shape with warning
    if (!shape_warnings.empty()) {
        for (const ExPolygonsWithId &shapes_with_id : shapes_with_ids){
            assert(shapes_with_id.id < shape_warnings.size());
            if (shapes_with_id.id >= shape_warnings.size())
                continue;
            if (shape_warnings[shapes_with_id.id].empty())
                continue; // no warnings for shape
            ExPolygons warn_shape = shapes_with_id.expoly; // copy
            translate(warn_shape, -bb.min); // align to texture
            draw_side_outline<4>(warn_shape, color_warning, data, texture_width, scale);
        }
    }

    // Draw rest of shape
    draw_filled<4>(shape, color_shape, data, texture_width, scale);

    // sends data to gpu 
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    if (texture.id != 0)
        glsafe(::glDeleteTextures(1, &texture.id));
    glsafe(::glGenTextures(1, &texture.id));
    glsafe(::glBindTexture(GL_TEXTURE_2D, texture.id));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei) texture.width, (GLsizei) texture.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                          (const void *) data.data()));

    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));

    GLuint NO_TEXTURE_ID = 0;
    glsafe(::glBindTexture(GL_TEXTURE_2D, NO_TEXTURE_ID));
    return true;
}

bool is_closed(NSVGpath *path){
    for (; path != NULL; path = path->next)
        if (path->next == NULL && path->closed)
            return true;    
    return false;
}

void add_comma_separated(std::string &result, const std::string &add){
    if (!result.empty())
        result += ", ";
    result += add;
}

const float warning_preccission = 1e-4f;
std::string create_fill_warning(const NSVGshape &shape) {
    if (!(shape.flags & NSVG_FLAGS_VISIBLE) || 
        shape.fill.type == NSVG_PAINT_NONE)
        return {}; // not visible

    std::string warning;
    if ((shape.opacity - 1.f + warning_preccission) <= 0.f)
        add_comma_separated(warning, GUI::format(_L("Opacity (%1%)"), shape.opacity));

    // if(shape->flags != NSVG_FLAGS_VISIBLE) add_warning(_u8L("Visibility flag"));
    bool is_fill_gradient = shape.fillGradient[0] != '\0';
    if (is_fill_gradient)
        add_comma_separated(warning, GUI::format(_L("Color gradient (%1%)"), shape.fillGradient));

    switch (shape.fill.type) {
    case NSVG_PAINT_UNDEF: add_comma_separated(warning, _u8L("Undefined fill type")); break;
    case NSVG_PAINT_LINEAR_GRADIENT:
        if (!is_fill_gradient)
            add_comma_separated(warning, _u8L("Linear gradient"));
        break;
    case NSVG_PAINT_RADIAL_GRADIENT:
        if (!is_fill_gradient)
            add_comma_separated(warning, _u8L("Radial gradient"));
        break;
    // case NSVG_PAINT_NONE:
    // case NSVG_PAINT_COLOR:
    // default: break;
    }

    // Unfilled is only line which could be opened
    if (shape.fill.type != NSVG_PAINT_NONE && !is_closed(shape.paths))
        add_comma_separated(warning, _u8L("Open filled path"));
    return warning;    
}

std::string create_stroke_warning(const NSVGshape &shape) {

    std::string warning;
    if (!(shape.flags & NSVG_FLAGS_VISIBLE) ||        
        shape.stroke.type == NSVG_PAINT_NONE ||
        shape.strokeWidth <= 1e-5f)
        return {}; // not visible

    if ((shape.opacity - 1.f + warning_preccission) <= 0.f)
        add_comma_separated(warning, GUI::format(_L("Opacity (%1%)"), shape.opacity));

    bool is_stroke_gradient = shape.strokeGradient[0] != '\0';
    if (is_stroke_gradient)
        add_comma_separated(warning, GUI::format(_L("Color gradient (%1%)"), shape.strokeGradient));

    switch (shape.stroke.type) {
    case NSVG_PAINT_UNDEF: add_comma_separated(warning, _u8L("Undefined stroke type")); break;
    case NSVG_PAINT_LINEAR_GRADIENT:
        if (!is_stroke_gradient)
            add_comma_separated(warning, _u8L("Linear gradient"));
        break;
    case NSVG_PAINT_RADIAL_GRADIENT:
        if (!is_stroke_gradient)
            add_comma_separated(warning, _u8L("Radial gradient"));
        break;
    // case NSVG_PAINT_COLOR:
    // case NSVG_PAINT_NONE:
    // default: break;
    }

    return warning;
}

/// <summary>
/// Create warnings about shape
/// </summary>
/// <param name="image">Input svg loaded to shapes</param>
/// <returns>Vector of warnings with same size as EmbossShape::shapes_with_ids
/// or Empty when no warnings -> for fast checking that every thing is all right(more common case) </returns>
std::vector<std::string> create_shape_warnings(const EmbossShape &shape, float scale){
    const std::shared_ptr<NSVGimage>& image_ptr = shape.svg_file->image;
    assert(image_ptr != nullptr);
    if (image_ptr == nullptr)
        return {std::string{"Uninitialized SVG image"}};

    const NSVGimage &image = *image_ptr;
    std::vector<std::string> result;
    auto add_warning = [&result, &image](size_t index, const std::string &message) {
        if (result.empty())
            result = std::vector<std::string>(get_shapes_count(image) * 2);
        std::string &res = result[index];
        if (res.empty())
            res = message;
        else
            res += '\n' + message;
    };

    if (!shape.final_shape.is_healed) {
        for (const ExPolygonsWithId &i : shape.shapes_with_ids)
            if (!i.is_healed)
                add_warning(i.id, _u8L("Path can't be healed from self-intersection and multiple points."));

        // This waning is not connected to NSVGshape. It is about union of paths, but Zero index is shown first
        size_t index = 0;
        add_warning(index, _u8L("Final shape contains self-intersection or multiple points with same coordinate."));
    }

    size_t shape_index = 0;
    for (NSVGshape *shape = image.shapes; shape != NULL; shape = shape->next, ++shape_index) {
        if (!(shape->flags & NSVG_FLAGS_VISIBLE)){
            add_warning(shape_index * 2, GUI::format(_L("Shape is marked as invisible (%1%)."), shape->id));
            continue;
        }

        std::string fill_warning = create_fill_warning(*shape);
        if (!fill_warning.empty()) {
            // TRN: The first placeholder is shape identifier, the second is text describing the problem.
            add_warning(shape_index * 2, GUI::format(_L("Fill of shape (%1%) contains unsupported: %2%."), shape->id, fill_warning));
        }
        
        float minimal_width_in_mm = 1e-3f;
        if (shape->strokeWidth <= minimal_width_in_mm * scale) {
            add_warning(shape_index * 2, GUI::format(_L("Stroke of shape (%1%) is too thin (minimal width is %2% mm)."), shape->id, minimal_width_in_mm));
            continue;
        }
        std::string stroke_warning = create_stroke_warning(*shape);
        if (!stroke_warning.empty())
            add_warning(shape_index * 2 + 1, GUI::format(_L("Stroke of shape (%1%) contains unsupported: %2%."), shape->id, stroke_warning));
    }
    return result;
}


} // namespace

void GLGizmoSVG::set_volume_by_selection()
{
    const Selection &selection = m_parent.get_selection();
    const GLVolume *gl_volume = get_selected_gl_volume(selection);
    if (gl_volume == nullptr)
        return reset_volume();

    const ModelObjectPtrs &objects = selection.get_model()->objects;
    ModelVolume *volume =get_model_volume(*gl_volume, objects);
    if (volume == nullptr)
        return reset_volume();

    // is same volume as actual selected?
    if (volume->id() == m_volume_id)
        return;

    // Do not use focused input value when switch volume(it must swith value)
    if (m_volume != nullptr && 
        m_volume != volume) // when update volume it changed id BUT not pointer
        ImGuiWrapper::left_inputs();

    // is valid svg volume?
    if (!is_svg(*volume)) 
        return reset_volume();

    // cancel previous job
    if (m_job_cancel != nullptr) {
        m_job_cancel->store(true);
        m_job_cancel = nullptr;
    }
        
    // calculate scale for height and depth inside of scaled object instance
    calculate_scale(); // must be before calculation of tesselation

    // checking that exist is inside of function "is_svg"
    EmbossShape &es = *volume->emboss_shape;
    EmbossShape::SvgFile &svg_file = *es.svg_file;
    if (svg_file.image == nullptr) {
        if (init_image(svg_file) == nullptr)
            return reset_volume();
    }
    assert(svg_file.image != nullptr);
    assert(svg_file.image.get() != nullptr);
    const NSVGimage &image = *svg_file.image;
    ExPolygonsWithIds &shape_ids = es.shapes_with_ids;
    if (shape_ids.empty()) {        
        NSVGLineParams params{get_tesselation_tolerance(get_scale_for_tolerance())};
        shape_ids = create_shape_with_ids(image, params);                
    }

    reset_volume(); // clear cached data

    m_volume = volume;
    m_volume_id = volume->id();
    m_volume_shape = es; // copy
    m_shape_warnings = create_shape_warnings(es, get_scale_for_tolerance());

    // Calculate current angle of up vector
    m_angle    = calc_angle(selection);
    m_distance = calc_distance(*gl_volume, m_raycast_manager, m_parent);
    
    m_shape_bb = get_extents(m_volume_shape.shapes_with_ids);
}
namespace {
void delete_texture(Texture& texture){
    if (texture.id != 0) {
        glsafe(::glDeleteTextures(1, &texture.id));
        texture.id = 0;
    }
}
}
void GLGizmoSVG::reset_volume()
{
    if (m_volume == nullptr)
        return; // already reseted

    m_volume = nullptr;
    m_volume_id.id = 0;
    m_volume_shape.shapes_with_ids.clear();
    m_filename_preview.clear();
    m_shape_warnings.clear();
    // delete texture after finish imgui draw
    wxGetApp().plater()->CallAfter([&texture = m_texture]() { delete_texture(texture); });
}

void GLGizmoSVG::calculate_scale() {
    // be carefull m_volume is not set yet
    const Selection &selection = m_parent.get_selection(); 
    const GLVolume *gl_volume = selection.get_first_volume();
    if (gl_volume == nullptr)
        return;

    Transform3d to_world = gl_volume->world_matrix();

    const ModelVolume *volume_ptr = get_model_volume(*gl_volume, selection.get_model()->objects);
    assert(volume_ptr != nullptr);
    assert(volume_ptr->emboss_shape.has_value());
    // Fix for volume loaded from 3mf
    if (volume_ptr != nullptr &&
        volume_ptr->emboss_shape.has_value()) {
        const std::optional<Transform3d> &fix_tr = volume_ptr->emboss_shape->fix_3mf_tr;
        if (fix_tr.has_value())
            to_world = to_world * (fix_tr->inverse());    
    }
    
    auto to_world_linear = to_world.linear();
    auto calc = [&to_world_linear](const Vec3d &axe, std::optional<float>& scale) {
        Vec3d axe_world = to_world_linear * axe;
        double norm_sq  = axe_world.squaredNorm();
        if (is_approx(norm_sq, 1.)) {
            if (!scale.has_value())
                return;
            scale.reset();
        } else {
            scale = sqrt(norm_sq);
        }
    };

    calc(Vec3d::UnitX(), m_scale_width);
    calc(Vec3d::UnitY(), m_scale_height);
    calc(Vec3d::UnitZ(), m_scale_depth);
}

float GLGizmoSVG::get_scale_for_tolerance(){ 
    return std::max(m_scale_width.value_or(1.f), m_scale_height.value_or(1.f)); }

bool GLGizmoSVG::process(bool make_snapshot) {
    // no volume is selected -> selection from right panel
    assert(m_volume != nullptr);
    if (m_volume == nullptr) 
        return false;
    
    assert(m_volume->emboss_shape.has_value());
    if (!m_volume->emboss_shape.has_value())
        return false;

    // Cancel previous Job, when it is in process
    // worker.cancel(); --> Use less in this case I want cancel only previous EmbossJob no other jobs
    // Cancel only EmbossUpdateJob no others
    if (m_job_cancel != nullptr)
        m_job_cancel->store(true);
    // create new shared ptr to cancel new job
    m_job_cancel = std::make_shared<std::atomic<bool>>(false);

    EmbossShape shape = m_volume_shape; // copy
    auto base = std::make_unique<DataBase>(m_volume->name, m_job_cancel, std::move(shape));
    base->is_outside = m_volume->type() == ModelVolumeType::MODEL_PART;
    DataUpdate data{std::move(base), m_volume_id, make_snapshot};
    return start_update_volume(std::move(data), *m_volume, m_parent.get_selection(), m_raycast_manager);    
}

void GLGizmoSVG::close()
{
    // close gizmo == open it again
    auto &mng = m_parent.get_gizmos_manager();
    if (mng.get_current_type() == GLGizmosManager::Svg)
        mng.open_gizmo(GLGizmosManager::Svg);
    reset_volume();
}

void GLGizmoSVG::draw_window()
{
    assert(m_volume != nullptr);
    assert(m_volume_id.valid());
    if (m_volume == nullptr ||
        m_volume_id.invalid()) {
        ImGui::Text("Not valid state please report reproduction steps on github");
        return;
    }

    assert(m_volume->emboss_shape.has_value());
    if (!m_volume->emboss_shape.has_value()) {
        ImGui::Text("No embossed file");
        return;
    }

    assert(m_volume->emboss_shape->svg_file.has_value());
    if (!m_volume->emboss_shape->svg_file.has_value()){
        ImGui::Text("Missing svg file in embossed shape");
        return;
    }

    assert(m_volume->emboss_shape->svg_file->file_data != nullptr);
    if (m_volume->emboss_shape->svg_file->file_data == nullptr){
        ImGui::Text("Missing data of svg file");
        return;
    }

    draw_preview();
    draw_filename();

    // Is SVG baked?
    if (m_volume == nullptr) return;

    ImGui::Separator();

    ImGui::Indent(m_gui_cfg->icon_width);
    draw_depth();
    draw_size();
    draw_use_surface();

    draw_distance();
    draw_rotation();
    draw_mirroring();
    draw_face_the_camera();

    ImGui::Unindent(m_gui_cfg->icon_width);  

    if (!m_volume->is_the_only_one_part()) {
        ImGui::Separator();
        draw_model_type();
    }
}

void GLGizmoSVG::draw_face_the_camera(){
    if (ImGui::Button(_u8L("Face the camera").c_str())) {
        const Camera &cam = wxGetApp().plater()->get_camera();
        auto wanted_up_limit = (m_keep_up) ? std::optional<double>(UP_LIMIT) : std::optional<double>{};
        if (face_selected_volume_to_camera(cam, m_parent, wanted_up_limit))
            volume_transformation_changed();
    }
}

void GLGizmoSVG::draw_preview(){
    // init texture when not initialized yet.
    // drag&drop is out of rendering scope so texture must be created on this place
    if (m_texture.id == 0) {
        const ExPolygonsWithIds &shapes = m_volume->emboss_shape->shapes_with_ids;
        init_texture(m_texture, shapes, m_gui_cfg->texture_max_size_px, m_shape_warnings);
    }

    //::draw(m_volume_shape.shapes_with_ids, m_gui_cfg->texture_max_size_px);

    if (m_texture.id != 0) {
        ImTextureID id = (void *) static_cast<intptr_t>(m_texture.id);
        ImVec2      s(m_texture.width, m_texture.height);

        std::optional<float> spacing;
        // is texture over full height?
        if (m_texture.height != m_gui_cfg->texture_max_size_px) {
            spacing = (m_gui_cfg->texture_max_size_px - m_texture.height) / 2.f;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + *spacing);
        }
        // is texture over full width?
        unsigned window_width = static_cast<unsigned>(
            ImGui::GetWindowSize().x - 2*ImGui::GetStyle().WindowPadding.x);
        if (window_width > m_texture.width){
            float space = (window_width - m_texture.width) / 2.f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + space);
        }

        ImGui::Image(id, s);
        //if(ImGui::IsItemHovered()){            
        //    const EmbossShape &es = *m_volume->emboss_shape;
        //    size_t count_of_shapes = get_shapes_count(*es.svg_file.image);
        //    size_t count_of_expolygons = 0;
        //    size_t count_of_points = 0;
        //    for (const auto &shape : es.shapes_with_ids) {
        //        for (const ExPolygon &expoly : shape.expoly){
        //            ++count_of_expolygons;
        //            count_of_points += count_points(expoly);
        //        }
        //    }
        //    // Do not translate it is only for debug
        //    std::string tooltip = GUI::format("%1% shapes, which create %2% polygons with %3% line segments",
        //        count_of_shapes, count_of_expolygons, count_of_points);
        //    ImGui::SetTooltip("%s", tooltip.c_str());
        //}
                
        if (spacing.has_value())
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + *spacing);        
    }    
}

void GLGizmoSVG::draw_filename(){
    const EmbossShape &es = *m_volume->emboss_shape;
    const EmbossShape::SvgFile &svg = *es.svg_file;
    if (m_filename_preview.empty()){
        // create filename preview
        if (!svg.path.empty()) {
            m_filename_preview = get_file_name(svg.path);
        } else if (!svg.path_in_3mf.empty()) {
            m_filename_preview = get_file_name(svg.path_in_3mf);
        }

        if (m_filename_preview.empty())
            // TRN - Preview of filename after clear local filepath.
            m_filename_preview = _u8L("Unknown filename");
        
        m_filename_preview = ImGuiWrapper::trunc(m_filename_preview, m_gui_cfg->input_width);
    }

    if (!m_shape_warnings.empty()){
        draw(get_icon(m_icons, IconType::exclamation, IconState::hovered));
        if (ImGui::IsItemHovered()) {
            std::string tooltip;
            for (const std::string &w: m_shape_warnings){
                if (w.empty())
                    continue;
                if (!tooltip.empty())
                    tooltip += "\n";
                tooltip += w;
            }
            m_imgui->tooltip(tooltip, m_gui_cfg->max_tooltip_width);
        }
        ImGui::SameLine();
    }

    // Remove space between filename and gray suffix ".svg"
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", m_filename_preview.c_str());
    bool is_hovered = ImGui::IsItemHovered();
    ImGui::SameLine();
    m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, ".svg");
    ImGui::PopStyleVar(); // ImGuiStyleVar_ItemSpacing 

    is_hovered |= ImGui::IsItemHovered();
    if (is_hovered) {
        std::string tooltip = GUI::format(_L("SVG file path is \"%1%\""), svg.path);
        m_imgui->tooltip(tooltip, m_gui_cfg->max_tooltip_width);
    }

    bool file_changed = false;

    // Re-Load button
    bool can_reload = !m_volume_shape.svg_file->path.empty();
    if (can_reload) {
        ImGui::SameLine();
        if (draw_clickable(m_icons, IconType::refresh)) {
            if (!boost::filesystem::exists(m_volume_shape.svg_file->path)) {
                m_volume_shape.svg_file->path.clear();
            } else {
                file_changed = true;
            }
        } else if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Reload SVG file from disk."), m_gui_cfg->max_tooltip_width);
    }

    std::string tooltip = "";
    ImGuiComboFlags flags = ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_NoPreview;
    ImGui::SameLine();
    ImGuiWrapper::push_combo_style(m_parent.get_scale());
    if (ImGui::BeginCombo("##file_options", nullptr, flags)) {
        ScopeGuard combo_sg([]() { ImGui::EndCombo(); });

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {ImGui::GetStyle().FramePadding.x, 0});
        draw(get_icon(m_icons, IconType::change_file, IconState::hovered));
        ImGui::SameLine();
        if (ImGui::Selectable((_L("Change file") + dots).ToUTF8().data())) {
            std::string new_path = choose_svg_file();
            if (!new_path.empty()) {
                file_changed = true;
                EmbossShape::SvgFile svg_file_new;
                svg_file_new.path = new_path;
                m_volume_shape.svg_file = svg_file_new; // clear data
            }
        } else if (ImGui::IsItemHovered()) {
            tooltip = _u8L("Change to another SVG file.");
        }

        std::string forget_path = _u8L("Forget the file path");
        if (m_volume->emboss_shape->svg_file->path.empty()){
            draw(get_icon(m_icons, IconType::bake, IconState::disabled));
            ImGui::SameLine();
            m_imgui->text_colored(ImGuiWrapper::COL_GREY_DARK, forget_path.c_str());
        } else {
            draw(get_icon(m_icons, IconType::bake, IconState::hovered));
            ImGui::SameLine();
            if (ImGui::Selectable(forget_path.c_str())) {
                // set .svg_file.path_in_3mf to remember file name
                m_volume->emboss_shape->svg_file->path.clear();
                m_volume_shape.svg_file->path.clear();
                m_filename_preview.clear();
            } else if (ImGui::IsItemHovered()) {
                tooltip = _u8L("Do NOT save local path to 3MF file.\n"
                               "Also disables 'reload from disk' option.");
            }
        }
        
        //draw(get_icon(m_icons, IconType::bake));
        //ImGui::SameLine();
        //if (ImGui::Selectable(_u8L("Bake 2 ©").c_str())) {
        //    EmbossShape::SvgFile &svg = m_volume_shape.svg_file;
        //    std::stringstream ss;
        //    Slic3r::save(*svg.image, ss);
        //    svg.file_data = std::make_unique<std::string>(ss.str());
        //    svg.image     = nsvgParse(*svg.file_data);
        //    assert(svg.image.get() != NULL);
        //    if (svg.image.get() != NULL) {
        //        m_volume->emboss_shape->svg_file = svg; // copy - write changes into volume
        //    } else {
        //        svg = m_volume->emboss_shape->svg_file; // revert changes
        //    }
        //} else if (ImGui::IsItemHovered()) {
        //    ImGui::SetTooltip("%s", _u8L("Use only paths from svg - recreate svg").c_str());
        //}
                
        draw(get_icon(m_icons, IconType::bake, IconState::hovered));
        ImGui::SameLine();
        // TRN: An menu option to convert the SVG into an unmodifiable model part.
        if (ImGui::Selectable(_u8L("Bake").c_str())) {
            m_volume->emboss_shape.reset();
            close();
        } else if (ImGui::IsItemHovered()) {
            // TRN: Tooltip for the menu item.
            tooltip = _u8L("Bake into model as uneditable part");
        }

        draw(get_icon(m_icons, IconType::save, IconState::activable));
        ImGui::SameLine();
        if (ImGui::Selectable((_L("Save as") + dots).ToUTF8().data())) {
            wxWindow *parent = nullptr;
            GUI::FileType file_type  = FT_SVG;
            wxString wildcard = file_wildcards(file_type);
            wxString dlg_title = _L("Save SVG file");
            const EmbossShape::SvgFile& svg = *m_volume_shape.svg_file;
            wxString dlg_file = from_u8(get_file_name(((!svg.path.empty()) ? svg.path : svg.path_in_3mf))) + ".svg";
            wxFileDialog dlg(parent, dlg_title, last_used_directory, dlg_file, wildcard, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            if (dlg.ShowModal() == wxID_OK ){
                last_used_directory = dlg.GetDirectory();
                wxString out_path = dlg.GetPath();
                std::string path{out_path.c_str()};
                //Slic3r::save(*m_volume_shape.svg_file.image, path);

                std::ofstream stream(path);
                if (stream.is_open()){
                    stream << *svg.file_data;

                    // change source file
                    m_filename_preview.clear();
                    m_volume_shape.svg_file->path = path;
                    m_volume_shape.svg_file->path_in_3mf.clear(); // possible change name
                    m_volume->emboss_shape->svg_file = m_volume_shape.svg_file; // copy - write changes into volume
                } else {
                    BOOST_LOG_TRIVIAL(error) << "Opening file: \"" << path << "\" Failed";
                }

            }
        } else if (ImGui::IsItemHovered()) {
            tooltip = _u8L("Save as SVG file.");
        }

        //draw(get_icon(m_icons, IconType::save));
        //ImGui::SameLine();
        //if (ImGui::Selectable((_L("Save used as") + dots).ToUTF8().data())) {
        //    GUI::FileType file_type  = FT_SVG;
        //    wxString wildcard = file_wildcards(file_type);
        //    wxString dlg_title = _L("Export SVG file:");
        //    wxString dlg_dir = from_u8(wxGetApp().app_config->get_last_dir());
        //    const EmbossShape::SvgFile& svg = m_volume_shape.svg_file;
        //    wxString dlg_file = from_u8(get_file_name(((!svg.path.empty()) ? svg.path : svg.path_in_3mf))) + ".svg";
        //    wxFileDialog dlg(nullptr, dlg_title, dlg_dir, dlg_file, wildcard, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        //    if (dlg.ShowModal() == wxID_OK ){
        //        wxString out_path = dlg.GetPath();        
        //        std::string path{out_path.c_str()};
        //        Slic3r::save(*m_volume_shape.svg_file.image, path);
        //    }
        //} else if (ImGui::IsItemHovered()) {
        //    ImGui::SetTooltip("%s", _u8L("Save only used path as '.svg' file").c_str());
        //}
        ImGui::PopStyleVar(1);
    }
    ImGuiWrapper::pop_combo_style();
    if (!tooltip.empty())
        m_imgui->tooltip(tooltip, m_gui_cfg->max_tooltip_width);

    if (file_changed) {
        float scale = get_scale_for_tolerance();
        double tes_tol = get_tesselation_tolerance(scale);
        EmbossShape es_ = select_shape(m_volume_shape.svg_file->path, tes_tol);
        m_volume_shape.svg_file = std::move(es_.svg_file);
        m_volume_shape.shapes_with_ids = std::move(es_.shapes_with_ids);
        m_volume_shape.final_shape = {}; // clear cache
        m_shape_warnings = create_shape_warnings(m_volume_shape, scale);
        init_texture(m_texture, m_volume_shape.shapes_with_ids, m_gui_cfg->texture_max_size_px, m_shape_warnings);
        process();
    }
}

void GLGizmoSVG::draw_depth()
{
    ImGui::AlignTextToFramePadding();
    ImGuiWrapper::text(m_gui_cfg->translations.depth);
    ImGui::SameLine(m_gui_cfg->input_offset);
    ImGui::SetNextItemWidth(m_gui_cfg->input_width);

    bool use_inch = wxGetApp().app_config->get_bool("use_inches");
    double &value = m_volume_shape.projection.depth;
    constexpr double step = 1.;
    constexpr double step_fast = 10.;
    std::optional<double> result_scale;
    const char *size_format = "%.1f mm";
    double input = value;
    if (use_inch) {
        size_format = "%.2f in";
        // input in inches
        input *= GizmoObjectManipulation::mm_to_in * m_scale_depth.value_or(1.f);
        result_scale = GizmoObjectManipulation::in_to_mm / m_scale_depth.value_or(1.f);
    } else if (m_scale_depth.has_value()) {
        // scale input
        input *= (*m_scale_depth);
        result_scale = 1. / (*m_scale_depth);
    }
    
    if (ImGui::InputDouble("##depth", &input, step, step_fast, size_format)) {
        if (result_scale.has_value())
            input *= (*result_scale);
        apply(input, limits.depth);
        if (!is_approx(input, value, 1e-4)){
            value = input;
            process();
        }
    } else if (ImGui::IsItemHovered())
        m_imgui->tooltip(_u8L("Size in emboss direction."), m_gui_cfg->max_tooltip_width);
}

void GLGizmoSVG::draw_size() 
{
    ImGui::AlignTextToFramePadding();
    bool can_reset = m_scale_width.has_value() || m_scale_height.has_value();
    ImVec4 text_color = can_reset ? ImGuiWrapper::COL_MODIFIED : ImGui::GetStyleColorVec4(ImGuiCol_Text); // ORCA use modified color on text
    ImGuiWrapper::text_colored(text_color, m_gui_cfg->translations.size);
    if (ImGui::IsItemHovered()){
        size_t count_points = 0;
        for (const auto &s : m_volume_shape.shapes_with_ids)
            count_points += Slic3r::count_points(s.expoly);
        // TRN: The placeholder contains a number.
        m_imgui->tooltip(GUI::format(_L("Scale also changes amount of curve samples (%1%)"), count_points), m_gui_cfg->max_tooltip_width);
    }

    bool use_inch = wxGetApp().app_config->get_bool("use_inches");
    
    Point size = m_shape_bb.size();
    double width = size.x() * m_volume_shape.scale * m_scale_width.value_or(1.f);
    if (use_inch) width *= GizmoObjectManipulation::mm_to_in;    
    double height = size.y() * m_volume_shape.scale * m_scale_height.value_or(1.f);
    if (use_inch) height *= GizmoObjectManipulation::mm_to_in;

    const auto is_valid_scale_ratio = [limit = &limits.relative_scale_ratio](double ratio) {
        if (std::fabs(ratio - 1.) < limit->min)
            return false; // too small ratio --> without effect

        if (ratio > limit->max)
            return false;

        if (ratio < 1e-4) 
            return false; // negative scale is not allowed

        return true;    
    };

    std::optional<Vec3d> new_relative_scale;
    bool make_snap = false;

    if (m_keep_ratio) {
        std::stringstream ss;
        ss << std::setprecision(2) << std::fixed << width << " x " << height << " " << (use_inch ? "in" : "mm");

        ImGui::SameLine(m_gui_cfg->input_offset);
        ImGui::SetNextItemWidth(m_gui_cfg->input_width);

        const MinMax<float> &minmax = use_inch ? limits.ui_size_in : limits.ui_size;
        // convert to float for slider
        float width_f = static_cast<float>(width);
        if (m_imgui->slider_float("##width_size_slider", &width_f, minmax.min, minmax.max, ss.str().c_str(), 1.f, false)) {
            double width_ratio = width_f / width;
            if (is_valid_scale_ratio(width_ratio)) {
                m_scale_width      = m_scale_width.value_or(1.f) * width_ratio;
                m_scale_height     = m_scale_height.value_or(1.f) * width_ratio;
                new_relative_scale = Vec3d(width_ratio, width_ratio, 1.);
            }
        }
        if (m_imgui->get_last_slider_status().deactivated_after_edit)
            make_snap = true; // only last change of slider make snap
    } else {
        ImGuiInputTextFlags flags = 0;

        float space         = m_gui_cfg->icon_width / 2;
        float input_width   = m_gui_cfg->input_width / 2 - space / 2;
        float second_offset = m_gui_cfg->input_offset + input_width + space;

        const char *size_format = (use_inch) ? "%.2f in" : "%.1f mm";
        double step = -1.0;
        double fast_step = -1.0;

        ImGui::SameLine(m_gui_cfg->input_offset);
        ImGui::SetNextItemWidth(input_width);
        double prev_width = width;
        if (ImGui::InputDouble("##width", &width, step, fast_step, size_format, flags)) {
            double width_ratio = width / prev_width;
            if (is_valid_scale_ratio(width_ratio)) {
                m_scale_width = m_scale_width.value_or(1.f) * width_ratio;
                new_relative_scale = Vec3d(width_ratio, 1., 1.);
                make_snap = true;
            }
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Width of SVG."), m_gui_cfg->max_tooltip_width);

        ImGui::SameLine(second_offset);
        ImGui::SetNextItemWidth(input_width);
        double prev_height = height;
        if (ImGui::InputDouble("##height", &height, step, fast_step, size_format, flags)) {
            double height_ratio = height / prev_height;
            if (is_valid_scale_ratio(height_ratio)) {
                m_scale_height = m_scale_height.value_or(1.f) * height_ratio;
                new_relative_scale  = Vec3d(1., height_ratio, 1.);
                make_snap = true;
            }
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Height of SVG."), m_gui_cfg->max_tooltip_width);
    }

    // Lock on ratio m_keep_ratio
    ImGui::SameLine(m_gui_cfg->lock_offset);
    const IconManager::Icon &icon       = get_icon(m_icons, m_keep_ratio ? IconType::lock : IconType::unlock, IconState::activable);
    const IconManager::Icon &icon_hover = get_icon(m_icons, m_keep_ratio ? IconType::lock : IconType::unlock, IconState::hovered);
    if (button(icon, icon_hover, icon))
        m_keep_ratio = !m_keep_ratio;    
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_u8L("Lock/unlock the aspect ratio of the SVG."), m_gui_cfg->max_tooltip_width);
    

    // reset button
    //bool can_reset = m_scale_width.has_value() || m_scale_height.has_value(); // ORCA update variable above if condition change
    if (can_reset) {
        if (reset_button(m_icons)) {
            new_relative_scale = Vec3d(1./m_scale_width.value_or(1.f), 1./m_scale_height.value_or(1.f), 1.);
            make_snap = true;
        } else if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Reset scale"), m_gui_cfg->max_tooltip_width);
    }

    if (new_relative_scale.has_value()){
        Selection &selection = m_parent.get_selection();
        selection.setup_cache();

        auto selection_scale_fnc = [&selection, rel_scale = *new_relative_scale]() {
            selection.scale(rel_scale, get_drag_transformation_type(selection));
        };        
        selection_transform(selection, selection_scale_fnc);

        std::string snap_name; // Empty mean do not store on undo/redo stack
        m_parent.do_scale(snap_name);
        wxGetApp().obj_manipul()->set_dirty();
        // should be the almost same
        calculate_scale();
                
        const NSVGimage *img = m_volume_shape.svg_file->image.get();
        assert(img != NULL);
        if (img != NULL){
            NSVGLineParams params{get_tesselation_tolerance(get_scale_for_tolerance())};
            m_volume_shape.shapes_with_ids = create_shape_with_ids(*img, params);
            m_volume_shape.final_shape = {}; // reset cache for final shape
            if (!make_snap) // Be carefull: Last change may be without change of scale
                process(false);
        }
    }

    if (make_snap)
        process(); // make undo/redo snap-shot
}

void GLGizmoSVG::draw_use_surface() 
{
    bool can_use_surface = (m_volume->emboss_shape->projection.use_surface)? true : // already used surface must have option to uncheck
        !m_volume->is_the_only_one_part();
    m_imgui->disabled_begin(!can_use_surface);
    ScopeGuard sc([imgui = m_imgui]() { imgui->disabled_end(); });

    ImGui::AlignTextToFramePadding();
    ImGuiWrapper::text(m_gui_cfg->translations.use_surface);
    ImGui::SameLine(m_gui_cfg->input_offset);

    if (m_imgui->bbl_checkbox("##useSurface", m_volume_shape.projection.use_surface))
        process();
}

void GLGizmoSVG::draw_distance()
{
    const EmbossProjection& projection = m_volume->emboss_shape->projection;
    bool use_surface = projection.use_surface;
    bool allowe_surface_distance = !use_surface && !m_volume->is_the_only_one_part();

    float prev_distance = m_distance.value_or(.0f);
    float min_distance = static_cast<float>(-2 * projection.depth);
    float max_distance = static_cast<float>(2 * projection.depth);
 
    m_imgui->disabled_begin(!allowe_surface_distance);
    ScopeGuard sg([imgui = m_imgui]() { imgui->disabled_end(); });

    ImGui::AlignTextToFramePadding();
    ImVec4 text_color = m_distance.has_value() ? ImGuiWrapper::COL_MODIFIED : ImGui::GetStyleColorVec4(ImGuiCol_Text); // ORCA use modified color on text
    ImGuiWrapper::text_colored(text_color, m_gui_cfg->translations.distance);
    ImGui::SameLine(m_gui_cfg->input_offset);
    ImGui::SetNextItemWidth(m_gui_cfg->input_width);

    bool use_inch = wxGetApp().app_config->get_bool("use_inches");
    const wxString move_tooltip = _L("Distance of the center of the SVG to the model surface.");
    bool is_moved = false;
    if (use_inch) {
        std::optional<float> distance_inch;
        if (m_distance.has_value()) distance_inch = (*m_distance * GizmoObjectManipulation::mm_to_in);
        min_distance = static_cast<float>(min_distance * GizmoObjectManipulation::mm_to_in);
        max_distance = static_cast<float>(max_distance * GizmoObjectManipulation::mm_to_in);
        if (m_imgui->slider_optional_float("##distance", m_distance, min_distance, max_distance, "%.3f in", 1.f, false, move_tooltip)) {
            if (distance_inch.has_value()) {
                m_distance = *distance_inch * GizmoObjectManipulation::in_to_mm;
            } else {
                m_distance.reset();
            }
            is_moved = true;
        }
    } else {
        if (m_imgui->slider_optional_float("##distance", m_distance, min_distance, max_distance, "%.2f mm", 1.f, false, move_tooltip)) 
            is_moved = true;
    }
    bool is_stop_sliding = m_imgui->get_last_slider_status().deactivated_after_edit;
    bool is_reseted = false;
    if (m_distance.has_value()) {
        if (reset_button(m_icons)) {
            m_distance.reset();
            is_reseted = true;
        } else if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Reset distance"), m_gui_cfg->max_tooltip_width);
    }

    if (is_moved || is_reseted)
        do_local_z_move(m_parent.get_selection(), m_distance.value_or(.0f) - prev_distance);    
    if (is_stop_sliding || is_reseted)
        m_parent.do_move(move_snapshot_name);
}

void GLGizmoSVG::draw_rotation()
{
    ImGui::AlignTextToFramePadding();
    ImVec4 text_color = m_angle.has_value() ? ImGuiWrapper::COL_MODIFIED : ImGui::GetStyleColorVec4(ImGuiCol_Text); // ORCA use modified color on text
    ImGuiWrapper::text_colored(text_color, m_gui_cfg->translations.rotation);
    ImGui::SameLine(m_gui_cfg->input_offset);
    ImGui::SetNextItemWidth(m_gui_cfg->input_width);

    // slider for Clockwise angle in degress
    // stored angle is optional CCW and in radians
    // Convert stored value to degress
    // minus create clockwise roation from CCW
    float angle = m_angle.value_or(0.f);
    float angle_deg = static_cast<float>(-angle * 180 / M_PI);
    if (m_imgui->slider_float("##angle", &angle_deg, limits.angle.min, limits.angle.max, u8"%.2f °", 1.f, false, _L("Rotate text Clockwise."))){
        // convert back to radians and CCW
        double angle_rad = -angle_deg * M_PI / 180.0;
        Geometry::to_range_pi_pi(angle_rad);                

        double diff_angle = angle_rad - angle;
        
        do_local_z_rotate(m_parent.get_selection(), diff_angle);

        // calc angle after rotation
        m_angle = calc_angle(m_parent.get_selection());
        
        // recalculate for surface cut
        if (m_volume->emboss_shape->projection.use_surface)
            process();
    }
    bool is_stop_sliding = m_imgui->get_last_slider_status().deactivated_after_edit;

    // Reset button
    bool is_reseted = false;
    if (m_angle.has_value()) {
        if (reset_button(m_icons)) {
            do_local_z_rotate(m_parent.get_selection(), -(*m_angle));
            m_angle.reset();

            // recalculate for surface cut
            if (m_volume->emboss_shape->projection.use_surface)
                process();

            is_reseted = true;
        } else if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Reset rotation"), m_gui_cfg->max_tooltip_width);
    }

    // Apply rotation on model (backend)
    if (is_stop_sliding || is_reseted)
        m_parent.do_rotate(rotation_snapshot_name);    

    // Keep up - lock button icon
    if (!m_volume->is_the_only_one_part()) {
        ImGui::SameLine(m_gui_cfg->lock_offset);
        const IconManager::Icon &icon       = get_icon(m_icons, m_keep_up ? IconType::lock : IconType::unlock, IconState::activable);
        const IconManager::Icon &icon_hover = get_icon(m_icons, m_keep_up ? IconType::lock : IconType::unlock, IconState::hovered);
        if (button(icon, icon_hover, icon))
            m_keep_up = !m_keep_up;    
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_u8L("Lock/unlock rotation angle when dragging above the surface."), m_gui_cfg->max_tooltip_width);
    }
}

void GLGizmoSVG::draw_mirroring()
{
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", m_gui_cfg->translations.mirror.c_str());
    ImGui::SameLine(m_gui_cfg->input_offset);
    Axis axis = Axis::UNKNOWN_AXIS;
    if (draw_clickable(m_icons, IconType::reflection_x)) {
        axis = Axis::X;
    } else if (ImGui::IsItemHovered()) {
        m_imgui->tooltip(_u8L("Mirror vertically"), m_gui_cfg->max_tooltip_width);
    }

    ImGui::SameLine();
    if (draw_clickable(m_icons, IconType::reflection_y)) {
        axis = Axis::Y;
    } else if (ImGui::IsItemHovered()) {
        m_imgui->tooltip(_u8L("Mirror horizontally"), m_gui_cfg->max_tooltip_width);
    }

    if (axis != Axis::UNKNOWN_AXIS){
        Selection &selection = m_parent.get_selection();
        selection.setup_cache();

        auto selection_mirror_fnc = [&selection, &axis](){
            selection.mirror(axis, get_drag_transformation_type(selection));
        };
        selection_transform(selection, selection_mirror_fnc);
        m_parent.do_mirror(L("Set Mirror"));

        // Mirror is ignoring keep up !!
        if (m_keep_up)
            m_angle = calc_angle(selection);

        volume_transformation_changed();


        if (m_volume_shape.projection.use_surface)
            process();
    }
}

void GLGizmoSVG::draw_model_type()
{
    ImGui::AlignTextToFramePadding();
    bool is_last_solid_part = m_volume->is_the_only_one_part();
    std::string title = _u8L("Operation");
    if (is_last_solid_part) {
        ImVec4 color{.5f, .5f, .5f, 1.f};
        m_imgui->text_colored(color, title.c_str());
    } else {
        ImGui::Text("%s", title.c_str());
    }

    std::optional<ModelVolumeType> new_type;
    ModelVolumeType modifier = ModelVolumeType::PARAMETER_MODIFIER;
    ModelVolumeType negative = ModelVolumeType::NEGATIVE_VOLUME;
    ModelVolumeType part = ModelVolumeType::MODEL_PART;
    ModelVolumeType type = m_volume->type();

    //TRN EmbossOperation
    ImGuiWrapper::push_radio_style(m_parent.get_scale()); //ORCA
    if (ImGui::RadioButton(_u8L("Join").c_str(), type == part))
        new_type = part;
    else if (ImGui::IsItemHovered())
        m_imgui->tooltip(_u8L("Click to change text into object part."), m_gui_cfg->max_tooltip_width);
    ImGui::SameLine();

    std::string last_solid_part_hint = _u8L("You can't change a type of the last solid part of the object.");
    if (ImGui::RadioButton(_CTX_utf8(L_CONTEXT("Cut", "EmbossOperation"), "EmbossOperation").c_str(), type == negative))
        new_type = negative;
    else if (ImGui::IsItemHovered()) {
        if (is_last_solid_part)
            m_imgui->tooltip(last_solid_part_hint, m_gui_cfg->max_tooltip_width);
        else if (type != negative)
            m_imgui->tooltip(_u8L("Click to change part type into negative volume."), m_gui_cfg->max_tooltip_width);
    }

    // In simple mode are not modifiers
    if (wxGetApp().plater()->printer_technology() != ptSLA && wxGetApp().get_mode() != ConfigOptionMode::comSimple) {
        ImGui::SameLine();
        if (ImGui::RadioButton(_u8L("Modifier").c_str(), type == modifier))
            new_type = modifier;
        else if (ImGui::IsItemHovered()) {
            if (is_last_solid_part)
                m_imgui->tooltip(last_solid_part_hint, m_gui_cfg->max_tooltip_width);
            else if (type != modifier)
                m_imgui->tooltip(_u8L("Click to change part type into modifier."), m_gui_cfg->max_tooltip_width);
        }
    }
    ImGuiWrapper::pop_radio_style();

    if (m_volume != nullptr && new_type.has_value() && !is_last_solid_part) {
        GUI_App &app    = wxGetApp();
        Plater * plater = app.plater();
        // TRN: This is the name of the action that shows in undo/redo stack (changing part type from SVG to something else).
        Plater::TakeSnapshot snapshot(plater, _u8L("Change SVG Type"), UndoRedo::SnapshotType::GizmoAction);
        m_volume->set_type(*new_type);

        bool is_volume_move_inside  = (type == part);
        bool is_volume_move_outside = (*new_type == part);
         // Update volume position when switch (from part) or (into part)
        if ((is_volume_move_inside || is_volume_move_outside))
            process();

        // inspiration in ObjectList::change_part_type()
        // how to view correct side panel with objects
        ObjectList *obj_list = app.obj_list();
        wxDataViewItemArray sel = obj_list->reorder_volumes_and_get_selection(
            obj_list->get_selected_obj_idx(),
            [volume = m_volume](const ModelVolume *vol) { return vol == volume; });
        if (!sel.IsEmpty()) obj_list->select_item(sel.front());       

        // NOTE: on linux, function reorder_volumes_and_get_selection call GLCanvas3D::reload_scene(refresh_immediately = false)
        // which discard m_volume pointer and set it to nullptr also selection is cleared so gizmo is automaticaly closed
        auto &mng = m_parent.get_gizmos_manager();
        if (mng.get_current_type() != GLGizmosManager::Svg)
            mng.open_gizmo(GLGizmosManager::Svg);
        // TODO: select volume back - Ask @Sasa
    }
}


/////////////
// private namespace implementation
///////////////
namespace {

std::string get_file_name(const std::string &file_path)
{
    if (file_path.empty())
        return file_path;

    size_t pos_last_delimiter = file_path.find_last_of("/\\");
    if (pos_last_delimiter == std::string::npos) {
        // should not happend that in path is not delimiter
        assert(false);
        pos_last_delimiter = 0;
    }

    size_t pos_point = file_path.find_last_of('.');
    if (pos_point == std::string::npos || pos_point < pos_last_delimiter // last point is inside of directory path
    ) {
        // there is no extension
        assert(false);
        pos_point = file_path.size();
    }

    size_t offset = pos_last_delimiter + 1;             // result should not contain last delimiter ( +1 )
    size_t count  = pos_point - pos_last_delimiter - 1; // result should not contain extension point ( -1 )
    return file_path.substr(offset, count);
}

std::string volume_name(const EmbossShape &shape)
{
    std::string file_name = get_file_name(shape.svg_file->path);
    if (!file_name.empty())
        return file_name;
    return "SVG shape";
}

CreateVolumeParams create_input(GLCanvas3D &canvas, RaycastManager& raycaster, ModelVolumeType volume_type)
{
    auto gizmo = static_cast<unsigned char>(GLGizmosManager::Svg);
    const GLVolume *gl_volume = get_first_hovered_gl_volume(canvas);
    Plater *plater = wxGetApp().plater();
    return CreateVolumeParams{canvas, plater->get_camera(), plater->build_volume(),
        plater->get_ui_job_worker(), volume_type, raycaster, gizmo, gl_volume};
}

GuiCfg create_gui_configuration() {
    GuiCfg cfg; // initialize by default values;

    float line_height = ImGui::GetTextLineHeight();
    float line_height_with_spacing = ImGui::GetTextLineHeightWithSpacing();

    float space = line_height_with_spacing - line_height;

    cfg.icon_width = std::max(std::round(line_height/8)*8, 8.f);    

    GuiCfg::Translations &tr = cfg.translations;

    float lock_width = cfg.icon_width + 3 * space;
    // TRN - Input label. Be short as possible
    tr.depth       = _u8L("Depth");
    // TRN - Input label. Be short as possible
    tr.size        = _u8L("Size");
    // TRN - Input label. Be short as possible
    tr.use_surface = _u8L("Use surface");
    // TRN - Input label. Be short as possible
    tr.distance    = _u8L("From surface");
    // TRN - Input label. Be short as possible
    tr.rotation    = _u8L("Rotation");
    // TRN - Input label. Be short as possible
    tr.mirror      = _u8L("Mirror");
    float max_tr_width = std::max({
        ImGui::CalcTextSize(tr.depth.c_str()).x,
        ImGui::CalcTextSize(tr.size.c_str()).x + lock_width,
        ImGui::CalcTextSize(tr.use_surface.c_str()).x,
        ImGui::CalcTextSize(tr.distance.c_str()).x,
        ImGui::CalcTextSize(tr.rotation.c_str()).x + lock_width,
        ImGui::CalcTextSize(tr.mirror.c_str()).x,
    });

    const ImGuiStyle &style = ImGui::GetStyle();
    cfg.input_offset = style.WindowPadding.x + max_tr_width + space + cfg.icon_width;
    cfg.lock_offset = cfg.input_offset - (cfg.icon_width + 2 * space);

    ImVec2 letter_m_size = ImGui::CalcTextSize("M");
    const float count_letter_M_in_input = 12.f;
    cfg.input_width = letter_m_size.x * count_letter_M_in_input;
    cfg.texture_max_size_px = std::round((cfg.input_width + cfg.input_offset + cfg.icon_width + space)/8) * 8;

    cfg.max_tooltip_width = ImGui::GetFontSize() * 20.0f;

    return cfg;
}

std::string choose_svg_file()
{
    wxWindow *parent = nullptr;
    wxString message = _L("Choose SVG file for emboss:");
    wxString selected_file = wxEmptyString;
    wxString wildcard = file_wildcards(FT_SVG);
    long style = wxFD_OPEN | wxFD_FILE_MUST_EXIST;
    wxFileDialog dialog(parent, message, last_used_directory, selected_file, wildcard, style);
    if (dialog.ShowModal() != wxID_OK) {
        BOOST_LOG_TRIVIAL(warning) << "SVG file for emboss was NOT selected.";
        return {};
    }

    wxArrayString input_files;
    dialog.GetPaths(input_files);
    if (input_files.IsEmpty()) {
        BOOST_LOG_TRIVIAL(warning) << "SVG file dialog result is empty.";
        return {};
    }

    if (input_files.size() != 1)
        BOOST_LOG_TRIVIAL(warning) << "SVG file dialog result contain multiple files but only first is used.";

    std::string path = into_u8(input_files.front());
    if (!boost::filesystem::exists(path)) {
        BOOST_LOG_TRIVIAL(warning) << "SVG file dialog return invalid path.";
        return {};
    }

    if (!boost::algorithm::iends_with(path, ".svg")) {
        BOOST_LOG_TRIVIAL(warning) << "SVG file dialog return path without '.svg' tail";
        return {};    
    }

    last_used_directory = dialog.GetDirectory();
    return path;
}

EmbossShape select_shape(std::string_view filepath, double tesselation_tolerance)
{
    EmbossShape shape;
    shape.projection.depth       = 10.;
    shape.projection.use_surface = false;
    
    EmbossShape::SvgFile svg;
    if (filepath.empty()) {
        // When empty open file dialog
        svg.path = choose_svg_file();
        if (svg.path.empty())            
            return {}; // file was not selected
    } else {
        svg.path = filepath; // copy
    }
    

    boost::filesystem::path path(svg.path);
    if (!boost::filesystem::exists(path)) {
        show_error(nullptr, GUI::format(_u8L("File does NOT exist (%1%)."), svg.path));
        return {};
    }

    if (!boost::algorithm::iends_with(svg.path, ".svg")) {
        show_error(nullptr, GUI::format(_u8L("Filename has to end with \".svg\" but you selected %1%"), svg.path));
        return {};
    }

    if(init_image(svg) == nullptr) {
        show_error(nullptr, GUI::format(_u8L("Nano SVG parser can't load from file (%1%)."), svg.path));
        return {};
    }

    // Set default and unchanging scale
    NSVGLineParams params{tesselation_tolerance};
    shape.shapes_with_ids = create_shape_with_ids(*svg.image, params);

    // Must contain some shapes !!!
    if (shape.shapes_with_ids.empty()) {
        show_error(nullptr, GUI::format(_u8L("SVG file does NOT contain a single path to be embossed (%1%)."), svg.path));
        return {};
    }
    shape.svg_file = std::move(svg);
    return shape;
}

DataBasePtr create_emboss_data_base(std::shared_ptr<std::atomic<bool>> &cancel, ModelVolumeType volume_type, std::string_view filepath)
{
    EmbossShape shape = select_shape(filepath);

    if (shape.shapes_with_ids.empty())
        // canceled selection of SVG file
        return nullptr;

    // Cancel previous Job, when it is in process
    // worker.cancel(); --> Use less in this case I want cancel only previous EmbossJob no other jobs
    // Cancel only EmbossUpdateJob no others
    if (cancel != nullptr)
        cancel->store(true);
    // create new shared ptr to cancel new job
    cancel = std::make_shared<std::atomic<bool>>(false);

    std::string name = volume_name(shape);

    auto result = std::make_unique<DataBase>(name, cancel /*copy*/, std::move(shape));
    result->is_outside = volume_type == ModelVolumeType::MODEL_PART;
    return result;
}
} // namespace

// any existing icon filename to not influence GUI
const std::string GLGizmoSVG::M_ICON_FILENAME = "cut.svg";
