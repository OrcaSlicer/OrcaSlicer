#include "libslic3r/libslic3r.h"
#include "GLCanvas3D.hpp"

#include <igl/unproject.h>

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Technologies.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "3DScene.hpp"
#include "BackgroundSlicingProcess.hpp"
#include "GLShader.hpp"
#include "GUI.hpp"
#include "Tab.hpp"
#include "GUI_Preview.hpp"
#include "OpenGLManager.hpp"
#include "Plater.hpp"
#include "MainFrame.hpp"
#include "WipeTowerDialog.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "GUI_Colors.hpp"
#include "Mouse3DController.hpp"
#include "I18N.hpp"
#include "NotificationManager.hpp"
#include "format.hpp"
#include "DailyTips.hpp"
#include "FilamentMapDialog.hpp"

#include "slic3r/GUI/Gizmos/GLGizmoPainterBase.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "slic3r/Utils/MacDarkMode.hpp"

#include <slic3r/GUI/GUI_Utils.hpp>

#if ENABLE_RETINA_GL
#include "slic3r/Utils/RetinaHelper.hpp"
#endif

#include <GL/glew.h>

#include <wx/glcanvas.h>
#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/settings.h>
#include <wx/tooltip.h>
#include <wx/debug.h>
#include <wx/fontutil.h>
// Print now includes tbb, and tbb includes Windows. This breaks compilation of wxWidgets if included before wx.
#include "libslic3r/Print.hpp"
#include "libslic3r/SLAPrint.hpp"

#include "wxExtensions.hpp"

#include <tbb/parallel_for.h>
#include <tbb/spin_mutex.h>

#include <boost/log/trivial.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <iostream>
#include <float.h>
#include <algorithm>
#include <cmath>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

#include <imguizmo/ImGuizmo.h>

static constexpr const float TRACKBALLSIZE = 0.8f;

static Slic3r::ColorRGBA DEFAULT_BG_LIGHT_COLOR      = { 0.906f, 0.906f, 0.906f, 1.0f };
static Slic3r::ColorRGBA DEFAULT_BG_LIGHT_COLOR_DARK = { 0.329f, 0.329f, 0.353f, 1.0f };
static Slic3r::ColorRGBA ERROR_BG_LIGHT_COLOR        = { 0.753f, 0.192f, 0.039f, 1.0f };
static Slic3r::ColorRGBA ERROR_BG_LIGHT_COLOR_DARK   = { 0.753f, 0.192f, 0.039f, 1.0f };

void GLCanvas3D::update_render_colors()
{
    DEFAULT_BG_LIGHT_COLOR = ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_3D_Background]);
}

void GLCanvas3D::load_render_colors()
{
    RenderColor::colors[RenderCol_3D_Background] = ImGuiWrapper::to_ImVec4(DEFAULT_BG_LIGHT_COLOR);
}

//static constexpr const float AXES_COLOR[3][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };

// Number of floats
static constexpr const size_t MAX_VERTEX_BUFFER_SIZE     = 131072 * 6; // 3.15MB

namespace Slic3r {
namespace GUI {

#ifdef __WXGTK3__
// wxGTK3 seems to simulate OSX behavior in regard to HiDPI scaling support.
RetinaHelper::RetinaHelper(wxWindow* window) : m_window(window), m_self(nullptr) {}
RetinaHelper::~RetinaHelper() {}
float RetinaHelper::get_scale_factor() { return float(m_window->GetContentScaleFactor()); }
#endif // __WXGTK3__

// Fixed the collision between BuildVolume_Type::Convex and macro Convex defined inside /usr/include/X11/X.h that is included by WxWidgets 3.0.
#if defined(__linux__) && defined(Convex)
#undef Convex
#endif


std::string& get_object_limited_text() {
    static std::string object_limited_text = _u8L("An object is placed in the left/right nozzle-only area or exceeds the printable height of the left nozzle.\n"
            "Please ensure the filaments used by this object are not arranged to other nozzles.");
    return object_limited_text;
}

std::string& get_object_clashed_text() {
    static std::string object_clashed_text = _u8L("An object is laid over the boundary of plate or exceeds the height limit.\n"
            "Please solve the problem by moving it totally on or off the plate, and confirming that the height is within the build volume.");
    return object_clashed_text;
}

std::string& get_left_extruder_unprintable_text() {
    static std::string left_unprintable_text;
    return left_unprintable_text;
}

std::string& get_right_extruder_unprintable_text() {
    static std::string right_unprintable_text;
    return right_unprintable_text;
}

std::string& get_nozzle_filament_incompatible_text() {
    static std::string nozzle_filament_incompatible_text;
    return nozzle_filament_incompatible_text;
}

std::string& get_filament_mixture_warning_text(){
    static std::string filament_mixture_warning_text;
    return filament_mixture_warning_text;
}


static std::string format_number(float value)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision((static_cast<int>(value * 10) % 10 == 0) ? 0 : 1) << value;
    return oss.str();
}

wxString filament_printable_error_msg;

GLCanvas3D::LayersEditing::~LayersEditing()
{
    if (m_z_texture_id != 0) {
        glsafe(::glDeleteTextures(1, &m_z_texture_id));
        m_z_texture_id = 0;
    }
    delete m_slicing_parameters;
}

const float GLCanvas3D::LayersEditing::THICKNESS_BAR_WIDTH = 70.0f;

void GLCanvas3D::LayersEditing::init()
{
    glsafe(::glGenTextures(1, (GLuint*)&m_z_texture_id));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_z_texture_id));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
}

void GLCanvas3D::LayersEditing::set_config(const DynamicPrintConfig* config)
{
    m_config = config;
    delete m_slicing_parameters;
    m_slicing_parameters = nullptr;
    m_layers_texture.valid = false;
    m_layer_height_profile.clear();
}

void GLCanvas3D::LayersEditing::select_object(const Model& model, int object_id)
{
    const ModelObject* model_object_new = (object_id >= 0) ? model.objects[object_id] : nullptr;
    // Maximum height of an object changes when the object gets rotated or scaled.
    // Changing maximum height of an object will invalidate the layer heigth editing profile.
    // m_model_object->bounding_box() is cached, therefore it is cheap even if this method is called frequently.
    const float new_max_z = (model_object_new == nullptr) ? 0.0f : static_cast<float>(model_object_new->max_z());

    if (m_model_object != model_object_new || this->last_object_id != object_id || m_object_max_z != new_max_z ||
        (model_object_new != nullptr && m_model_object->id() != model_object_new->id())) {
        m_layer_height_profile.clear();
        delete m_slicing_parameters;
        m_slicing_parameters = nullptr;
        m_layers_texture.valid = false;
        this->last_object_id = object_id;
        m_model_object = model_object_new;
        m_object_max_z = new_max_z;
    }
}

bool GLCanvas3D::LayersEditing::is_allowed() const
{
    return wxGetApp().get_shader("variable_layer_height") != nullptr && m_z_texture_id > 0;
}

bool GLCanvas3D::LayersEditing::is_enabled() const
{
    return m_enabled;
}

void GLCanvas3D::LayersEditing::set_enabled(bool enabled)
{
    m_enabled = is_allowed() && enabled;
}

float GLCanvas3D::LayersEditing::s_overlay_window_width;

void GLCanvas3D::LayersEditing::show_tooltip_information(const GLCanvas3D& canvas, std::map<wxString, wxString> captions_texts, float x, float y)
{
    ImTextureID normal_id = canvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id = canvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    float caption_max = 0.f;
    for (auto caption_text : captions_texts) {
        caption_max = std::max(imgui.calc_text_size(caption_text.first).x, caption_max);
    }
    caption_max += GImGui->Style.WindowPadding.x + imgui.scaled(1);

	float  scale       = canvas.get_scale();
    #ifdef WIN32
        int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
        scale *= (float) dpi / (float) DPI_DEFAULT;
    #endif // WIN32
    ImVec2 button_size = ImVec2(25 * scale, 25 * scale); // ORCA: Use exact resolution will prevent blur on icon
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, 0}); // ORCA: Dont add padding
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &caption_max, &imgui](const wxString& caption, const wxString& text) {
            imgui.text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            imgui.text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };

        for (const auto& caption_text : captions_texts) draw_text_with_caption(caption_text.first, caption_text.second);

        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

void GLCanvas3D::LayersEditing::render_variable_layer_height_dialog(const GLCanvas3D& canvas) {
    if (!m_enabled)
        return;

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    const Size& cnv_size = canvas.get_canvas_size();
    float left_pos = canvas.m_main_toolbar.get_item("layersediting")->render_left_pos;
    float x = (1 + left_pos) * cnv_size.get_width() / 2;

    const auto canvas_width = cnv_size.get_width();
    if (abs(GLCanvas3D::LayersEditing::s_overlay_window_width) > 0.01f) {
        if (x + GLCanvas3D::LayersEditing::s_overlay_window_width > canvas_width) {
            if (GLCanvas3D::LayersEditing::s_overlay_window_width > canvas_width)
                x = 0;
            else
                x = canvas_width - GLCanvas3D::LayersEditing::s_overlay_window_width;
        }
    }
    x = std::max(x, 0.0f);

    imgui.set_next_window_pos(x, canvas.m_main_toolbar.get_height(), ImGuiCond_Always, 0.0f, 0.0f);

    imgui.push_toolbar_style(canvas.get_scale());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * canvas.get_scale(), 4.0f * canvas.get_scale()));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * canvas.get_scale(), 10.0f * canvas.get_scale()));
    imgui.begin(_L("Variable layer height"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    const float sliders_width = imgui.scaled(7.0f);
    const float input_box_width = 1.5 * imgui.get_slider_icon_size().x;

    if (imgui.button(_L("Adaptive")))
        wxPostEvent((wxEvtHandler*)canvas.get_wxglcanvas(), Event<float>(EVT_GLCANVAS_ADAPTIVE_LAYER_HEIGHT_PROFILE, m_adaptive_quality));
    ImGui::SameLine();
    static float text_align = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    text_align = std::max(text_align, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(text_align);
    imgui.text(_L("Quality / Speed"));
    if (ImGui::IsItemHovered()) {
        //ImGui::BeginTooltip();
        //ImGui::TextUnformatted(_L("Higher print quality versus higher print speed.").ToUTF8());
        //ImGui::EndTooltip();
    }
    ImGui::SameLine();
    static float slider_align = ImGui::GetCursorPosX();
    ImGui::PushItemWidth(sliders_width);
    m_adaptive_quality = std::clamp(m_adaptive_quality, 0.0f, 1.f);
    slider_align = std::max(slider_align, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(slider_align);
    imgui.bbl_slider_float_style("##adaptive_slider", &m_adaptive_quality, 0.0f, 1.f, "%.2f");
    ImGui::SameLine();
    static float input_align = ImGui::GetCursorPosX();
    ImGui::PushItemWidth(input_box_width);
    input_align = std::max(input_align, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(input_align);
    ImGui::BBLDragFloat("##adaptive_input", &m_adaptive_quality, 0.05f, 0.0f, 0.0f, "%.2f");

    if (imgui.button(_L("Smooth")))
        wxPostEvent((wxEvtHandler*)canvas.get_wxglcanvas(), HeightProfileSmoothEvent(EVT_GLCANVAS_SMOOTH_LAYER_HEIGHT_PROFILE, m_smooth_params));
    ImGui::SameLine();
    text_align = std::max(text_align, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(text_align);
    ImGui::AlignTextToFramePadding();
    imgui.text(_L("Radius"));
    ImGui::SameLine();
    slider_align = std::max(slider_align, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(slider_align);
    ImGui::PushItemWidth(sliders_width);
    int radius = (int)m_smooth_params.radius;
    int v_min = 1, v_max = 10;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 0.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(238 / 255.0f, 238 / 255.0f, 238 / 255.0f, 0.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.81f, 0.81f, 0.81f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));
    if(ImGui::BBLSliderScalar("##radius_slider", ImGuiDataType_S32, &radius, &v_min, &v_max)){
        radius = std::clamp(radius, 1, 10);
        m_smooth_params.radius = (unsigned int)radius;
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::SameLine();
    input_align = std::max(input_align, ImGui::GetCursorPosX());
    ImGui::SetCursorPosX(input_align);
    ImGui::PushItemWidth(input_box_width);
    ImGui::PushStyleColor(ImGuiCol_BorderActive, ImVec4(0.00f, 0.59f, 0.53f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.00f, 0.59f, 0.53f, 0.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.00f, 0.59f, 0.53f, 0.00f));
    ImGui::BBLDragScalar("##radius_input", ImGuiDataType_S32, &radius, 1, &v_min, &v_max);
    ImGui::PopStyleColor(3);

    imgui.bbl_checkbox("##keep_min", m_smooth_params.keep_min);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    imgui.text(_L("Keep min"));

    ImGui::Separator();

    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + canvas.m_main_toolbar.get_height();
    std::map<wxString, wxString> captions_texts = {
        {_L("Left mouse button") + ":" , _L("Add detail")},
        {_L("Right mouse button") + ":", _L("Remove detail")},
        {_L("Shift+") + _L("Left mouse button") + ":", _L("Reset to base")},
        {_L("Shift+") + _L("Right mouse button") + ":", _L("Smoothing")},
        {_L("Mouse wheel:"), _L("Increase/decrease edit area")}
    };
    show_tooltip_information(canvas, captions_texts, x, get_cur_y);
    ImGui::SameLine();
    if (imgui.button(_L("Reset")))
        wxPostEvent((wxEvtHandler*)canvas.get_wxglcanvas(), SimpleEvent(EVT_GLCANVAS_RESET_LAYER_HEIGHT_PROFILE));

    GLCanvas3D::LayersEditing::s_overlay_window_width = ImGui::GetWindowSize().x;
    imgui.end();
    ImGui::PopStyleVar(2);
    imgui.pop_toolbar_style();
}

void GLCanvas3D::LayersEditing::render_overlay(const GLCanvas3D& canvas)
{
    render_variable_layer_height_dialog(canvas);
    render_active_object_annotations(canvas);
    render_profile(canvas);
}

float GLCanvas3D::LayersEditing::get_cursor_z_relative(const GLCanvas3D& canvas)
{
    const Vec2d mouse_pos = canvas.get_local_mouse_position();
    const Rect& rect = get_bar_rect_screen(canvas);
    float x = (float)mouse_pos.x();
    float y = (float)mouse_pos.y();
    float t = rect.get_top();
    float b = rect.get_bottom();

    return (rect.get_left() <= x && x <= rect.get_right() && t <= y && y <= b) ?
        // Inside the bar.
        (b - y - 1.0f) / (b - t - 1.0f) :
        // Outside the bar.
        -1000.0f;
}

bool GLCanvas3D::LayersEditing::bar_rect_contains(const GLCanvas3D& canvas, float x, float y)
{
    const Rect& rect = get_bar_rect_screen(canvas);
    return rect.get_left() <= x && x <= rect.get_right() && rect.get_top() <= y && y <= rect.get_bottom();
}

Rect GLCanvas3D::LayersEditing::get_bar_rect_screen(const GLCanvas3D& canvas)
{
    const Size& cnv_size = canvas.get_canvas_size();
    float w = (float)cnv_size.get_width();
    float h = (float)cnv_size.get_height();

    return { w - thickness_bar_width(canvas), 0.0f, w, h };
}

bool GLCanvas3D::LayersEditing::is_initialized() const
{
    return wxGetApp().get_shader("variable_layer_height") != nullptr;
}

std::string GLCanvas3D::LayersEditing::get_tooltip(const GLCanvas3D& canvas) const
{
    std::string ret;
    if (m_enabled && m_layer_height_profile.size() >= 4) {
        float z = get_cursor_z_relative(canvas);
        if (z != -1000.0f) {
            z *= m_object_max_z;

            float h = 0.0f;
            for (size_t i = m_layer_height_profile.size() - 2; i >= 2; i -= 2) {
                const float zi = static_cast<float>(m_layer_height_profile[i]);
                const float zi_1 = static_cast<float>(m_layer_height_profile[i - 2]);
                if (zi_1 <= z && z <= zi) {
                    float dz = zi - zi_1;
                    h = (dz != 0.0f) ? static_cast<float>(lerp(m_layer_height_profile[i - 1], m_layer_height_profile[i + 1], (z - zi_1) / dz)) :
                        static_cast<float>(m_layer_height_profile[i + 1]);
                    break;
                }
            }
            if (h > 0.0f)
                ret = wxString::Format("%.3f",h).ToStdString();
        }
    }
    return ret;
}

void GLCanvas3D::LayersEditing::render_active_object_annotations(const GLCanvas3D& canvas)
{
    if (!m_enabled)
        return;

    const Size cnv_size = canvas.get_canvas_size();
    const float cnv_width  = (float)cnv_size.get_width();
    const float cnv_height = (float)cnv_size.get_height();
    if (cnv_width == 0.0f || cnv_height == 0.0f)
        return;

    const float cnv_inv_width = 1.0f / cnv_width;

    GLShaderProgram* shader = wxGetApp().get_shader("variable_layer_height");
    if (shader == nullptr)
        return;

    shader->start_using();

    shader->set_uniform("z_to_texture_row", float(m_layers_texture.cells - 1) / (float(m_layers_texture.width) * m_object_max_z));
    shader->set_uniform("z_texture_row_to_normalized", 1.0f / (float)m_layers_texture.height);
    shader->set_uniform("z_cursor", m_object_max_z * this->get_cursor_z_relative(canvas));
    shader->set_uniform("z_cursor_band_width", band_width);
    shader->set_uniform("object_max_z", m_object_max_z);
    shader->set_uniform("view_model_matrix", Transform3d::Identity());
    shader->set_uniform("projection_matrix", Transform3d::Identity());
    shader->set_uniform("view_normal_matrix", (Matrix3d)Matrix3d::Identity());

    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_z_texture_id));

    // Render the color bar
    if (!m_profile.background.is_initialized() || m_profile.old_canvas_width != cnv_width) {
        m_profile.old_canvas_width = cnv_width;
        m_profile.background.reset();

        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3T2 };
        init_data.reserve_vertices(4);
        init_data.reserve_indices(6);

        // vertices
        const float l = 1.0f - 2.0f * THICKNESS_BAR_WIDTH * cnv_inv_width;
        const float r = 1.0f;
        const float t = 1.0f;
        const float b = -1.0f;
        init_data.add_vertex(Vec3f(l, b, 0.0f), Vec3f::UnitZ(), Vec2f(0.0f, 0.0f));
        init_data.add_vertex(Vec3f(r, b, 0.0f), Vec3f::UnitZ(), Vec2f(1.0f, 0.0f));
        init_data.add_vertex(Vec3f(r, t, 0.0f), Vec3f::UnitZ(), Vec2f(1.0f, 1.0f));
        init_data.add_vertex(Vec3f(l, t, 0.0f), Vec3f::UnitZ(), Vec2f(0.0f, 1.0f));

        // indices
        init_data.add_triangle(0, 1, 2);
        init_data.add_triangle(2, 3, 0);

        m_profile.background.init_from(std::move(init_data));
    }

    m_profile.background.render();

    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    shader->stop_using();
}

void GLCanvas3D::LayersEditing::render_profile(const GLCanvas3D& canvas)
{
    if (!m_enabled)
        return;

    //FIXME show some kind of legend.

    if (!m_slicing_parameters)
        return;

    const Size cnv_size = canvas.get_canvas_size();
    const float cnv_width  = (float)cnv_size.get_width();
    const float cnv_height = (float)cnv_size.get_height();
    if (cnv_width == 0.0f || cnv_height == 0.0f)
        return;

    // Make the vertical bar a bit wider so the layer height curve does not touch the edge of the bar region.
    const float scale_x = THICKNESS_BAR_WIDTH / float(1.12 * m_slicing_parameters->max_layer_height);
    const float scale_y = cnv_height / m_object_max_z;

    const float cnv_inv_width  = 1.0f / cnv_width;
    const float cnv_inv_height = 1.0f / cnv_height;

    // Baseline
    if (!m_profile.baseline.is_initialized() || m_profile.old_layer_height_profile != m_layer_height_profile) {
        m_profile.baseline.reset();

        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P2};
        init_data.color = ColorRGBA::BLACK();
        init_data.reserve_vertices(2);
        init_data.reserve_indices(2);

        // vertices
        const float axis_x = 2.0f * ((cnv_width - THICKNESS_BAR_WIDTH + float(m_slicing_parameters->layer_height) * scale_x) * cnv_inv_width - 0.5f);
        init_data.add_vertex(Vec2f(axis_x, -1.0f));
        init_data.add_vertex(Vec2f(axis_x, 1.0f));

        // indices
        init_data.add_line(0, 1);

        m_profile.baseline.init_from(std::move(init_data));
    }

    if (!m_profile.profile.is_initialized() || m_profile.old_layer_height_profile != m_layer_height_profile) {
        m_profile.old_layer_height_profile = m_layer_height_profile;
        m_profile.profile.reset();

        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::LineStrip, GLModel::Geometry::EVertexLayout::P2 };
        init_data.color = ColorRGBA::BLUE();
        init_data.reserve_vertices(m_layer_height_profile.size() / 2);
        init_data.reserve_indices(m_layer_height_profile.size() / 2);

        // vertices + indices
        for (unsigned int i = 0; i < (unsigned int)m_layer_height_profile.size(); i += 2) {
            init_data.add_vertex(Vec2f(2.0f * ((cnv_width - THICKNESS_BAR_WIDTH + float(m_layer_height_profile[i + 1]) * scale_x) * cnv_inv_width - 0.5f),
                                       2.0f * (float(m_layer_height_profile[i]) * scale_y * cnv_inv_height - 0.5)));
            init_data.add_index(i / 2);
        }

        m_profile.profile.init_from(std::move(init_data));
    }

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader != nullptr) {
        shader->start_using();
        shader->set_uniform("view_model_matrix", Transform3d::Identity());
        shader->set_uniform("projection_matrix", Transform3d::Identity());
        m_profile.baseline.render();
        m_profile.profile.render();
        shader->stop_using();
    }
}

void GLCanvas3D::LayersEditing::render_volumes(const GLCanvas3D& canvas, const GLVolumeCollection& volumes)
{
    assert(this->is_allowed());
    assert(this->last_object_id != -1);

    GLShaderProgram* current_shader = wxGetApp().get_current_shader();
    ScopeGuard guard([current_shader]() { if (current_shader != nullptr) current_shader->start_using(); });
    if (current_shader != nullptr)
        current_shader->stop_using();

    GLShaderProgram* shader = wxGetApp().get_shader("variable_layer_height");
    if (shader == nullptr)
        return;

    shader->start_using();

    generate_layer_height_texture();

    // Uniforms were resolved, go ahead using the layer editing shader.
    shader->set_uniform("z_to_texture_row", float(m_layers_texture.cells - 1) / (float(m_layers_texture.width) * float(m_object_max_z)));
    shader->set_uniform("z_texture_row_to_normalized", 1.0f / float(m_layers_texture.height));
    shader->set_uniform("z_cursor", float(m_object_max_z) * float(this->get_cursor_z_relative(canvas)));
    shader->set_uniform("z_cursor_band_width", float(this->band_width));

    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    // Initialize the layer height texture mapping.
    const GLsizei w = (GLsizei)m_layers_texture.width;
    const GLsizei h = (GLsizei)m_layers_texture.height;
    const GLsizei half_w = w / 2;
    const GLsizei half_h = h / 2;
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_z_texture_id));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, half_w, half_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0));
    glsafe(::glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, m_layers_texture.data.data()));
    glsafe(::glTexSubImage2D(GL_TEXTURE_2D, 1, 0, 0, half_w, half_h, GL_RGBA, GL_UNSIGNED_BYTE, m_layers_texture.data.data() + m_layers_texture.width * m_layers_texture.height * 4));
    for (GLVolume* glvolume : volumes.volumes) {
        // Render the object using the layer editing shader and texture.
        if (!glvolume->is_active || glvolume->composite_id.object_id != this->last_object_id || glvolume->is_modifier)
            continue;

        shader->set_uniform("volume_world_matrix", glvolume->world_matrix());
        shader->set_uniform("object_max_z", 0.0f);
        const Transform3d& view_matrix = camera.get_view_matrix();
        const Transform3d model_matrix = glvolume->world_matrix();
        shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
        const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);

        glvolume->render();
    }
    // Revert back to the previous shader.
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GLCanvas3D::LayersEditing::adjust_layer_height_profile()
{
    this->update_slicing_parameters();
    PrintObject::update_layer_height_profile(*m_model_object, *m_slicing_parameters, m_layer_height_profile);
    Slic3r::adjust_layer_height_profile(*m_model_object, *m_slicing_parameters, m_layer_height_profile, this->last_z, this->strength, this->band_width, this->last_action);
    m_layers_texture.valid = false;
}

void GLCanvas3D::LayersEditing::reset_layer_height_profile(GLCanvas3D & canvas)
{
    const_cast<ModelObject*>(m_model_object)->layer_height_profile.clear();
    m_layer_height_profile.clear();
    m_layers_texture.valid = false;
    canvas.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    wxGetApp().obj_list()->update_info_items(last_object_id);
}

void GLCanvas3D::LayersEditing::adaptive_layer_height_profile(GLCanvas3D & canvas, float quality_factor)
{
    this->update_slicing_parameters();
    m_layer_height_profile = layer_height_profile_adaptive(*m_slicing_parameters, *m_model_object, quality_factor);
    const_cast<ModelObject*>(m_model_object)->layer_height_profile.set(m_layer_height_profile);
    m_layers_texture.valid = false;
    canvas.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    wxGetApp().obj_list()->update_info_items(last_object_id);
}

void GLCanvas3D::LayersEditing::smooth_layer_height_profile(GLCanvas3D & canvas, const HeightProfileSmoothingParams & smoothing_params)
{
    this->update_slicing_parameters();
    m_layer_height_profile = smooth_height_profile(m_layer_height_profile, *m_slicing_parameters, smoothing_params);
    const_cast<ModelObject*>(m_model_object)->layer_height_profile.set(m_layer_height_profile);
    m_layers_texture.valid = false;
    canvas.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    wxGetApp().obj_list()->update_info_items(last_object_id);
}

void GLCanvas3D::LayersEditing::generate_layer_height_texture()
{
    this->update_slicing_parameters();
    // Always try to update the layer height profile.
    bool update = !m_layers_texture.valid;
    if (PrintObject::update_layer_height_profile(*m_model_object, *m_slicing_parameters, m_layer_height_profile)) {
        // Initialized to the default value.
        update = true;
    }
    // Update if the layer height profile was changed, or when the texture is not valid.
    if (!update && !m_layers_texture.data.empty() && m_layers_texture.cells > 0)
        // Texture is valid, don't update.
        return;

    if (m_layers_texture.data.empty()) {
        m_layers_texture.width = 1024;
        m_layers_texture.height = 1024;
        m_layers_texture.levels = 2;
        m_layers_texture.data.assign(m_layers_texture.width * m_layers_texture.height * 5, 0);
    }

    bool level_of_detail_2nd_level = true;
    m_layers_texture.cells = Slic3r::generate_layer_height_texture(
        *m_slicing_parameters,
        Slic3r::generate_object_layers(*m_slicing_parameters, m_layer_height_profile, false),
        m_layers_texture.data.data(), m_layers_texture.height, m_layers_texture.width, level_of_detail_2nd_level);
    m_layers_texture.valid = true;
}

void GLCanvas3D::LayersEditing::accept_changes(GLCanvas3D & canvas)
{
    if (last_object_id >= 0) {
        wxGetApp().plater()->take_snapshot("Variable layer height - Manual edit");
        const_cast<ModelObject*>(m_model_object)->layer_height_profile.set(m_layer_height_profile);
        canvas.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        wxGetApp().obj_list()->update_info_items(last_object_id);
    }
}

void GLCanvas3D::LayersEditing::update_slicing_parameters()
{
    if (m_slicing_parameters == nullptr) {
        m_slicing_parameters = new SlicingParameters();
        *m_slicing_parameters = PrintObject::slicing_parameters(*m_config, *m_model_object, m_object_max_z, m_shrinkage_compensation);
    }

}

float GLCanvas3D::LayersEditing::thickness_bar_width(const GLCanvas3D & canvas)
{
    return
#if ENABLE_RETINA_GL
        canvas.get_canvas_size().get_scale_factor()
#else
        canvas.get_wxglcanvas()->GetContentScaleFactor()
#endif
        * THICKNESS_BAR_WIDTH;
}

const Point GLCanvas3D::Mouse::Drag::Invalid_2D_Point(INT_MAX, INT_MAX);
const Vec3d GLCanvas3D::Mouse::Drag::Invalid_3D_Point(DBL_MAX, DBL_MAX, DBL_MAX);
const int GLCanvas3D::Mouse::Drag::MoveThresholdPx = 5;

GLCanvas3D::Mouse::Drag::Drag()
    : start_position_2D(Invalid_2D_Point)
    , start_position_3D(Invalid_3D_Point)
    , move_volume_idx(-1)
    , move_requires_threshold(false)
    , move_start_threshold_position_2D(Invalid_2D_Point)
{
}

GLCanvas3D::Mouse::Mouse()
    : dragging(false)
    , position(DBL_MAX, DBL_MAX)
    , scene_position(DBL_MAX, DBL_MAX, DBL_MAX)
    , ignore_left_up(false)
    , ignore_right_up(false)
{
}

void GLCanvas3D::Labels::render(const std::vector<const ModelInstance*>& sorted_instances) const
{
    if (!m_enabled || !is_shown() || m_canvas.get_gizmos_manager().is_running())
        return;

    const Camera& camera = wxGetApp().plater()->get_camera();
    const Model* model = m_canvas.get_model();
    if (model == nullptr)
        return;

    Transform3d world_to_eye = camera.get_view_matrix();
    Transform3d world_to_screen = camera.get_projection_matrix() * world_to_eye;
    const std::array<int, 4>& viewport = camera.get_viewport();

    struct Owner
    {
        int obj_idx;
        int inst_idx;
        size_t model_instance_id;
        BoundingBoxf3 world_box;
        double eye_center_z;
        std::string title;
        std::string label;
        std::string print_order;
        bool selected;
    };

    // collect owners world bounding boxes and data from volumes
    std::vector<Owner> owners;
    const GLVolumeCollection& volumes = m_canvas.get_volumes();
    PartPlate* cur_plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    for (const GLVolume* volume : volumes.volumes) {
        int obj_idx = volume->object_idx();
        if (0 <= obj_idx && obj_idx < (int)model->objects.size()) {
            int inst_idx = volume->instance_idx();
            //only show current plate's label
            if (!cur_plate->contain_instance(obj_idx, inst_idx))
                continue;
            std::vector<Owner>::iterator it = std::find_if(owners.begin(), owners.end(), [obj_idx, inst_idx](const Owner& owner) {
                return (owner.obj_idx == obj_idx) && (owner.inst_idx == inst_idx);
                });
            if (it != owners.end()) {
                it->world_box.merge(volume->transformed_bounding_box());
                it->selected &= volume->selected;
            } else {
                const ModelObject* model_object = model->objects[obj_idx];
                Owner owner;
                owner.obj_idx = obj_idx;
                owner.inst_idx = inst_idx;
                owner.model_instance_id = model_object->instances[inst_idx]->id().id;
                owner.world_box = volume->transformed_bounding_box();
                owner.title = "object" + std::to_string(obj_idx) + "_inst##" + std::to_string(inst_idx);
                owner.label = model_object->name;
                if (model_object->instances.size() > 1)
                    owner.label += " (" + std::to_string(inst_idx + 1) + ")";
                owner.selected = volume->selected;
                owners.emplace_back(owner);
            }
        }
    }

    // updates print order strings
    if (sorted_instances.size() > 0) {
        for (size_t i = 0; i < sorted_instances.size(); ++i) {
            size_t id = sorted_instances[i]->id().id;
            std::vector<Owner>::iterator it = std::find_if(owners.begin(), owners.end(), [id](const Owner& owner) {
                return owner.model_instance_id == id;
                });
            if (it != owners.end())
                //it->print_order = std::string((_(L("Sequence"))).ToUTF8()) + "#: " + std::to_string(i + 1);
                it->print_order = std::string((_(L("Sequence"))).ToUTF8()) + "#: " + std::to_string(sorted_instances[i]->arrange_order);
        }
    }

    // calculate eye bounding boxes center zs
    for (Owner& owner : owners) {
        owner.eye_center_z = (world_to_eye * owner.world_box.center())(2);
    }

    // sort owners by center eye zs and selection
    std::sort(owners.begin(), owners.end(), [](const Owner& owner1, const Owner& owner2) {
        if (!owner1.selected && owner2.selected)
            return true;
        else if (owner1.selected && !owner2.selected)
            return false;
        else
            return (owner1.eye_center_z < owner2.eye_center_z);
        });

    ImGuiWrapper& imgui = *wxGetApp().imgui();

    // render info windows
    for (const Owner& owner : owners) {
        Vec3d screen_box_center = world_to_screen * owner.world_box.center();
        float x = 0.0f;
        float y = 0.0f;
        if (camera.get_type() == Camera::EType::Perspective) {
            x = (0.5f + 0.001f * 0.5f * (float)screen_box_center(0)) * viewport[2];
            y = (0.5f - 0.001f * 0.5f * (float)screen_box_center(1)) * viewport[3];
        } else {
            x = (0.5f + 0.5f * (float)screen_box_center(0)) * viewport[2];
            y = (0.5f - 0.5f * (float)screen_box_center(1)) * viewport[3];
        }

        if (x < 0.0f || viewport[2] < x || y < 0.0f || viewport[3] < y)
            continue;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, owner.selected ? 3.0f : 1.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, owner.selected ? ImVec4(0.757f, 0.404f, 0.216f, 1.0f) : ImVec4(0.75f, 0.75f, 0.75f, 1.0f));
        imgui.set_next_window_pos(x, y, ImGuiCond_Always, 0.5f, 0.5f);
        imgui.begin(owner.title, ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        float win_w = ImGui::GetWindowWidth();
        ImGui::AlignTextToFramePadding();
        imgui.text(owner.label);

        if (!owner.print_order.empty()) {
            ImGui::Separator();
            float po_len = imgui.calc_text_size(owner.print_order).x;
            ImGui::SetCursorPosX(0.5f * (win_w - po_len));
            ImGui::AlignTextToFramePadding();
            imgui.text(owner.print_order);
        }

        // force re-render while the windows gets to its final size (it takes several frames)
        if (ImGui::GetWindowContentRegionWidth() + 2.0f * ImGui::GetStyle().WindowPadding.x != ImGui::CalcWindowNextAutoFitSize(ImGui::GetCurrentWindow()).x)
            imgui.set_requires_extra_frame();

        imgui.end();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
}

void GLCanvas3D::Tooltip::set_text(const std::string& text)
{
    // If the mouse is inside an ImGUI dialog, then the tooltip is suppressed.
    m_text = m_in_imgui ? std::string() : text;
}

void GLCanvas3D::Tooltip::render(const Vec2d& mouse_position, GLCanvas3D& canvas)
{
    static ImVec2 size(0.0f, 0.0f);

    auto validate_position = [](const Vec2d& position, const GLCanvas3D& canvas, const ImVec2& wnd_size) {
        const Size cnv_size = canvas.get_canvas_size();
        const float x = std::clamp((float)position.x(), 0.0f, (float)cnv_size.get_width() - wnd_size.x);
        const float y = std::clamp((float)position.y() + 16.0f, 0.0f, (float)cnv_size.get_height() - wnd_size.y);
        return Vec2f(x, y);
    };

    if (m_text.empty()) {
        m_start_time = std::chrono::steady_clock::now();
        return;
    }

    // draw the tooltip as hidden until the delay is expired
    // use a value of alpha slightly different from 0.0f because newer imgui does not calculate properly the window size if alpha == 0.0f
    const float alpha = (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start_time).count() < 500) ? 0.01f : 1.0f;

    const Vec2f position = validate_position(mouse_position, canvas, size);

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    imgui.set_next_window_pos(position.x(), position.y(), ImGuiCond_Always, 0.0f, 0.0f);

    imgui.begin(wxString("canvas_tooltip"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::TextUnformatted(m_text.c_str());

    // force re-render while the windows gets to its final size (it may take several frames) or while hidden
#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
    if (alpha < 1.0f || ImGui::GetWindowContentRegionWidth() + 2.0f * ImGui::GetStyle().WindowPadding.x != ImGui::CalcWindowNextAutoFitSize(ImGui::GetCurrentWindow()).x)
        imgui.set_requires_extra_frame();
#else
    if (alpha < 1.0f || ImGui::GetWindowContentRegionWidth() + 2.0f * ImGui::GetStyle().WindowPadding.x != ImGui::CalcWindowNextAutoFitSize(ImGui::GetCurrentWindow()).x)
        canvas.request_extra_frame();
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT

    size = ImGui::GetWindowSize();

    imgui.end();
    ImGui::PopStyleVar(2);
}

//BBS: add height limit logic
void GLCanvas3D::SequentialPrintClearance::set_polygons(const Polygons& polygons, const std::vector<std::pair<Polygon, float>>& height_polygons)
{
    //BBS: add height limit logic
    m_height_limit.reset();
    m_perimeter.reset();
    m_fill.reset();
    if (!polygons.empty()) {
        if (m_render_fill) {
            GLModel::Geometry fill_data;
            fill_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
            fill_data.color  = { 0.8f, 0.8f, 1.0f, 0.5f };

            // vertices + indices
            const ExPolygons polygons_union = union_ex(polygons);
            unsigned int vertices_counter = 0;
            for (const ExPolygon& poly : polygons_union) {
                const std::vector<Vec3d> triangulation = triangulate_expolygon_3d(poly);
                fill_data.reserve_vertices(fill_data.vertices_count() + triangulation.size());
                fill_data.reserve_indices(fill_data.indices_count() + triangulation.size());
                for (const Vec3d& v : triangulation) {
                    fill_data.add_vertex((Vec3f)(v.cast<float>() + 0.0125f * Vec3f::UnitZ())); // add a small positive z to avoid z-fighting
                    ++vertices_counter;
                    if (vertices_counter % 3 == 0)
                        fill_data.add_triangle(vertices_counter - 3, vertices_counter - 2, vertices_counter - 1);
                }
            }

            m_fill.init_from(std::move(fill_data));
        }

        m_perimeter.init_from(polygons, 0.025f); // add a small positive z to avoid z-fighting
    }

    //BBS: add the height limit compute logic
    if (!height_polygons.empty()) {
        GLModel::Geometry height_fill_data;
        height_fill_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        height_fill_data.color  = {0.8f, 0.8f, 1.0f, 0.5f};

        // vertices + indices
        unsigned int vertices_counter = 0;
        for (const auto &poly : height_polygons) {
            ExPolygon                ex_poly(poly.first);
            const std::vector<Vec3d> height_triangulation = triangulate_expolygon_3d(ex_poly);
            for (const Vec3d &v : height_triangulation) {
                Vec3f point{(float) v.x(), (float) v.y(), poly.second};
                height_fill_data.add_vertex(point);
                ++vertices_counter;
                if (vertices_counter % 3 == 0)
                    height_fill_data.add_triangle(vertices_counter - 3, vertices_counter - 2, vertices_counter - 1);
            }
        }

        m_height_limit.init_from(std::move(height_fill_data));
    }
}

void GLCanvas3D::SequentialPrintClearance::render()
{
    const ColorRGBA FILL_COLOR = { 0.7f, 0.7f, 1.0f, 0.5f };
    const ColorRGBA NO_FILL_COLOR = { 0.75f, 0.75f, 0.75f, 0.75f };

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    shader->start_using();

    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    m_perimeter.set_color(m_render_fill ? FILL_COLOR : NO_FILL_COLOR);
    m_perimeter.render();
    m_fill.render();
    //BBS: add height limit
    m_height_limit.set_color(m_render_fill ? FILL_COLOR : NO_FILL_COLOR);
    m_height_limit.render();

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_DEPTH_TEST));

    shader->stop_using();
}

wxDEFINE_EVENT(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_OBJECT_SELECT, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_PLATE_NAME_CHANGE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_PLATE_SELECT, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RIGHT_CLICK, RBtnEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_PLATE_RIGHT_CLICK, RBtnPlateEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_REMOVE_OBJECT, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ARRANGE, SimpleEvent);
//BBS: add arrange and orient event
wxDEFINE_EVENT(EVT_GLCANVAS_ARRANGE_PARTPLATE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ORIENT, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ORIENT_PARTPLATE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_SELECT_CURR_PLATE_ALL, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_SELECT_ALL, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_QUESTION_MARK, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_INCREASE_INSTANCES, Event<int>);
wxDEFINE_EVENT(EVT_GLCANVAS_INSTANCE_MOVED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_INSTANCE_ROTATED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_INSTANCE_SCALED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_FORCE_UPDATE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, Event<bool>);
wxDEFINE_EVENT(EVT_GLCANVAS_UPDATE_GEOMETRY, Vec3dsEvent<2>);
wxDEFINE_EVENT(EVT_GLCANVAS_MOUSE_DRAGGING_STARTED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_UPDATE_BED_SHAPE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_TAB, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RESETGIZMOS, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_MOVE_SLIDERS, wxKeyEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_EDIT_COLOR_CHANGE, wxKeyEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_JUMP_TO, wxKeyEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_UNDO, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_REDO, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_SWITCH_TO_OBJECT, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_SWITCH_TO_GLOBAL, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_COLLAPSE_SIDEBAR, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RELOAD_FROM_DISK, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RENDER_TIMER, wxTimerEvent/*RenderTimerEvent*/);
wxDEFINE_EVENT(EVT_GLCANVAS_TOOLBAR_HIGHLIGHTER_TIMER, wxTimerEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_GIZMO_HIGHLIGHTER_TIMER, wxTimerEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_UPDATE, SimpleEvent);
wxDEFINE_EVENT(EVT_CUSTOMEVT_TICKSCHANGED, wxCommandEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_RESET_LAYER_HEIGHT_PROFILE, SimpleEvent);
wxDEFINE_EVENT(EVT_GLCANVAS_ADAPTIVE_LAYER_HEIGHT_PROFILE, Event<float>);
wxDEFINE_EVENT(EVT_GLCANVAS_SMOOTH_LAYER_HEIGHT_PROFILE, HeightProfileSmoothEvent);

const double GLCanvas3D::DefaultCameraZoomToBoxMarginFactor = 1.25;
const double GLCanvas3D::DefaultCameraZoomToBedMarginFactor = 2.00;
const double GLCanvas3D::DefaultCameraZoomToPlateMarginFactor = 1.25;

void GLCanvas3D::load_arrange_settings()
{
    std::string dist_fff_str =
        wxGetApp().app_config->get("arrange", "min_object_distance_fff");

    std::string dist_fff_seq_print_str =
        wxGetApp().app_config->get("arrange", "min_object_distance_seq_print_fff");

    std::string dist_sla_str =
        wxGetApp().app_config->get("arrange", "min_object_distance_sla");

    std::string en_rot_fff_str =
        wxGetApp().app_config->get("arrange", "enable_rotation_fff");

    std::string en_rot_fff_seqp_str =
        wxGetApp().app_config->get("arrange", "enable_rotation_seq_print");

    std::string en_rot_sla_str =
        wxGetApp().app_config->get("arrange", "enable_rotation_sla");

    std::string en_allow_multiple_materials_str =
        wxGetApp().app_config->get("arrange", "allow_multi_materials_on_same_plate");

    std::string en_avoid_region_str =
        wxGetApp().app_config->get("arrange", "avoid_extrusion_cali_region");



    if (!dist_fff_str.empty())
        m_arrange_settings_fff.distance = std::stof(dist_fff_str);

    if (!dist_fff_seq_print_str.empty())
        m_arrange_settings_fff_seq_print.distance = std::stof(dist_fff_seq_print_str);

    if (!dist_sla_str.empty())
        m_arrange_settings_sla.distance = std::stof(dist_sla_str);

    if (!en_rot_fff_str.empty())
        m_arrange_settings_fff.enable_rotation = (en_rot_fff_str == "1" || en_rot_fff_str == "true");

    if (!en_allow_multiple_materials_str.empty())
        m_arrange_settings_fff.allow_multi_materials_on_same_plate = (en_allow_multiple_materials_str == "1" || en_allow_multiple_materials_str == "true");


    if (!en_rot_fff_seqp_str.empty())
        m_arrange_settings_fff_seq_print.enable_rotation = (en_rot_fff_seqp_str == "1" || en_rot_fff_seqp_str == "true");

    if(!en_avoid_region_str.empty())
        m_arrange_settings_fff.avoid_extrusion_cali_region = (en_avoid_region_str == "1" || en_avoid_region_str == "true");

    if (!en_rot_sla_str.empty())
        m_arrange_settings_sla.enable_rotation = (en_rot_sla_str == "1" || en_rot_sla_str == "true");

    //BBS: add specific arrange settings
    m_arrange_settings_fff_seq_print.is_seq_print = true;
}

GLCanvas3D::ArrangeSettings& GLCanvas3D::get_arrange_settings()
{
    PrinterTechnology ptech = current_printer_technology();

    auto* ptr = &m_arrange_settings_fff;

    if (ptech == ptSLA) {
        ptr = &m_arrange_settings_sla;
    }
    else if (ptech == ptFFF) {
        if (wxGetApp().global_print_sequence() == PrintSequence::ByObject)
            ptr = &m_arrange_settings_fff_seq_print;
        else
            ptr = &m_arrange_settings_fff;
    }

    return *ptr;
}

int GLCanvas3D::GetHoverId()
{
    if (m_hover_plate_idxs.size() == 0) {
        return -1; }
    return m_hover_plate_idxs.front();

}

PrinterTechnology GLCanvas3D::current_printer_technology() const {
    return m_process->current_printer_technology();
}

GLCanvas3D::GLCanvas3D(wxGLCanvas* canvas, Bed3D &bed)
    : m_canvas(canvas)
    , m_context(nullptr)
    , m_bed(bed)
#if ENABLE_RETINA_GL
    , m_retina_helper(nullptr)
#endif
    , m_in_render(false)
    , m_main_toolbar(GLToolbar::Normal, "Main")
    , m_separator_toolbar(GLToolbar::Normal, "Separator")
    , m_assemble_view_toolbar(GLToolbar::Normal, "Assembly_View")
    , m_return_toolbar()
    , m_canvas_type(ECanvasType::CanvasView3D)
    , m_gizmos(*this)
    , m_use_clipping_planes(false)
    , m_sidebar_field("")
    , m_extra_frame_requested(false)
    , m_config(nullptr)
    , m_process(nullptr)
    , m_model(nullptr)
    , m_dirty(true)
    , m_initialized(false)
    , m_apply_zoom_to_volumes_filter(false)
    , m_picking_enabled(false)
    , m_moving_enabled(false)
    , m_dynamic_background_enabled(false)
    , m_multisample_allowed(false)
    , m_moving(false)
    , m_tab_down(false)
    , m_camera_movement(false)
    , m_cursor_type(Standard)
    , m_color_by("volume")
    , m_reload_delayed(false)
    , m_render_sla_auxiliaries(true)
    , m_labels(*this)
    , m_slope(m_volumes)
{
    if (m_canvas != nullptr) {
        m_timer.SetOwner(m_canvas);
        m_render_timer.SetOwner(m_canvas);
#if ENABLE_RETINA_GL
        m_retina_helper.reset(new RetinaHelper(canvas));
#endif // ENABLE_RETINA_GL
    }
    m_timer_set_color.Bind(wxEVT_TIMER, &GLCanvas3D::on_set_color_timer, this);
    load_arrange_settings();

    m_selection.set_volumes(&m_volumes.volumes);

    m_assembly_view_desc["object_selection_caption"] = _L("Left mouse button");
    m_assembly_view_desc["object_selection"]         = _L("object selection");
    // FIXME: maybe should be using GUI::shortkey_alt_prefix() or equivalent?
    m_assembly_view_desc["part_selection_caption"]   = _L("Alt+") + _L("Left mouse button");
    m_assembly_view_desc["part_selection"]           = _L("part selection");
    m_assembly_view_desc["number_key_caption"]       = "1~16 " + _L("number keys");
    m_assembly_view_desc["number_key"]       = _L("number keys can quickly change the color of objects");
}

GLCanvas3D::~GLCanvas3D()
{
    reset_volumes();

    m_sel_plate_toolbar.del_all_item();
    m_sel_plate_toolbar.del_stats_item();
}

void GLCanvas3D::post_event(wxEvent &&event)
{
    event.SetEventObject(m_canvas);
    wxPostEvent(m_canvas, event);
}

bool GLCanvas3D::init()
{
    if (m_initialized)
        return true;

    if (m_canvas == nullptr || m_context == nullptr)
        return false;

    // init dark mode status
    on_change_color_mode(wxGetApp().app_config->get("dark_color_mode") == "1", false);

    m_show_world_axes = wxGetApp().app_config->get("show_axes") == "true";

    BOOST_LOG_TRIVIAL(info) <<__FUNCTION__<< " enter";
    glsafe(::glClearColor(1.0f, 1.0f, 1.0f, 1.0f));
    glsafe(::glClearDepth(1.0f));

    glsafe(::glDepthFunc(GL_LESS));

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    if (m_multisample_allowed)
        glsafe(::glEnable(GL_MULTISAMPLE));

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": before m_layers_editing init";
    if (m_main_toolbar.is_enabled())
        m_layers_editing.init();

    BOOST_LOG_TRIVIAL(info) <<__FUNCTION__<< ": before gizmo init";
    if (m_gizmos.is_enabled() && !m_gizmos.init())
        std::cout << "Unable to initialize gizmos: please, check that all the required textures are available" << std::endl;

    BOOST_LOG_TRIVIAL(info) <<__FUNCTION__<< ": before _init_toolbars";
    if (!_init_toolbars())
        return false;

    BOOST_LOG_TRIVIAL(info) <<__FUNCTION__<< ": finish _init_toolbars";
    if (m_selection.is_enabled() && !m_selection.init())
        return false;

    BOOST_LOG_TRIVIAL(info) <<__FUNCTION__<< ": finish m_selection";

#if ENABLE_IMGUI_STYLE_EDITOR
    //BBS load render color for style editor
    GLVolume::load_render_colors();
    PartPlate::load_render_colors();
    GLGizmoBase::load_render_colors();
    GLCanvas3D::load_render_colors();
    Bed3D::load_render_colors();
#endif
    //if (!wxGetApp().is_gl_version_greater_or_equal_to(3, 0))
    //    wxGetApp().plater()->enable_wireframe(false);
    m_initialized = true;

    return true;
}

void GLCanvas3D::on_change_color_mode(bool is_dark, bool reinit) {
    m_is_dark = is_dark;
    // Bed color
    m_bed.on_change_color_mode(is_dark);
    // GcodeViewer color
    m_gcode_viewer.on_change_color_mode(is_dark);
    // ImGui Style
    wxGetApp().imgui()->on_change_color_mode(is_dark);
    // Notification
    wxGetApp().plater()->get_notification_manager()->on_change_color_mode(is_dark);
    // DailyTips Window
    wxGetApp().plater()->get_dailytips()->on_change_color_mode(is_dark);
    // Preview Slider
    IMSlider* m_layers_slider = get_gcode_viewer().get_layers_slider();
    IMSlider* m_moves_slider = get_gcode_viewer().get_moves_slider();
    m_layers_slider->on_change_color_mode(is_dark);
    m_moves_slider->on_change_color_mode(is_dark);
    // Partplate
    wxGetApp().plater()->get_partplate_list().on_change_color_mode(is_dark);
    // Plates toolbar - Reload All Stats button images
    _init_select_plate_toolbar();

    // Toolbar
    if (m_canvas_type == CanvasView3D) {
        m_gizmos.on_change_color_mode(is_dark);
        if (reinit) {
            // reset svg
            _switch_toolbars_icon_filename();
            m_gizmos.switch_gizmos_icon_filename();
            // set dirty to re-generate icon texture
            m_separator_toolbar.set_icon_dirty();
            m_main_toolbar.set_icon_dirty();
            wxGetApp().plater()->get_collapse_toolbar().set_icon_dirty();
            m_assemble_view_toolbar.set_icon_dirty();
            m_gizmos.set_icon_dirty();
        }
    }
    if (m_canvas_type == CanvasAssembleView) {
        m_gizmos.on_change_color_mode(is_dark);
        if (reinit) {
            // reset svg
            m_gizmos.switch_gizmos_icon_filename();
            // set dirty to re-generate icon texture
            m_gizmos.set_icon_dirty();
        }
    }
}

void GLCanvas3D::set_as_dirty()
{
    m_dirty = true;
}

const float GLCanvas3D::get_scale() const
{
#if ENABLE_RETINA_GL
    return m_retina_helper->get_scale_factor();
#else
    return 1.0f;
#endif
}

unsigned int GLCanvas3D::get_volumes_count() const
{
    return (unsigned int)m_volumes.volumes.size();
}

void GLCanvas3D::reset_volumes()
{
    if (!m_initialized)
        return;

    if (m_volumes.empty())
        return;

    _set_current();

    m_selection.clear();
    m_volumes.clear();
    m_dirty = true;

    _set_warning_notification(EWarning::ObjectOutside, false);
}

//BBS: get current plater's bounding box
BoundingBoxf3 GLCanvas3D::_get_current_partplate_print_volume()
{
    BoundingBoxf3 test_volume;
    if (m_process && m_config)
    {
        BoundingBoxf3 plate_bb = m_process->get_current_plate()->get_bounding_box(false);
        BoundingBoxf3 print_volume({ plate_bb.min(0), plate_bb.min(1), 0.0 }, { plate_bb.max(0), plate_bb.max(1), m_config->opt_float("printable_height") });
        // Allow the objects to protrude below the print bed
        print_volume.min(2) = -1e10;
        print_volume.min(0) -= Slic3r::BuildVolume::BedEpsilon;
        print_volume.min(1) -= Slic3r::BuildVolume::BedEpsilon;
        print_volume.max(0) += Slic3r::BuildVolume::BedEpsilon;
        print_volume.max(1) += Slic3r::BuildVolume::BedEpsilon;
        test_volume = print_volume;
    }
    else
        test_volume = BoundingBoxf3();

    return test_volume;
}

static bool construct_error_string(ObjectFilamentResults& object_result, std::string& error_string)
{
    error_string.clear();
    if (!object_result.partly_outside_objects.empty()) {
        error_string += _u8L("Following objects are laid over the boundary of plate or exceeds the height limit:\n");
        for(auto& object: object_result.partly_outside_objects)
        {
            error_string += object->name;
            error_string += "\n";
        }
        error_string += _u8L("Please solve the problem by moving it totally on or off the plate, and confirming that the height is within the build volume.\n");
        return true;
    }
    return false;
}

static std::pair<bool, bool> construct_extruder_unprintable_error(ObjectFilamentResults& object_result, std::string& left_extruder_unprintable_text, std::string& right_extruder_unprintable_text)
{
    left_extruder_unprintable_text.clear();
    right_extruder_unprintable_text.clear();
    if (object_result.filaments.empty())
        return {false,false};

    const std::vector<std::string> nozzle_name_list = { _u8L("left nozzle"), _u8L("right nozzle") };

    std::vector<ObjectFilamentInfo> left_unprintable_objects;
    std::vector<ObjectFilamentInfo> right_unprintable_objects;

    std::vector<int> conflicted_filaments = object_result.filaments;

    auto mode = object_result.mode;

    for (auto& obj_filament : object_result.object_filaments) {
        if (mode == FilamentMapMode::fmmManual) {
            for (auto& elem : obj_filament.manual_filaments) {
                bool found_left = false, found_right = false;
                int filamnet_id = elem.first;
                int extruder_id = elem.second;
                if (extruder_id == 1 && !found_left) {
                    found_left = true;
                    left_unprintable_objects.emplace_back(obj_filament);
                }
                if (extruder_id == 2 && !found_right) {
                    found_right = true;
                    right_unprintable_objects.emplace_back(obj_filament);
                }
            }
        }
        else {
            if (!obj_filament.auto_filaments.empty()) {
                left_unprintable_objects.emplace_back(obj_filament);
                right_unprintable_objects.emplace_back(obj_filament);
            }
        }
    }

    Preset &preset = GUI::wxGetApp().preset_bundle->printers.get_edited_preset();
    float   left_x_min = 0, left_x_max = 0, left_y_min = 0, left_y_max = 0, left_z_min = 0, left_z_max = 0;
    float   right_x_min = 0, right_x_max = 0, right_y_min = 0, right_y_max = 0, right_z_min = 0, right_z_max = 0;
    auto printable_height_option = preset.config.option<ConfigOptionFloatsNullable>("extruder_printable_height");
    if (printable_height_option && printable_height_option->values.size() == 2) {
        left_z_max = (float) printable_height_option->values[0];
        right_z_max = (float) printable_height_option->values[1];
    }
    std::vector<Pointfs> printable_areas = preset.config.option<ConfigOptionPointsGroups>("extruder_printable_area")->values;
    if (printable_areas.size() == 2 && printable_areas[0].size() == 4) {
        left_x_min = printable_areas[0][0][0];
        left_y_min = printable_areas[0][0][1];
        left_x_max = printable_areas[0][2][0];
        left_y_max = printable_areas[0][2][1];

        right_x_min = printable_areas[1][0][0];
        right_y_min = printable_areas[1][0][1];
        right_x_max = printable_areas[1][2][0];
        right_y_max = printable_areas[1][2][1];
    }

    std::vector<std::string> tips(2);
    for (size_t idx = 0; idx < tips.size(); ++idx) {
        const auto& unprintable_objs = idx == 0 ? left_unprintable_objects : right_unprintable_objects;
        auto& output_text = idx == 0 ? left_extruder_unprintable_text : right_extruder_unprintable_text;
        if (unprintable_objs.empty())
            continue;
        std::string nozzle_name = nozzle_name_list[idx];
        std::string model_prefix;
        if ((idx == 0 && left_unprintable_objects.size() > 1) || (idx == 1 && right_unprintable_objects.size() > 1))
            model_prefix = (boost::format(_u8L("The position or size of some models exceeds the %s's printable range.")) % nozzle_name).str();
        else
            model_prefix = (boost::format(_u8L("The position or size of the model %s exceeds the %s's printable range."))
                           %object_result.object_filaments.front().object->name % nozzle_name).str();
        tips[idx] += model_prefix;

        tips[idx] += (boost::format(_u8L(" Please check and adjust the part's position or size to fit the printable range:\n"))).str();
        if (idx == 0)
            tips[idx] += (boost::format(_u8L("Left nozzle: X:%1%-%2%, Y:%3%-%4%, Z:%5%-%6%\n"))
                         % format_number(left_x_min) % format_number(left_x_max) % format_number(left_y_min)
                         % format_number(left_y_max) % format_number(left_z_min) % format_number(left_z_max)).str();
        else
            tips[idx] += (boost::format(_u8L("Right nozzle: X:%1%-%2%, Y:%3%-%4%, Z:%5%-%6%"))
                         %format_number(right_x_min) %format_number(right_x_max) %format_number(right_y_min)
                         %format_number(right_y_max) %format_number(right_z_min) %format_number(right_z_max)).str();
        output_text = tips[idx];
    }

    return { !left_unprintable_objects.empty(),!right_unprintable_objects.empty() };
}

ModelInstanceEPrintVolumeState GLCanvas3D::check_volumes_outside_state(ObjectFilamentResults* object_results) const
{
    //BBS: if not initialized, return inside directly insteadof assert
    if (!m_initialized) {
        return ModelInstancePVS_Inside;
    }
    //assert(m_initialized);

    ModelInstanceEPrintVolumeState state;
    m_volumes.check_outside_state(m_bed.build_volume(), &state, object_results);

    construct_error_string(*object_results, get_object_clashed_text());
    construct_extruder_unprintable_error(*object_results, get_left_extruder_unprintable_text(), get_right_extruder_unprintable_text());
    return state;
}

void GLCanvas3D::toggle_world_axes_visibility(bool force_show)
{
    if (force_show) {
        m_show_world_axes = true;
    } else {
        m_show_world_axes = !m_show_world_axes;
    }
    wxGetApp().app_config->set_bool("show_axes", m_show_world_axes);
    set_as_dirty();
}

void GLCanvas3D::toggle_selected_volume_visibility(bool selected_visible)
{
    m_render_sla_auxiliaries = !selected_visible;
    if (selected_visible) {
        const Selection::IndicesList &idxs = m_selection.get_volume_idxs();
        if (idxs.size() > 0) {
            for (GLVolume *vol : m_volumes.volumes) {
                if (vol->composite_id.object_id >= 1000 && vol->composite_id.object_id < 1000 + wxGetApp().plater()->get_partplate_list().get_plate_count())
                    continue; // the wipe tower
                if (vol->composite_id.volume_id >= 0) {
                    vol->is_active = false;
                }
            }
            for (unsigned int idx : idxs) {
                GLVolume *v  = const_cast<GLVolume *>(m_selection.get_volume(idx));
                v->is_active = true;
            }
        }
    } else { // show all
        for (GLVolume *vol : m_volumes.volumes) {
            if (vol->composite_id.object_id >= 1000 && vol->composite_id.object_id < 1000 + wxGetApp().plater()->get_partplate_list().get_plate_count())
                continue; // the wipe tower
            if (vol->composite_id.volume_id >= 0) {
                vol->is_active = true;
            }
        }
    }
}

void GLCanvas3D::toggle_sla_auxiliaries_visibility(bool visible, const ModelObject *mo, int instance_idx)
{
    if (current_printer_technology() != ptSLA)
        return;

    m_render_sla_auxiliaries = visible;

    std::vector<std::shared_ptr<SceneRaycasterItem>>* raycasters = get_raycasters_for_picking(SceneRaycaster::EType::Volume);

    for (GLVolume* vol : m_volumes.volumes) {
        if (vol->composite_id.object_id >= 1000 &&
            vol->composite_id.object_id < 1000 + wxGetApp().plater()->get_partplate_list().get_plate_count())
            continue; // the wipe tower
      if ((mo == nullptr || m_model->objects[vol->composite_id.object_id] == mo)
            && (instance_idx == -1 || vol->composite_id.instance_id == instance_idx)
            && vol->composite_id.volume_id < 0) {
            vol->is_active = visible;
            auto it = std::find_if(raycasters->begin(), raycasters->end(), [vol](std::shared_ptr<SceneRaycasterItem> item) { return item->get_raycaster() == vol->mesh_raycaster.get(); });
            if (it != raycasters->end())
                (*it)->set_active(vol->is_active);
        }
    }
}

void GLCanvas3D::toggle_model_objects_visibility(bool visible, const ModelObject* mo, int instance_idx, const ModelVolume* mv)
{
    std::vector<std::shared_ptr<SceneRaycasterItem>>* raycasters = get_raycasters_for_picking(SceneRaycaster::EType::Volume);
    for (GLVolume* vol : m_volumes.volumes) {
        // BBS: add partplate logic
        if (vol->composite_id.object_id >= 1000 &&
            vol->composite_id.object_id < 1000 + wxGetApp().plater()->get_partplate_list().get_plate_count()) { // wipe tower
            vol->is_active = (visible && mo == nullptr);
        }
        else {
            if ((mo == nullptr || m_model->objects[vol->composite_id.object_id] == mo)
            && (instance_idx == -1 || vol->composite_id.instance_id == instance_idx)
            && (mv == nullptr || m_model->objects[vol->composite_id.object_id]->volumes[vol->composite_id.volume_id] == mv)) {
                vol->is_active = visible;
                if (!vol->is_modifier)
                    vol->color.a(1.f);

                if (instance_idx == -1) {
                    vol->force_native_color = false;
                    vol->force_neutral_color = false;
                } else {
                    const GLGizmosManager& gm = get_gizmos_manager();
                    auto gizmo_type = gm.get_current_type();
                    if (  (gizmo_type == GLGizmosManager::FdmSupports
                        || gizmo_type == GLGizmosManager::Seam
                        || gizmo_type == GLGizmosManager::Cut
                        || gizmo_type == GLGizmosManager::FuzzySkin)
                        && !vol->is_modifier) {
                        vol->force_neutral_color = true;
                    }
                    else if (gizmo_type == GLGizmosManager::BrimEars)
                        vol->force_neutral_color = false;
                    else if (gizmo_type == GLGizmosManager::MmSegmentation)
                        vol->is_active = false;
                    else
                        vol->force_native_color = true;
                }
            }
        }

        auto it = std::find_if(raycasters->begin(), raycasters->end(), [vol](std::shared_ptr<SceneRaycasterItem> item) { return item->get_raycaster() == vol->mesh_raycaster.get(); });
        if (it != raycasters->end())
            (*it)->set_active(vol->is_active);
    }

    if (visible && !mo)
        toggle_sla_auxiliaries_visibility(true, mo, instance_idx);

    if (!mo && !visible && !m_model->objects.empty() && (m_model->objects.size() > 1 || m_model->objects.front()->instances.size() > 1))
        _set_warning_notification(EWarning::SomethingNotShown, true);

    if (!mo && visible)
        _set_warning_notification(EWarning::SomethingNotShown, false);
}

void GLCanvas3D::update_instance_printable_state_for_object(const size_t obj_idx)
{
    ModelObject* model_object = m_model->objects[obj_idx];
    for (int inst_idx = 0; inst_idx < (int)model_object->instances.size(); ++inst_idx) {
        ModelInstance* instance = model_object->instances[inst_idx];

        for (GLVolume* volume : m_volumes.volumes) {
            if ((volume->object_idx() == (int)obj_idx) && (volume->instance_idx() == inst_idx))
                volume->printable = instance->printable;
                if (!volume->printable) {
                    volume->render_color = GLVolume::UNPRINTABLE_COLOR;
                }
        }
    }
}

void GLCanvas3D::update_instance_printable_state_for_objects(const std::vector<size_t>& object_idxs)
{
    for (size_t obj_idx : object_idxs)
        update_instance_printable_state_for_object(obj_idx);
}

void GLCanvas3D::set_config(const DynamicPrintConfig* config)
{
    m_config = config;
    m_layers_editing.set_config(config);

    // Orca: Filament shrinkage compensation
    const Print *print = fff_print();
    if (print != nullptr)
        m_layers_editing.set_shrinkage_compensation(fff_print()->shrinkage_compensation());
}

void GLCanvas3D::set_process(BackgroundSlicingProcess *process)
{
    m_process = process;
}

void GLCanvas3D::set_model(Model* model)
{
    m_model = model;
    m_selection.set_model(m_model);
}

void GLCanvas3D::bed_shape_changed()
{
    refresh_camera_scene_box();
    wxGetApp().plater()->get_camera().requires_zoom_to_bed = true;
    m_dirty = true;
}

void GLCanvas3D::plates_count_changed()
{
    refresh_camera_scene_box();
    m_dirty = true;
}

Camera& GLCanvas3D::get_camera()
{
    return camera;
}

void GLCanvas3D::set_color_by(const std::string& value)
{
    m_color_by = value;
}

void GLCanvas3D::refresh_camera_scene_box()
{
    wxGetApp().plater()->get_camera().set_scene_box(scene_bounding_box());
}

BoundingBoxf3 GLCanvas3D::volumes_bounding_box(bool current_plate_only) const
{
    BoundingBoxf3 bb;
    BoundingBoxf3 expand_part_plate_list_box;
    bool          is_limit = m_canvas_type != ECanvasType::CanvasAssembleView;
    if (is_limit) {
        if (current_plate_only) {
            expand_part_plate_list_box = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_bounding_box();
        } else {
            auto        plate_list_box = wxGetApp().plater()->get_partplate_list().get_bounding_box();
            auto        horizontal_radius = 0.5 * sqrt(std::pow(plate_list_box.min[0] - plate_list_box.max[0], 2) + std::pow(plate_list_box.min[1] - plate_list_box.max[1], 2));
            const float scale             = 2;
            expand_part_plate_list_box.merge(plate_list_box.min - scale * Vec3d(horizontal_radius, horizontal_radius, 0));
            expand_part_plate_list_box.merge(plate_list_box.max + scale * Vec3d(horizontal_radius, horizontal_radius, 0));
        }
    }
    for (const GLVolume *volume : m_volumes.volumes) {
        if (!m_apply_zoom_to_volumes_filter || ((volume != nullptr) && volume->zoom_to_volumes)) {
            const auto v_bb     = volume->transformed_bounding_box();
            if (is_limit && !expand_part_plate_list_box.overlap(v_bb))
                continue;
            bb.merge(v_bb);
        }
    }
    return bb;
}

BoundingBoxf3 GLCanvas3D::scene_bounding_box() const
{
    BoundingBoxf3 bb = volumes_bounding_box();
    bb.merge(m_bed.extended_bounding_box());
    double h = m_bed.build_volume().printable_height();
    //FIXME why -h?
    bb.min.z() = std::min(bb.min.z(), -h);
    bb.max.z() = std::max(bb.max.z(), h);

    //BBS merge plate scene bounding box
    if (m_canvas_type == ECanvasType::CanvasView3D) {
        PartPlateList& plate = wxGetApp().plater()->get_partplate_list();
        bb.merge(plate.get_bounding_box());
    }

    return bb;
}

BoundingBoxf3 GLCanvas3D::plate_scene_bounding_box(int plate_idx) const
{
    PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_plate(plate_idx);

    BoundingBoxf3 bb = plate->get_bounding_box(true);
    if (m_config != nullptr) {
        double h = m_config->opt_float("printable_height");
        bb.min(2) = std::min(bb.min(2), -h);
        bb.max(2) = std::max(bb.max(2), h);
    }

    return bb;
}

bool GLCanvas3D::is_layers_editing_enabled() const
{
    return m_layers_editing.is_enabled();
}

bool GLCanvas3D::is_layers_editing_allowed() const
{
    return m_layers_editing.is_allowed();
}

void GLCanvas3D::reset_layer_height_profile()
{
    wxGetApp().plater()->take_snapshot("Variable layer height - Reset");
    m_layers_editing.reset_layer_height_profile(*this);
    m_layers_editing.state = LayersEditing::Completed;
    m_dirty = true;
}

void GLCanvas3D::adaptive_layer_height_profile(float quality_factor)
{
    wxGetApp().plater()->take_snapshot("Variable layer height - Adaptive");
    m_layers_editing.adaptive_layer_height_profile(*this, quality_factor);
    m_layers_editing.state = LayersEditing::Completed;
    m_dirty = true;
}

void GLCanvas3D::smooth_layer_height_profile(const HeightProfileSmoothingParams& smoothing_params)
{
    wxGetApp().plater()->take_snapshot("Variable layer height - Smooth all");
    m_layers_editing.smooth_layer_height_profile(*this, smoothing_params);
    m_layers_editing.state = LayersEditing::Completed;
    m_dirty = true;
}

bool GLCanvas3D::is_reload_delayed() const
{
    return m_reload_delayed;
}

void GLCanvas3D::enable_layers_editing(bool enable)
{
    m_layers_editing.set_enabled(enable);
    set_as_dirty();
}

void GLCanvas3D::enable_legend_texture(bool enable)
{
    m_gcode_viewer.enable_legend(enable);
}

void GLCanvas3D::enable_picking(bool enable)
{
    m_picking_enabled = enable;

    // Orca: invalidate hover state when dragging is toggled, otherwise if we turned off dragging
    // while hovering above a volume, the hovering state won't update even if mouse has moved away.
    // Fixes https://github.com/OrcaSlicer/OrcaSlicer/pull/9979#issuecomment-3065575889
    m_hover_volume_idxs.clear();
}

void GLCanvas3D::enable_moving(bool enable)
{
    m_moving_enabled = enable;
}

void GLCanvas3D::enable_gizmos(bool enable)
{
    m_gizmos.set_enabled(enable);
}

void GLCanvas3D::enable_selection(bool enable)
{
    m_selection.set_enabled(enable);
}

void GLCanvas3D::enable_main_toolbar(bool enable)
{
    m_main_toolbar.set_enabled(enable);
}

void GLCanvas3D::reset_select_plate_toolbar_selection() {
    if (m_sel_plate_toolbar.m_all_plates_stats_item)
        m_sel_plate_toolbar.m_all_plates_stats_item->selected = false;
    if (wxGetApp().mainframe)
        wxGetApp().mainframe->update_slice_print_status(MainFrame::eEventSliceUpdate, true, true);
}

void GLCanvas3D::enable_select_plate_toolbar(bool enable)
{
    m_sel_plate_toolbar.set_enabled(enable);
}

void GLCanvas3D::enable_assemble_view_toolbar(bool enable)
{
    m_assemble_view_toolbar.set_enabled(enable);
}

void GLCanvas3D::enable_return_toolbar(bool enable)
{
    m_return_toolbar.set_enabled(enable);
}

void GLCanvas3D::enable_separator_toolbar(bool enable)
{
    m_separator_toolbar.set_enabled(enable);
}

void GLCanvas3D::enable_dynamic_background(bool enable)
{
    m_dynamic_background_enabled = enable;
}

void GLCanvas3D::allow_multisample(bool allow)
{
    m_multisample_allowed = allow;
}

void GLCanvas3D::zoom_to_bed()
{
    BoundingBoxf3 box = m_bed.build_volume().bounding_volume();
    box.min.z() = 0.0;
    box.max.z() = 0.0;
    _zoom_to_box(box, DefaultCameraZoomToBedMarginFactor);
}

void GLCanvas3D::zoom_to_volumes()
{
    m_apply_zoom_to_volumes_filter = true;
    _zoom_to_box(volumes_bounding_box());
    m_apply_zoom_to_volumes_filter = false;
}

void GLCanvas3D::zoom_to_selection()
{
    if (!m_selection.is_empty())
        _zoom_to_box(m_selection.get_bounding_box());
}

void GLCanvas3D::zoom_to_gcode()
{
    _zoom_to_box(m_gcode_viewer.get_paths_bounding_box(), 1.05);
}

void GLCanvas3D::zoom_to_plate(int plate_idx)
{
    BoundingBoxf3 box;
    if (plate_idx == REQUIRES_ZOOM_TO_ALL_PLATE) {
        box = wxGetApp().plater()->get_partplate_list().get_bounding_box();
        box.min.z() = 0.0;
        box.max.z() = 0.0;
        _zoom_to_box(box, DefaultCameraZoomToPlateMarginFactor);
    } else {
        PartPlate* plate = nullptr;
        if (plate_idx == REQUIRES_ZOOM_TO_CUR_PLATE) {
            plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
        }else {
            assert(plate_idx >= 0 && plate_idx < wxGetApp().plater()->get_partplate_list().get_plate_count());
            plate = wxGetApp().plater()->get_partplate_list().get_plate(plate_idx);
        }
        box = plate->get_bounding_box(true);
        box.min.z() = 0.0;
        box.max.z() = 0.0;
        _zoom_to_box(box, DefaultCameraZoomToPlateMarginFactor);
    }
}

void GLCanvas3D::select_view(const std::string& direction)
{
    wxGetApp().plater()->get_camera().select_view(direction);
    if (m_canvas != nullptr)
        m_canvas->Refresh();
}

void GLCanvas3D::select_plate()
{
    wxGetApp().plater()->get_partplate_list().select_plate_view();
    if (m_canvas != nullptr)
        m_canvas->Refresh();
}

void GLCanvas3D::update_volumes_colors_by_extruder()
{
    if (m_config != nullptr)
        m_volumes.update_colors_by_extruder(m_config);
}

bool GLCanvas3D::is_collapse_toolbar_on_left() const
{
    auto state = wxGetApp().plater()->get_sidebar_docking_state();
    return state == Sidebar::Left;
}

float GLCanvas3D::get_collapse_toolbar_width() const
{
    GLToolbar& collapse_toolbar = wxGetApp().plater()->get_collapse_toolbar();
    const auto state            = wxGetApp().plater()->get_sidebar_docking_state();

    return state != Sidebar::None ? collapse_toolbar.get_width() : 0;
}

float GLCanvas3D::get_collapse_toolbar_height() const
{
    GLToolbar& collapse_toolbar = wxGetApp().plater()->get_collapse_toolbar();
    const auto state            = wxGetApp().plater()->get_sidebar_docking_state();

    return state != Sidebar::None ? collapse_toolbar.get_height() : 0;
}

bool GLCanvas3D::make_current_for_postinit() {
    return _set_current();
}

void GLCanvas3D::render(bool only_init)
{
    if (m_in_render) {
        // if called recursively, return
        m_dirty = true;
        return;
    }

    m_in_render = true;
    Slic3r::ScopeGuard in_render_guard([this]() { m_in_render = false; });
    (void)in_render_guard;

    if (m_canvas == nullptr)
        return;

    //BBS: add enable_render
    if (!m_enable_render)
        return;

    // ensures this canvas is current and initialized
    if (!_is_shown_on_screen() || !_set_current() || !wxGetApp().init_opengl())
        return;

    if (!is_initialized() && !init())
        return;
    if (m_canvas_type == ECanvasType::CanvasView3D  && m_gizmos.get_current_type() == GLGizmosManager::Undefined) {
        enable_return_toolbar(false);
    }
    if (!m_main_toolbar.is_enabled())
        m_gcode_viewer.init(wxGetApp().get_mode(), wxGetApp().preset_bundle);

    if (! m_bed.build_volume().valid()) {
        // this happens at startup when no data is still saved under <>\AppData\Roaming\Slic3rPE
        post_event(SimpleEvent(EVT_GLCANVAS_UPDATE_BED_SHAPE));
        return;
    }

    if (only_init)
        return;

#if ENABLE_ENVIRONMENT_MAP
    if (wxGetApp().is_editor())
        wxGetApp().plater()->init_environment_texture();
#endif // ENABLE_ENVIRONMENT_MAP

    const Size& cnv_size = get_canvas_size();
    // Probably due to different order of events on Linux/GTK2, when one switched from 3D scene
    // to preview, this was called before canvas had its final size. It reported zero width
    // and the viewport was set incorrectly, leading to tripping glAsserts further down
    // the road (in apply_projection). That's why the minimum size is forced to 10.
    Camera& camera = wxGetApp().plater()->get_camera();
    camera.set_viewport(0, 0, std::max(10u, (unsigned int)cnv_size.get_width()), std::max(10u, (unsigned int)cnv_size.get_height()));
    camera.apply_viewport();

    if (camera.requires_zoom_to_bed) {
        zoom_to_bed();
        _resize((unsigned int)cnv_size.get_width(), (unsigned int)cnv_size.get_height());
        camera.requires_zoom_to_bed = false;
    }

    if (camera.requires_zoom_to_plate > REQUIRES_ZOOM_TO_PLATE_IDLE) {
        zoom_to_plate(camera.requires_zoom_to_plate);
        _resize((unsigned int)cnv_size.get_width(), (unsigned int)cnv_size.get_height());
        camera.requires_zoom_to_plate = REQUIRES_ZOOM_TO_PLATE_IDLE;
    }

    if (camera.requires_zoom_to_volumes) {
        zoom_to_volumes();
        _resize((unsigned int)cnv_size.get_width(), (unsigned int)cnv_size.get_height());
        camera.requires_zoom_to_volumes = false;
    }

    camera.apply_projection(_max_bounding_box(true, true, true));

    wxGetApp().imgui()->new_frame();

    if (m_picking_enabled) {
        if (m_rectangle_selection.is_dragging())
            // picking pass using rectangle selection
            _rectangular_selection_picking_pass();
        //BBS: enable picking when no volumes for partplate logic
        //else if (!m_volumes.empty())
        else {
            // regular picking pass
            _picking_pass();

#if ENABLE_RAYCAST_PICKING_DEBUG
            ImGuiWrapper& imgui = *wxGetApp().imgui();
            imgui.begin(std::string("Hit result"), ImGuiWindowFlags_AlwaysAutoResize);
            imgui.text("Picking disabled");
            imgui.end();
#endif // ENABLE_RAYCAST_PICKING_DEBUG
        }
    }

    // draw scene
    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    _render_background();

    //BBS add partplater rendering logic
    bool only_current = false, only_body = false, no_partplate = false;    
    bool show_grid = true;
    GLGizmosManager::EType gizmo_type = m_gizmos.get_current_type();
    if (!m_main_toolbar.is_enabled()) {
        //only_body = true;
        only_current = true;
    }
    else if ((gizmo_type == GLGizmosManager::FdmSupports) || (gizmo_type == GLGizmosManager::Seam) || (gizmo_type == GLGizmosManager::MmSegmentation) || (gizmo_type == GLGizmosManager::FuzzySkin))
        no_partplate = true;
    else if (gizmo_type == GLGizmosManager::BrimEars && !camera.is_looking_downward())
        show_grid = false;

    /* view3D render*/
    int hover_id = (m_hover_plate_idxs.size() > 0)?m_hover_plate_idxs.front():-1;
    if (m_canvas_type == ECanvasType::CanvasView3D) {
        //BBS: add outline logic
        _render_objects(GLVolumeCollection::ERenderType::Opaque, !m_gizmos.is_running());
        _render_sla_slices();
        _render_selection();
        if (!no_partplate)
            _render_bed(camera.get_view_matrix(), camera.get_projection_matrix(), !camera.is_looking_downward(), m_show_world_axes);
        if (!no_partplate) //BBS: add outline logic
            _render_platelist(camera.get_view_matrix(), camera.get_projection_matrix(), !camera.is_looking_downward(), only_current, only_body, hover_id, true, show_grid);
        _render_objects(GLVolumeCollection::ERenderType::Transparent, !m_gizmos.is_running());
    }
    /* preview render */
    else if (m_canvas_type == ECanvasType::CanvasPreview && m_render_preview) {
        _render_objects(GLVolumeCollection::ERenderType::Opaque, !m_gizmos.is_running());
        _render_sla_slices();
        _render_selection();
        _render_bed(camera.get_view_matrix(), camera.get_projection_matrix(), !camera.is_looking_downward(), m_show_world_axes);
        _render_platelist(camera.get_view_matrix(), camera.get_projection_matrix(), !camera.is_looking_downward(), only_current, true, hover_id);
        // BBS: GUI refactor: add canvas size as parameters
        _render_gcode(cnv_size.get_width(), cnv_size.get_height());
    }
    /* assemble render*/
    else if (m_canvas_type == ECanvasType::CanvasAssembleView) {
        //BBS: add outline logic
        //if (m_show_world_axes) {
        //    m_axes.render();
        //}
        _render_objects(GLVolumeCollection::ERenderType::Opaque, !m_gizmos.is_running());
        _render_selection();
        //_render_bed(camera.get_view_matrix(), camera.get_projection_matrix(), !camera.is_looking_downward(), show_axes);
        _render_plane();
        //BBS: add outline logic insteadof selection under assemble view
        //_render_selection();
        // BBS: add outline logic
        _render_objects(GLVolumeCollection::ERenderType::Transparent, !m_gizmos.is_running());
    }

    _render_sequential_clearance();
#if ENABLE_RENDER_SELECTION_CENTER
    _render_selection_center();
#endif // ENABLE_RENDER_SELECTION_CENTER

    // we need to set the mouse's scene position here because the depth buffer
    // could be invalidated by the following gizmo render methods
    // this position is used later into on_mouse() to drag the objects
    if (m_picking_enabled)
        m_mouse.scene_position = _mouse_to_3d(m_mouse.position.cast<coord_t>());

    // sidebar hints need to be rendered before the gizmos because the depth buffer
    // could be invalidated by the following gizmo render methods
    _render_selection_sidebar_hints();
    _render_current_gizmo();

#if ENABLE_RAYCAST_PICKING_DEBUG
    if (m_picking_enabled && !m_mouse.dragging && !m_gizmos.is_dragging() && !m_rectangle_selection.is_dragging())
        m_scene_raycaster.render_hit(camera);
#endif // ENABLE_RAYCAST_PICKING_DEBUG

#if ENABLE_SHOW_CAMERA_TARGET
    _render_camera_target();
#endif // ENABLE_SHOW_CAMERA_TARGET

    if (m_picking_enabled && m_rectangle_selection.is_dragging())
        m_rectangle_selection.render(*this);

    // draw overlays
    _render_overlays();

    if (wxGetApp().plater()->is_render_statistic_dialog_visible()) {
        ImGui::ShowMetricsWindow();

        ImGuiWrapper& imgui = *wxGetApp().imgui();
        imgui.begin(std::string("Render statistics"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
        imgui.text("FPS (SwapBuffers() calls per second):");
        ImGui::SameLine();
        imgui.text(std::to_string(m_render_stats.get_fps_and_reset_if_needed()));
        ImGui::Separator();
        imgui.text("Compressed textures:");
        ImGui::SameLine();
        imgui.text(OpenGLManager::are_compressed_textures_supported() ? "supported" : "not supported");
        imgui.text("Max texture size:");
        ImGui::SameLine();
        imgui.text(std::to_string(OpenGLManager::get_gl_info().get_max_tex_size()));
        imgui.end();
    }

#if ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW
    if (wxGetApp().is_editor() && wxGetApp().plater()->is_view3D_shown())
        wxGetApp().plater()->render_project_state_debug_window();
#endif // ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW

#if ENABLE_CAMERA_STATISTICS
    camera.debug_render();
#endif // ENABLE_CAMERA_STATISTICS

#if ENABLE_IMGUI_STYLE_EDITOR
    if (wxGetApp().get_mode() == ConfigOptionMode::comDevelop)
        _render_style_editor();
#endif


    std::string tooltip;

	// Negative coordinate means out of the window, likely because the window was deactivated.
	// In that case the tooltip should be hidden.
    if (m_mouse.position.x() >= 0. && m_mouse.position.y() >= 0.) {
        if (tooltip.empty())
            tooltip = m_layers_editing.get_tooltip(*this);

	    if (tooltip.empty())
	        tooltip = m_gizmos.get_tooltip();

	    if (tooltip.empty())
	        tooltip = m_main_toolbar.get_tooltip();

        //BBS: GUI refactor: GLToolbar
        if (tooltip.empty())
            tooltip = m_assemble_view_toolbar.get_tooltip();

	    if (tooltip.empty())
            tooltip = wxGetApp().plater()->get_collapse_toolbar().get_tooltip();

        // BBS
#if 0
	    if (tooltip.empty())
            tooltip = wxGetApp().plater()->get_view_toolbar().get_tooltip();
#endif
    }

    set_tooltip(tooltip);

    if (m_tooltip_enabled)
        m_tooltip.render(m_mouse.position, *this);

    wxGetApp().plater()->get_mouse3d_controller().render_settings_dialog(*this);

    if (m_canvas_type != ECanvasType::CanvasAssembleView) {
        float right_margin = SLIDER_DEFAULT_RIGHT_MARGIN;
        float bottom_margin = SLIDER_DEFAULT_BOTTOM_MARGIN;
        if (m_canvas_type == ECanvasType::CanvasPreview) {
            float scale_factor = get_scale();
#ifdef WIN32
            int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
            scale_factor *= (float) dpi / (float) DPI_DEFAULT;
#endif // WIN32
            right_margin = SLIDER_RIGHT_MARGIN * scale_factor * GCODE_VIEWER_SLIDER_SCALE;
            bottom_margin = SLIDER_BOTTOM_MARGIN * scale_factor * GCODE_VIEWER_SLIDER_SCALE;
        }
        wxGetApp().plater()->get_notification_manager()->render_notifications(*this, get_overlay_window_width(), bottom_margin, right_margin);
        wxGetApp().plater()->get_dailytips()->render();
    }

    wxGetApp().imgui()->render();

    m_canvas->SwapBuffers();
    m_render_stats.increment_fps_counter();
}

void GLCanvas3D::render_thumbnail(ThumbnailData &         thumbnail_data,
                                  unsigned int            w,
                                  unsigned int            h,
                                  const ThumbnailsParams &thumbnail_params,
                                  Camera::EType           camera_type,
                                  Camera::ViewAngleType   camera_view_angle_type,
                                  bool                    for_picking,
                                  bool                    ban_light)
{
    ModelObjectPtrs &model_objects = GUI::wxGetApp().model().objects;
    render_thumbnail(thumbnail_data, w, h, thumbnail_params, model_objects, m_volumes, camera_type, camera_view_angle_type, for_picking, ban_light);
}

void GLCanvas3D::render_thumbnail(ThumbnailData &           thumbnail_data,
                                  unsigned int              w,
                                  unsigned int              h,
                                  const ThumbnailsParams &  thumbnail_params,
                                  ModelObjectPtrs &         model_objects,
                                  const GLVolumeCollection &volumes,
                                  Camera::EType             camera_type,
                                  Camera::ViewAngleType     camera_view_angle_type,
                                  bool                      for_picking,
                                  bool                      ban_light)
{
    GLShaderProgram* shader = nullptr;
    if (for_picking)
        shader = wxGetApp().get_shader("flat");
    else
        shader = wxGetApp().get_shader("thumbnail");
    std::vector<ColorRGBA> colors = wxGetApp().plater()->get_extruders_colors();
    switch (OpenGLManager::get_framebuffers_type())
    {
    case OpenGLManager::EFramebufferType::Arb:
        { render_thumbnail_framebuffer(thumbnail_data, w, h, thumbnail_params, wxGetApp().plater()->get_partplate_list(), model_objects, volumes, colors, shader, camera_type,
                                     camera_view_angle_type, for_picking, ban_light);
        break;
    }
    case OpenGLManager::EFramebufferType::Ext:
        { render_thumbnail_framebuffer_ext(thumbnail_data, w, h, thumbnail_params, wxGetApp().plater()->get_partplate_list(), model_objects, volumes, colors, shader, camera_type,
                                         camera_view_angle_type, for_picking, ban_light);
        break;
    }
    default:{
        render_thumbnail_legacy(thumbnail_data, w, h, thumbnail_params, wxGetApp().plater()->get_partplate_list(), model_objects, volumes, colors, shader, camera_type,
                                camera_view_angle_type, for_picking, ban_light);
        break;
    }
    }
}

void GLCanvas3D::render_thumbnail(ThumbnailData &                    thumbnail_data,
                                  std::vector<ColorRGBA> &           extruder_colors,
                                  unsigned int                       w,
                                  unsigned int                       h,
                                  const ThumbnailsParams &           thumbnail_params,
                                  ModelObjectPtrs &                  model_objects,
                                  const GLVolumeCollection &         volumes,
                                  Camera::EType                      camera_type,
                                  Camera::ViewAngleType              camera_view_angle_type,
                                  bool                               for_picking,
                                  bool                               ban_light)
{
    GLShaderProgram *shader = wxGetApp().get_shader("thumbnail");
    switch (OpenGLManager::get_framebuffers_type()) {
        case OpenGLManager::EFramebufferType::Arb: {
            render_thumbnail_framebuffer(thumbnail_data, w, h, thumbnail_params, wxGetApp().plater()->get_partplate_list(), model_objects, volumes, extruder_colors, shader,
                                         camera_type, camera_view_angle_type, for_picking, ban_light);
            break;
        }
        case OpenGLManager::EFramebufferType::Ext: {
            render_thumbnail_framebuffer_ext(thumbnail_data, w, h, thumbnail_params, wxGetApp().plater()->get_partplate_list(), model_objects, volumes, extruder_colors, shader,
                                             camera_type, camera_view_angle_type, for_picking, ban_light);
            break;
        }
        default: {
            render_thumbnail_legacy(thumbnail_data, w, h, thumbnail_params, wxGetApp().plater()->get_partplate_list(), model_objects, volumes, extruder_colors, shader, camera_type, camera_view_angle_type, for_picking, ban_light);
            break;
        }
    }
}

void GLCanvas3D::render_calibration_thumbnail(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params)
{
    //load current plate gcode
    m_gcode_viewer.render_calibration_thumbnail(thumbnail_data, w, h, thumbnail_params,
        wxGetApp().plater()->get_partplate_list(), wxGetApp().get_opengl_manager());
}

//BBS
void GLCanvas3D::select_curr_plate_all()
{
    m_selection.add_curr_plate();
    m_dirty = true;
}

void GLCanvas3D::select_object_from_idx(std::vector<int>& object_idxs) {
    m_selection.add_object_from_idx(object_idxs);
    m_dirty = true;
}

//BBS
void GLCanvas3D::remove_curr_plate_all()
{
    m_selection.remove_curr_plate();
    m_dirty = true;
}

void GLCanvas3D::update_plate_thumbnails()
{
    _update_imgui_select_plate_toolbar();
}

void GLCanvas3D::select_all()
{
    if (!m_gizmos.is_allow_select_all()) {
        return;
    }
    m_selection.add_all();
    m_dirty = true;
}

void GLCanvas3D::deselect_all()
{
    m_selection.remove_all();
    // BBS
    //wxGetApp().obj_manipul()->set_dirty();
    m_gizmos.reset_all_states();
    m_gizmos.update_data();
    post_event(SimpleEvent(EVT_GLCANVAS_OBJECT_SELECT));
}

void GLCanvas3D::exit_gizmo() {
    if (m_gizmos.get_current_type() != GLGizmosManager::Undefined) {
        m_gizmos.reset_all_states();
        m_gizmos.update_data();
    }
}

void GLCanvas3D::set_selected_visible(bool visible)
{
    for (unsigned int i : m_selection.get_volume_idxs()) {
        GLVolume* volume = const_cast<GLVolume*>(m_selection.get_volume(i));
        volume->visible = visible;
        volume->color.a(visible ? 1.f : GLVolume::MODEL_HIDDEN_COL.a());
        volume->render_color.a(volume->color.a());
        volume->force_native_color = !visible;
    }
}

void GLCanvas3D::delete_selected()
{
    m_selection.erase();
}

void GLCanvas3D::ensure_on_bed(unsigned int object_idx, bool allow_negative_z)
{
    //BBS if asseble view canvas
    if (m_canvas_type == ECanvasType::CanvasAssembleView) {
        return;
    }

    if (allow_negative_z)
        return;

    typedef std::map<std::pair<int, int>, double> InstancesToZMap;
    InstancesToZMap instances_min_z;

    for (GLVolume* volume : m_volumes.volumes) {
        if (volume->object_idx() == (int)object_idx && !volume->is_modifier) {
            double min_z = volume->transformed_convex_hull_bounding_box().min.z();
            std::pair<int, int> instance = std::make_pair(volume->object_idx(), volume->instance_idx());
            InstancesToZMap::iterator it = instances_min_z.find(instance);
            if (it == instances_min_z.end())
                it = instances_min_z.insert(InstancesToZMap::value_type(instance, DBL_MAX)).first;

            it->second = std::min(it->second, min_z);
        }
    }

    for (GLVolume* volume : m_volumes.volumes) {
        std::pair<int, int> instance = std::make_pair(volume->object_idx(), volume->instance_idx());
        InstancesToZMap::iterator it = instances_min_z.find(instance);
        if (it != instances_min_z.end())
            volume->set_instance_offset(Z, volume->get_instance_offset(Z) - it->second);
    }
}


const std::vector<double>& GLCanvas3D::get_gcode_layers_zs() const
{
    return m_gcode_viewer.get_layers_zs();
}

std::vector<double> GLCanvas3D::get_volumes_print_zs(bool active_only) const
{
    return m_volumes.get_current_print_zs(active_only);
}

void GLCanvas3D::set_gcode_options_visibility_from_flags(unsigned int flags)
{
    m_gcode_viewer.set_options_visibility_from_flags(flags);
}

void GLCanvas3D::set_volumes_z_range(const std::array<double, 2>& range)
{
    m_volumes.set_range(range[0] - 1e-6, range[1] + 1e-6);
}

std::vector<int> GLCanvas3D::load_object(const ModelObject& model_object, int obj_idx, std::vector<int> instance_idxs)
{
    if (instance_idxs.empty()) {
        for (unsigned int i = 0; i < model_object.instances.size(); ++i) {
            instance_idxs.emplace_back(i);
        }
    }
    return m_volumes.load_object(&model_object, obj_idx, instance_idxs, m_color_by, m_initialized);
}

std::vector<int> GLCanvas3D::load_object(const Model& model, int obj_idx)
{
    if (0 <= obj_idx && obj_idx < (int)model.objects.size()) {
        const ModelObject* model_object = model.objects[obj_idx];
        if (model_object != nullptr)
            return load_object(*model_object, obj_idx, std::vector<int>());
    }

    return std::vector<int>();
}

void GLCanvas3D::mirror_selection(Axis axis)
{
    TransformationType transformation_type;
    //transformation_type.set_world();
    transformation_type.set_relative();
    m_selection.setup_cache();
    m_selection.mirror(axis, transformation_type);
    do_mirror(L("Mirror Object"));
    // BBS
    //wxGetApp().obj_manipul()->set_dirty();
}

// Reload the 3D scene of
// 1) Model / ModelObjects / ModelInstances / ModelVolumes
// 2) Print bed
// 3) SLA support meshes for their respective ModelObjects / ModelInstances
// 4) Wipe tower preview
// 5) Out of bed collision status & message overlay (texture)
void GLCanvas3D::reload_scene(bool refresh_immediately, bool force_full_scene_refresh)
{
    if (m_canvas == nullptr || m_config == nullptr || m_model == nullptr)
        return;

    if (!m_initialized)
        return;

    _set_current();

    m_hover_volume_idxs.clear();

    struct ModelVolumeState {
        ModelVolumeState(const GLVolume* volume) :
            model_volume(nullptr), geometry_id(volume->geometry_id), volume_idx(-1) {}
        ModelVolumeState(const ModelVolume* model_volume, const ObjectID& instance_id, const GLVolume::CompositeID& composite_id) :
            model_volume(model_volume), geometry_id(std::make_pair(model_volume->id().id, instance_id.id)), composite_id(composite_id), volume_idx(-1) {}
        ModelVolumeState(const ObjectID& volume_id, const ObjectID& instance_id) :
            model_volume(nullptr), geometry_id(std::make_pair(volume_id.id, instance_id.id)), volume_idx(-1) {}
        bool new_geometry() const { return this->volume_idx == size_t(-1); }
        const ModelVolume* model_volume;
        // ObjectID of ModelVolume + ObjectID of ModelInstance
        // or timestamp of an SLAPrintObjectStep + ObjectID of ModelInstance
        std::pair<size_t, size_t>   geometry_id;
        GLVolume::CompositeID       composite_id;
        // Volume index in the new GLVolume vector.
        size_t                      volume_idx;
    };
    std::vector<ModelVolumeState> model_volume_state;
    std::vector<ModelVolumeState> aux_volume_state;

    struct GLVolumeState {
        GLVolumeState() :
            volume_idx(size_t(-1)) {}
        GLVolumeState(const GLVolume* volume, unsigned int volume_idx) :
            composite_id(volume->composite_id), volume_idx(volume_idx) {}
        GLVolumeState(const GLVolume::CompositeID &composite_id) :
            composite_id(composite_id), volume_idx(size_t(-1)) {}

        GLVolume::CompositeID       composite_id;
        // Volume index in the old GLVolume vector.
        size_t                      volume_idx;
    };

    // SLA steps to pull the preview meshes for.
	typedef std::array<SLAPrintObjectStep, 3> SLASteps;
    SLASteps sla_steps = { slaposDrillHoles, slaposSupportTree, slaposPad };
    struct SLASupportState {
        std::array<PrintStateBase::StateWithTimeStamp, std::tuple_size<SLASteps>::value> step;
    };
    // State of the sla_steps for all SLAPrintObjects.
    std::vector<SLASupportState>   sla_support_state;

    std::vector<size_t> instance_ids_selected;
    std::vector<size_t> map_glvolume_old_to_new(m_volumes.volumes.size(), size_t(-1));
    std::vector<GLVolumeState> deleted_volumes;
    // BBS
    std::vector<GLVolumeState> deleted_wipe_towers;
    std::vector<GLVolume*> glvolumes_new;
    glvolumes_new.reserve(m_volumes.volumes.size());
    auto model_volume_state_lower = [](const ModelVolumeState& m1, const ModelVolumeState& m2) { return m1.geometry_id < m2.geometry_id; };

    m_reload_delayed = !m_canvas->IsShown() && !refresh_immediately && !force_full_scene_refresh;

    PrinterTechnology printer_technology = current_printer_technology();

    // BBS: support wipe tower for multi-plates
    PartPlateList& ppl = wxGetApp().plater()->get_partplate_list();
    int n_plates = ppl.get_plate_count();
    std::vector<int> volume_idxs_wipe_tower_old(n_plates, -1);

    // Release invalidated volumes to conserve GPU memory in case of delayed refresh (see m_reload_delayed).
    // First initialize model_volumes_new_sorted & model_instances_new_sorted.
    for (int object_idx = 0; object_idx < (int)m_model->objects.size(); ++object_idx) {
        const ModelObject* model_object = m_model->objects[object_idx];
        for (int instance_idx = 0; instance_idx < (int)model_object->instances.size(); ++instance_idx) {
            const ModelInstance* model_instance = model_object->instances[instance_idx];
            for (int volume_idx = 0; volume_idx < (int)model_object->volumes.size(); ++volume_idx) {
                const ModelVolume* model_volume = model_object->volumes[volume_idx];
                if (m_canvas_type == ECanvasType::CanvasAssembleView) {
                    if (model_volume->is_model_part())
                        model_volume_state.emplace_back(model_volume, model_instance->id(), GLVolume::CompositeID(object_idx, volume_idx, instance_idx));
                }
                else {
                    model_volume_state.emplace_back(model_volume, model_instance->id(), GLVolume::CompositeID(object_idx, volume_idx, instance_idx));
                }
            }
        }
    }
    if (printer_technology == ptSLA) {
        const SLAPrint* sla_print = this->sla_print();
#ifndef NDEBUG
        // Verify that the SLAPrint object is synchronized with m_model.
        check_model_ids_equal(*m_model, sla_print->model());
#endif /* NDEBUG */
        sla_support_state.reserve(sla_print->objects().size());
        for (const SLAPrintObject* print_object : sla_print->objects()) {
            SLASupportState state;
            for (size_t istep = 0; istep < sla_steps.size(); ++istep) {
                state.step[istep] = print_object->step_state_with_timestamp(sla_steps[istep]);
                if (state.step[istep].state == PrintStateBase::DONE) {
                    if (!print_object->has_mesh(sla_steps[istep]))
                        // Consider the DONE step without a valid mesh as invalid for the purpose
                        // of mesh visualization.
                        state.step[istep].state = PrintStateBase::INVALID;
                    else if (sla_steps[istep] != slaposDrillHoles)
                        for (const ModelInstance* model_instance : print_object->model_object()->instances)
                            // Only the instances, which are currently printable, will have the SLA support structures kept.
                            // The instances outside the print bed will have the GLVolumes of their support structures released.
                            if (model_instance->is_printable())
                                aux_volume_state.emplace_back(state.step[istep].timestamp, model_instance->id());
                }
            }
            sla_support_state.emplace_back(state);
        }
    }
    std::sort(model_volume_state.begin(), model_volume_state.end(), model_volume_state_lower);
    std::sort(aux_volume_state.begin(), aux_volume_state.end(), model_volume_state_lower);

    // BBS: normalize painting data with current filament count
    for (unsigned int obj_idx = 0; obj_idx < (unsigned int)m_model->objects.size(); ++obj_idx) {
        const ModelObject& model_object = *m_model->objects[obj_idx];
        for (int volume_idx = 0; volume_idx < (int)model_object.volumes.size(); ++volume_idx) {
            ModelVolume& model_volume = *model_object.volumes[volume_idx];
            if (!model_volume.is_model_part())
                continue;

            unsigned int filaments_count = (unsigned int)dynamic_cast<const ConfigOptionStrings*>(m_config->option("filament_colour"))->values.size();
            model_volume.update_extruder_count(filaments_count);
        }
    }

    // Release all ModelVolume based GLVolumes not found in the current Model. Find the GLVolume of a hollowed mesh.
    for (size_t volume_id = 0; volume_id < m_volumes.volumes.size(); ++volume_id) {
        GLVolume* volume = m_volumes.volumes[volume_id];
        ModelVolumeState  key(volume);
        ModelVolumeState* mvs = nullptr;
        if (volume->volume_idx() < 0) {
            auto it = std::lower_bound(aux_volume_state.begin(), aux_volume_state.end(), key, model_volume_state_lower);
            if (it != aux_volume_state.end() && it->geometry_id == key.geometry_id)
                // This can be an SLA support structure that should not be rendered (in case someone used undo
                // to revert to before it was generated). We only reuse the volume if that's not the case.
                if (m_model->objects[volume->composite_id.object_id]->sla_points_status != sla::PointsStatus::NoPoints)
                    mvs = &(*it);
        }
        else {
            auto it = std::lower_bound(model_volume_state.begin(), model_volume_state.end(), key, model_volume_state_lower);
            if (it != model_volume_state.end() && it->geometry_id == key.geometry_id)
                mvs = &(*it);
        }
        // Emplace instance ID of the volume. Both the aux volumes and model volumes share the same instance ID.
        // The wipe tower has its own wipe_tower_instance_id().
        if (m_selection.contains_volume(volume_id)) {
            if (m_canvas_type == ECanvasType::CanvasAssembleView) {
                if (!volume->is_modifier)
                    instance_ids_selected.emplace_back(volume->geometry_id.second);
            }
            else {
                instance_ids_selected.emplace_back(volume->geometry_id.second);
            }
        }
        if (mvs == nullptr || force_full_scene_refresh) {
            // This GLVolume will be released.
            if (volume->is_wipe_tower) {
                // There is only one wipe tower.
                //assert(volume_idx_wipe_tower_old == -1);
                int plate_id = volume->composite_id.object_id - 1000;
                if (plate_id < n_plates)
                    volume_idxs_wipe_tower_old[plate_id] = (int)volume_id;
            }
            if (!m_reload_delayed) {
                deleted_volumes.emplace_back(volume, volume_id);
                // BBS
                if (volume->is_wipe_tower)
                    deleted_wipe_towers.emplace_back(volume, volume_id);
                delete volume;
            }
        }
        else {
            // This GLVolume will be reused.
            volume->set_sla_shift_z(0.0);
            map_glvolume_old_to_new[volume_id] = glvolumes_new.size();
            mvs->volume_idx = glvolumes_new.size();
            glvolumes_new.emplace_back(volume);
            // Update color of the volume based on the current extruder.
            if (mvs->model_volume != nullptr) {
                int extruder_id = mvs->model_volume->extruder_id();
                if (extruder_id != -1)
                    volume->extruder_id = extruder_id;

                volume->is_modifier = !mvs->model_volume->is_model_part();
                volume->set_color(color_from_model_volume(*mvs->model_volume));

                // updates volumes transformations
                if (m_canvas_type != ECanvasType::CanvasAssembleView) {
                    volume->set_instance_transformation(mvs->model_volume->get_object()->instances[mvs->composite_id.instance_id]->get_transformation());
                    volume->set_volume_transformation(mvs->model_volume->get_transformation());
                    // updates volumes convex hull
                    if (mvs->model_volume->is_model_part() && ! volume->convex_hull())
                        // Model volume was likely changed from modifier or support blocker / enforcer to a model part.
                        // Only model parts require convex hulls.
                        volume->set_convex_hull(mvs->model_volume->get_convex_hull_shared_ptr());
                    volume->set_offset_to_assembly(Vec3d(0, 0, 0));
                }
                else {
                    volume->set_instance_transformation(mvs->model_volume->get_object()->instances[mvs->composite_id.instance_id]->get_assemble_transformation());
                    volume->set_volume_transformation(mvs->model_volume->get_transformation());
                    // updates volumes convex hull
                    if (mvs->model_volume->is_model_part() && ! volume->convex_hull())
                        // Model volume was likely changed from modifier or support blocker / enforcer to a model part.
                        // Only model parts require convex hulls.
                        volume->set_convex_hull(mvs->model_volume->get_convex_hull_shared_ptr());
                    volume->set_offset_to_assembly(mvs->model_volume->get_object()->instances[mvs->composite_id.instance_id]->get_offset_to_assembly());
                }
            }
        }
    }
    sort_remove_duplicates(instance_ids_selected);
    auto deleted_volumes_lower = [](const GLVolumeState &v1, const GLVolumeState &v2) { return v1.composite_id < v2.composite_id; };
    std::sort(deleted_volumes.begin(), deleted_volumes.end(), deleted_volumes_lower);

    //BBS clean hover_volume_idxs
    m_hover_volume_idxs.clear();

    if (m_reload_delayed)
        return;

    // BBS: do not check wipe tower changes
    bool update_object_list = false;
    if (deleted_volumes.size() != deleted_wipe_towers.size())
        update_object_list = true;

    if (m_volumes.volumes != glvolumes_new && !update_object_list) {
        int vol_idx = 0;
        for (; vol_idx < std::min(m_volumes.volumes.size(), glvolumes_new.size()); vol_idx++) {
            if (m_volumes.volumes[vol_idx] != glvolumes_new[vol_idx]) {
                update_object_list = true;
                break;
            }
        }
        for (int temp_idx = vol_idx; temp_idx < m_volumes.volumes.size() && !update_object_list; temp_idx++) {
            // Volumes in m_volumes might not exist anymore, so we cannot
            // directly check if they are is_wipe_towers, for which we do
            // not want to update the object list.  Instead, we do a kind of
            // slow thing of seeing if they were in the deleted list, and if
            // so, if they were a wipe tower.
            bool was_deleted_wipe_tower = false;
            for (int del_idx = 0; del_idx < deleted_wipe_towers.size(); del_idx++) {
                if (deleted_wipe_towers[del_idx].volume_idx == temp_idx) {
                    was_deleted_wipe_tower = true;
                    break;
                }
            }
            if (!was_deleted_wipe_tower) {
                update_object_list = true;
            }
        }
        for (int temp_idx = vol_idx; temp_idx < glvolumes_new.size() && !update_object_list; temp_idx++) {
            if (!glvolumes_new[temp_idx]->is_wipe_tower)
                update_object_list = true;
        }
    }
    m_volumes.volumes = std::move(glvolumes_new);
    for (unsigned int obj_idx = 0; obj_idx < (unsigned int)m_model->objects.size(); ++ obj_idx) {
        const ModelObject &model_object = *m_model->objects[obj_idx];
        for (int volume_idx = 0; volume_idx < (int)model_object.volumes.size(); ++ volume_idx) {
			const ModelVolume &model_volume = *model_object.volumes[volume_idx];
            if (m_canvas_type == ECanvasType::CanvasAssembleView && !model_volume.is_model_part())
                continue;
            for (int instance_idx = 0; instance_idx < (int)model_object.instances.size(); ++ instance_idx) {
				const ModelInstance &model_instance = *model_object.instances[instance_idx];
				ModelVolumeState key(model_volume.id(), model_instance.id());
				auto it = std::lower_bound(model_volume_state.begin(), model_volume_state.end(), key, model_volume_state_lower);
				assert(it != model_volume_state.end() && it->geometry_id == key.geometry_id);
                if (it->new_geometry()) {
                    // New volume.
                    auto it_old_volume = std::lower_bound(deleted_volumes.begin(), deleted_volumes.end(), GLVolumeState(it->composite_id), deleted_volumes_lower);
                    if (it_old_volume != deleted_volumes.end() && it_old_volume->composite_id == it->composite_id)
                        // If a volume changed its ObjectID, but it reuses a GLVolume's CompositeID, maintain its selection.
                        map_glvolume_old_to_new[it_old_volume->volume_idx] = m_volumes.volumes.size();
                    // Note the index of the loaded volume, so that we can reload the main model GLVolume with the hollowed mesh
                    // later in this function.
                    it->volume_idx = m_volumes.volumes.size();
                    m_volumes.load_object_volume(&model_object, obj_idx, volume_idx, instance_idx, m_color_by, m_initialized, m_canvas_type == ECanvasType::CanvasAssembleView);
                    m_volumes.volumes.back()->geometry_id = key.geometry_id;
                    update_object_list = true;
                } else {
					// Recycling an old GLVolume.
					GLVolume &existing_volume = *m_volumes.volumes[it->volume_idx];
                    assert(existing_volume.geometry_id == key.geometry_id);
					// Update the Object/Volume/Instance indices into the current Model.
					if (existing_volume.composite_id != it->composite_id) {
						existing_volume.composite_id = it->composite_id;
						update_object_list = true;
					}
                }
            }
        }
    }
    if (printer_technology == ptSLA) {
        size_t idx = 0;
        const SLAPrint *sla_print = this->sla_print();
		std::vector<double> shift_zs(m_model->objects.size(), 0);
        double relative_correction_z = sla_print->relative_correction().z();
        if (relative_correction_z <= EPSILON)
            relative_correction_z = 1.;
		for (const SLAPrintObject *print_object : sla_print->objects()) {
            SLASupportState   &state        = sla_support_state[idx ++];
            const ModelObject *model_object = print_object->model_object();
            // Find an index of the ModelObject
            int object_idx;
            // There may be new SLA volumes added to the scene for this print_object.
            // Find the object index of this print_object in the Model::objects list.
            auto it = std::find(sla_print->model().objects.begin(), sla_print->model().objects.end(), model_object);
            assert(it != sla_print->model().objects.end());
			object_idx = it - sla_print->model().objects.begin();
			// Cache the Z offset to be applied to all volumes with this object_idx.
			shift_zs[object_idx] = print_object->get_current_elevation() / relative_correction_z;
            // Collect indices of this print_object's instances, for which the SLA support meshes are to be added to the scene.
            // pairs of <instance_idx, print_instance_idx>
			std::vector<std::pair<size_t, size_t>> instances[std::tuple_size<SLASteps>::value];
            for (size_t print_instance_idx = 0; print_instance_idx < print_object->instances().size(); ++ print_instance_idx) {
                const SLAPrintObject::Instance &instance = print_object->instances()[print_instance_idx];
                // Find index of ModelInstance corresponding to this SLAPrintObject::Instance.
				auto it = std::find_if(model_object->instances.begin(), model_object->instances.end(),
                    [&instance](const ModelInstance *mi) { return mi->id() == instance.instance_id; });
                assert(it != model_object->instances.end());
                int instance_idx = it - model_object->instances.begin();
                for (size_t istep = 0; istep < sla_steps.size(); ++ istep)
                    if (sla_steps[istep] == slaposDrillHoles) {
                    	// Hollowing is a special case, where the mesh from the backend is being loaded into the 1st volume of an instance,
                    	// not into its own GLVolume.
                        // There shall always be such a GLVolume allocated.
                        ModelVolumeState key(model_object->volumes.front()->id(), instance.instance_id);
                        auto it = std::lower_bound(model_volume_state.begin(), model_volume_state.end(), key, model_volume_state_lower);
                        assert(it != model_volume_state.end() && it->geometry_id == key.geometry_id);
                        assert(!it->new_geometry());
                        GLVolume &volume = *m_volumes.volumes[it->volume_idx];
                        if (! volume.offsets.empty() && state.step[istep].timestamp != volume.offsets.front()) {
                        	// The backend either produced a new hollowed mesh, or it invalidated the one that the front end has seen.
                            volume.model.reset();
                            if (state.step[istep].state == PrintStateBase::DONE) {
                                TriangleMesh mesh = print_object->get_mesh(slaposDrillHoles);
	                            assert(! mesh.empty());
                                mesh.transform(sla_print->sla_trafo(*m_model->objects[volume.object_idx()]).inverse());
#if ENABLE_SMOOTH_NORMALS
                                volume.model.init_from(mesh, true);
#else
                                volume.model.init_from(mesh);
                                volume.mesh_raycaster = std::make_unique<GUI::MeshRaycaster>(std::make_shared<TriangleMesh>(mesh));
#endif // ENABLE_SMOOTH_NORMALS
                            }
                            else {
	                        	// Reload the original volume.
#if ENABLE_SMOOTH_NORMALS
                                volume.model.init_from(m_model->objects[volume.object_idx()]->volumes[volume.volume_idx()]->mesh(), true);
#else
                                const TriangleMesh& new_mesh = m_model->objects[volume.object_idx()]->volumes[volume.volume_idx()]->mesh();
                                volume.model.init_from(new_mesh);
                                volume.mesh_raycaster = std::make_unique<GUI::MeshRaycaster>(std::make_shared<TriangleMesh>(new_mesh));
#endif // ENABLE_SMOOTH_NORMALS
                            }
	                    }
                    	//FIXME it is an ugly hack to write the timestamp into the "offsets" field to not have to add another member variable
                    	// to the GLVolume. We should refactor GLVolume significantly, so that the GLVolume will not contain member variables
                    	// of various concenrs (model vs. 3D print path).
                    	volume.offsets = { state.step[istep].timestamp };
                    }
                    else if (state.step[istep].state == PrintStateBase::DONE) {
                        // Check whether there is an existing auxiliary volume to be updated, or a new auxiliary volume to be created.
						ModelVolumeState key(state.step[istep].timestamp, instance.instance_id.id);
						auto it = std::lower_bound(aux_volume_state.begin(), aux_volume_state.end(), key, model_volume_state_lower);
						assert(it != aux_volume_state.end() && it->geometry_id == key.geometry_id);
                    	if (it->new_geometry()) {
                            // This can be an SLA support structure that should not be rendered (in case someone used undo
                            // to revert to before it was generated). If that's the case, we should not generate anything.
                            if (model_object->sla_points_status != sla::PointsStatus::NoPoints)
                                instances[istep].emplace_back(std::pair<size_t, size_t>(instance_idx, print_instance_idx));
                            else
                                shift_zs[object_idx] = 0.;
                        }
                        else {
                            // Recycling an old GLVolume. Update the Object/Instance indices into the current Model.
                            m_volumes.volumes[it->volume_idx]->composite_id = GLVolume::CompositeID(object_idx, m_volumes.volumes[it->volume_idx]->volume_idx(), instance_idx);
                            m_volumes.volumes[it->volume_idx]->set_instance_transformation(model_object->instances[instance_idx]->get_transformation());
                        }
                    }
            }

            for (size_t istep = 0; istep < sla_steps.size(); ++istep)
                if (!instances[istep].empty())
                    m_volumes.load_object_auxiliary(print_object, object_idx, instances[istep], sla_steps[istep], state.step[istep].timestamp);
        }

		// Shift-up all volumes of the object so that it has the right elevation with respect to the print bed
		for (GLVolume* volume : m_volumes.volumes)
			if (volume->object_idx() < (int)m_model->objects.size() && m_model->objects[volume->object_idx()]->instances[volume->instance_idx()]->is_printable())
				volume->set_sla_shift_z(shift_zs[volume->object_idx()]);
    }

    // BBS
    if (printer_technology == ptFFF && m_config->has("filament_colour") && (m_canvas_type != ECanvasType::CanvasAssembleView)) {
        // Should the wipe tower be visualized ?
        unsigned int filaments_count = (unsigned int)dynamic_cast<const ConfigOptionStrings*>(m_config->option("filament_colour"))->values.size();

        bool wt = dynamic_cast<const ConfigOptionBool*>(m_config->option("enable_prime_tower"))->value;
        auto co = dynamic_cast<const ConfigOptionEnum<PrintSequence>*>(m_config->option<ConfigOptionEnum<PrintSequence>>("print_sequence"));

        const DynamicPrintConfig &dconfig           = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        auto timelapse_type = dconfig.option<ConfigOptionEnum<TimelapseType>>("timelapse_type");
        bool need_wipe_tower = timelapse_type ? (timelapse_type->value == TimelapseType::tlSmooth) : false;

        if (dconfig.has("enable_wrapping_detection")) {
            need_wipe_tower |= dynamic_cast<const ConfigOptionBool*>(dconfig.option("enable_wrapping_detection"))->value;
        }

        if (wt && (need_wipe_tower || filaments_count > 1) && !wxGetApp().plater()->only_gcode_mode() && !wxGetApp().plater()->is_gcode_3mf()) {
            for (int plate_id = 0; plate_id < n_plates; plate_id++) {
                // If print ByObject and there is only one object in the plate, the wipe tower is allowed to be generated.
                PartPlate* part_plate = ppl.get_plate(plate_id);
                if (part_plate->get_print_seq() == PrintSequence::ByObject ||
                    (part_plate->get_print_seq() == PrintSequence::ByDefault && co != nullptr && co->value == PrintSequence::ByObject)) {
                    if (ppl.get_plate(plate_id)->printable_instance_size() != 1)
                        continue;
                }

                DynamicPrintConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
                float x = dynamic_cast<const ConfigOptionFloats*>(proj_cfg.option("wipe_tower_x"))->get_at(plate_id);
                float y = dynamic_cast<const ConfigOptionFloats*>(proj_cfg.option("wipe_tower_y"))->get_at(plate_id);
                float w = dynamic_cast<const ConfigOptionFloat*>(m_config->option("prime_tower_width"))->value;
                float a = dynamic_cast<const ConfigOptionFloat*>(proj_cfg.option("wipe_tower_rotation_angle"))->value;
                // BBS
                float v = dynamic_cast<const ConfigOptionFloat*>(m_config->option("prime_volume"))->value;
                Vec3d plate_origin = ppl.get_plate(plate_id)->get_origin();

                const Print* print = m_process->fff_print();
                const Print* current_print = part_plate->fff_print();
                if (!need_wipe_tower && part_plate->get_extruders(true).size() < 2) continue;
                if (part_plate->get_objects_on_this_plate().empty()) continue;

                float brim_width = print->wipe_tower_data(filaments_count).brim_width;
                const DynamicPrintConfig &print_cfg   = wxGetApp().preset_bundle->prints.get_edited_preset().config;
                int nozzle_nums = wxGetApp().preset_bundle->get_printer_extruder_count();
                Vec3d wipe_tower_size = ppl.get_plate(plate_id)->estimate_wipe_tower_size(print_cfg, w, v, nozzle_nums, 0, false, dynamic_cast<const ConfigOptionBool*>(dconfig.option("enable_wrapping_detection"))->value);

                {
                    const float                 margin     = WIPE_TOWER_MARGIN + brim_width;
                    BoundingBoxf3               plate_bbox = part_plate->get_bounding_box();
                    BoundingBoxf                plate_bbox_2d(Vec2d(plate_bbox.min(0), plate_bbox.min(1)), Vec2d(plate_bbox.max(0), plate_bbox.max(1)));
                    const std::vector<Pointfs> &extruder_areas = part_plate->get_extruder_areas();
                    for (Pointfs points : extruder_areas) {
                        BoundingBoxf bboxf(points);
                        plate_bbox_2d.min = plate_bbox_2d.min(0) >= bboxf.min(0) ? plate_bbox_2d.min : bboxf.min;
                        plate_bbox_2d.max = plate_bbox_2d.max(0) <= bboxf.max(0) ? plate_bbox_2d.max : bboxf.max;
                    }

                    coordf_t plate_bbox_x_min_local_coord = plate_bbox_2d.min(0) - plate_origin(0);
                    coordf_t plate_bbox_x_max_local_coord = plate_bbox_2d.max(0) - plate_origin(0);
                    coordf_t plate_bbox_y_max_local_coord = plate_bbox_2d.max(1) - plate_origin(1);

                    if (!current_print->is_step_done(psWipeTower) || !current_print->wipe_tower_data().wipe_tower_mesh_data) {
                        // update for wipe tower position
                        {
                            int volume_idx_wipe_tower_new = m_volumes.load_wipe_tower_preview(1000 + plate_id, x + plate_origin(0), y + plate_origin(1),
                                                                                              (float) wipe_tower_size(0), (float) wipe_tower_size(1), (float) wipe_tower_size(2),
                                                                                              a,
                                                                                              /*!print->is_step_done(psWipeTower)*/ true, brim_width);
                            int volume_idx_wipe_tower_old = volume_idxs_wipe_tower_old[plate_id];
                            if (volume_idx_wipe_tower_old != -1) map_glvolume_old_to_new[volume_idx_wipe_tower_old] = volume_idx_wipe_tower_new;
                        }
                    } else {
                        const float margin                    = 2.f;
                        auto        tower_bottom = current_print->wipe_tower_data().wipe_tower_mesh_data->bottom;
                        tower_bottom.translate(scaled(Vec2d{x, y}));
                        tower_bottom.translate(scaled(Vec2d{plate_origin[0], plate_origin[1]}));
                        auto tower_bottom_bbox = get_extents(tower_bottom);
                        BoundingBoxf3 plate_bbox        = wxGetApp().plater()->get_partplate_list().get_plate(plate_id)->get_build_volume(true);
                        BoundingBox   plate_bbox2d      = BoundingBox(scaled(Vec2f(plate_bbox.min[0], plate_bbox.min[1])), scaled(Vec2f(plate_bbox.max[0], plate_bbox.max[1])));
                        Vec2f         offset            = WipeTower::move_box_inside_box(tower_bottom_bbox, plate_bbox2d, scaled(margin));
                        int volume_idx_wipe_tower_new = m_volumes.load_real_wipe_tower_preview(1000 + plate_id, x + plate_origin(0), y + plate_origin(1),
                                                                                               current_print->wipe_tower_data().wipe_tower_mesh_data->real_wipe_tower_mesh,
                                                                                               current_print->wipe_tower_data().wipe_tower_mesh_data->real_brim_mesh,
                                                                                            true,a,/*!print->is_step_done(psWipeTower)*/ true, m_initialized);
                        int volume_idx_wipe_tower_old = volume_idxs_wipe_tower_old[plate_id];
                        if (volume_idx_wipe_tower_old != -1) map_glvolume_old_to_new[volume_idx_wipe_tower_old] = volume_idx_wipe_tower_new;
                    }
                }
            }
        }
    }

    update_volumes_colors_by_extruder();
	// Update selection indices based on the old/new GLVolumeCollection.
    if (m_selection.get_mode() == Selection::Instance)
        m_selection.instances_changed(instance_ids_selected);
    else
        m_selection.volumes_changed(map_glvolume_old_to_new);

    // @Enrico suggest this solution to preven accessing pointer on caster without data
    m_scene_raycaster.remove_raycasters(SceneRaycaster::EType::Bed);
    m_scene_raycaster.remove_raycasters(SceneRaycaster::EType::Volume);
    m_gizmos.update_data();
    m_gizmos.update_assemble_view_data();
    m_gizmos.refresh_on_off_state();

    // Update the toolbar
    //BBS: notify the PartPlateList to reload all objects
    if (update_object_list)
        post_event(SimpleEvent(EVT_GLCANVAS_OBJECT_SELECT));

    //BBS:exclude the assmble view
    if (m_canvas_type != ECanvasType::CanvasAssembleView) {
        _update_slice_error_status();
        // checks for geometry outside the print volume to render it accordingly
        if (!m_volumes.empty()) {
            ModelInstanceEPrintVolumeState state;
            ObjectFilamentResults object_results;
            const bool contained_min_one = m_volumes.check_outside_state(m_bed.build_volume(), &state, &object_results);
            const bool partlyOut = (state == ModelInstanceEPrintVolumeState::ModelInstancePVS_Partly_Outside);
            const bool fullyOut = (state == ModelInstanceEPrintVolumeState::ModelInstancePVS_Fully_Outside);
           // const bool objectLimited = (state == ModelInstanceEPrintVolumeState::ModelInstancePVS_Limited);

            PartPlate *cur_plate  = wxGetApp().plater()->get_partplate_list().get_curr_plate();
            bool show_read_wipe_tower = cur_plate->fff_print()->is_step_done(psWipeTower);
            bool       wipe_tower_outside   = m_volumes.check_wipe_tower_outside_state(m_bed.build_volume(), wxGetApp().plater()->get_partplate_list().get_curr_plate_index());
            bool show_wipe_tower_outside_error = show_read_wipe_tower ? !wipe_tower_outside : false;
            _set_warning_notification(EWarning::PrimeTowerOutside, show_wipe_tower_outside_error);

            auto clash_flag = construct_error_string(object_results, get_object_clashed_text());
            auto unprintable_flag= construct_extruder_unprintable_error(object_results, get_left_extruder_unprintable_text(), get_right_extruder_unprintable_text());

            bool is_flushing_volume_valid = is_flushing_matrix_error();
            _set_warning_notification(EWarning::FlushingVolumeZero, is_flushing_volume_valid);
            _set_warning_notification(EWarning::ObjectClashed, clash_flag);
            _set_warning_notification(EWarning::LeftExtruderPrintableError, unprintable_flag.first);
            _set_warning_notification(EWarning::RightExtruderPrintableError, unprintable_flag.second);
            //_set_warning_notification(EWarning::ObjectLimited, objectLimited);
            //BBS: turn off the warning when fully outside
            //_set_warning_notification(EWarning::ObjectOutside, fullyOut);
            //if (printer_technology != ptSLA || !contained_min_one)
            //    _set_warning_notification(EWarning::SlaSupportsOutside, false);

            auto full_config_temp = wxGetApp().preset_bundle->full_config();
            bool tpu_valid = cur_plate->check_tpu_printable_status(full_config_temp, wxGetApp().preset_bundle->get_used_tpu_filaments(cur_plate->get_extruders(true)));
            _set_warning_notification(EWarning::TPUPrintableError, !tpu_valid);

            bool filament_printable = cur_plate->check_filament_printable(full_config_temp, filament_printable_error_msg);
            _set_warning_notification(EWarning::FilamentPrintableError, !filament_printable);

            bool mix_pla_and_petg = cur_plate->check_mixture_of_pla_and_petg(full_config_temp);
            _set_warning_notification(EWarning::MixUsePLAAndPETG, !mix_pla_and_petg);

            bool filament_nozzle_compatible = cur_plate->check_compatible_of_nozzle_and_filament(full_config_temp, wxGetApp().preset_bundle->filament_presets, get_nozzle_filament_incompatible_text());
            _set_warning_notification(EWarning::NozzleFilamentIncompatible, !filament_nozzle_compatible);

            bool filament_mixture_compatible = cur_plate->check_mixture_filament_compatible(full_config_temp, get_filament_mixture_warning_text());
            _set_warning_notification(EWarning::MixtureFilamentIncompatible, !filament_mixture_compatible);

            bool model_fits = contained_min_one && !m_model->objects.empty() && !partlyOut && object_results.filaments.empty() && tpu_valid && filament_printable;
            post_event(Event<bool>(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, model_fits));
            ppl.get_curr_plate()->update_slice_ready_status(model_fits);
        }
        else {
            _set_warning_notification(EWarning::ObjectOutside, false);
            _set_warning_notification(EWarning::FlushingVolumeZero, false);
            _set_warning_notification(EWarning::ObjectClashed, false);
            _set_warning_notification(EWarning::LeftExtruderPrintableError, false);
            _set_warning_notification(EWarning::RightExtruderPrintableError, false);
            //_set_warning_notification(EWarning::ObjectLimited, false);
            //_set_warning_notification(EWarning::SlaSupportsOutside, false);
           _set_warning_notification(EWarning::TPUPrintableError, false);
           _set_warning_notification(EWarning::FilamentPrintableError, false);
           _set_warning_notification(EWarning::MixUsePLAAndPETG, false);
           _set_warning_notification(EWarning::PrimeTowerOutside, false);
           _set_warning_notification(EWarning::MultiExtruderPrintableError,false);
           _set_warning_notification(EWarning::MultiExtruderHeightOutside,false);
           _set_warning_notification(EWarning::NozzleFilamentIncompatible,false);
           _set_warning_notification(EWarning::MixtureFilamentIncompatible,false);

           post_event(Event<bool>(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, false));
        }
    }

    refresh_camera_scene_box();

    if (m_selection.is_empty()) {
        // If no object is selected, deactivate the active gizmo, if any
        // Otherwise it may be shown after cleaning the scene (if it was active while the objects were deleted)
        m_gizmos.reset_all_states();
        // BBS
#if 0
        // If no object is selected, reset the objects manipulator on the sidebar
        // to force a reset of its cache
        auto manip = wxGetApp().obj_manipul();
        if (manip != nullptr)
            manip->set_dirty();
#endif
    }

    // refresh bed raycasters for picking
    if (m_canvas_type == ECanvasType::CanvasView3D) {
        wxGetApp().plater()->get_partplate_list().register_raycasters_for_picking(*this);
    }

    // refresh volume raycasters for picking
    for (size_t i = 0; i < m_volumes.volumes.size(); ++i) {
        const GLVolume* v = m_volumes.volumes[i];
        assert(v->mesh_raycaster != nullptr);
        std::shared_ptr<SceneRaycasterItem> raycaster = add_raycaster_for_picking(SceneRaycaster::EType::Volume, i, *v->mesh_raycaster, v->world_matrix());
        raycaster->set_active(v->is_active);
    }

    // refresh gizmo elements raycasters for picking
    GLGizmoBase* curr_gizmo = m_gizmos.get_current();
    if (curr_gizmo != nullptr)
        curr_gizmo->unregister_raycasters_for_picking();
    m_scene_raycaster.remove_raycasters(SceneRaycaster::EType::Gizmo);
    m_scene_raycaster.remove_raycasters(SceneRaycaster::EType::FallbackGizmo);
    if (curr_gizmo != nullptr && !m_selection.is_empty())
        curr_gizmo->register_raycasters_for_picking();

    // and force this canvas to be redrawn.
    m_dirty = true;
}

void GLCanvas3D::load_shells(const Print& print, bool force_previewing)
{
    if (m_initialized)
    {
        m_gcode_viewer.load_shells(print, m_initialized, force_previewing);
        m_gcode_viewer.update_shells_color_by_extruder(m_config);
    }
}

void GLCanvas3D::set_shell_transparence(float alpha){
    m_gcode_viewer.set_shell_transparency(alpha);

}
//BBS: add only gcode mode
void GLCanvas3D::load_gcode_preview(const GCodeProcessorResult& gcode_result, const std::vector<std::string>& str_tool_colors, bool only_gcode)
{
    PartPlateList& partplate_list = wxGetApp().plater()->get_partplate_list();
    PartPlate* plate = partplate_list.get_curr_plate();
    const std::vector<BoundingBoxf3>& exclude_bounding_box = plate->get_exclude_areas();

    //BBS: init is called in GLCanvas3D.render()
    //when load gcode directly, it is too late
    m_gcode_viewer.init(wxGetApp().get_mode(), wxGetApp().preset_bundle);
    m_gcode_viewer.load(gcode_result, *this->fff_print(), wxGetApp().plater()->build_volume(), exclude_bounding_box,
        wxGetApp().get_mode(), only_gcode);
    m_gcode_viewer.get_moves_slider()->SetHigherValue(m_gcode_viewer.get_moves_slider()->GetMaxValue());

    if (wxGetApp().is_editor()) {
        //BBS: always load shell at preview, do this in load_shells
        _update_slice_error_status();
    }

    m_gcode_viewer.refresh(gcode_result, str_tool_colors);
    set_as_dirty();
    request_extra_frame();
}

void GLCanvas3D::refresh_gcode_preview_render_paths()
{
    m_gcode_viewer.refresh_render_paths();
    set_as_dirty();
    request_extra_frame();
}

void GLCanvas3D::load_sla_preview()
{
    const SLAPrint* print = sla_print();
    if (m_canvas != nullptr && print != nullptr) {
        _set_current();
	    // Release OpenGL data before generating new data.
	    reset_volumes();
        _load_sla_shells();
        _update_sla_shells_outside_state();
        _set_warning_notification_if_needed(EWarning::SlaSupportsOutside);
    }
}

/*void GLCanvas3D::load_preview(const std::vector<std::string>& str_tool_colors, const std::vector<CustomGCode::Item>& color_print_values)
{
    const Print *print = this->fff_print();
    if (print == nullptr)
        return;

    _set_current();

    // Release OpenGL data before generating new data.
    this->reset_volumes();

    const BuildVolume &build_volume = m_bed.build_volume();
    _load_print_toolpaths(build_volume);
    _load_wipe_tower_toolpaths(build_volume, str_tool_colors);
    for (const PrintObject* object : print->objects())
        _load_print_object_toolpaths(*object, build_volume, str_tool_colors, color_print_values);

    _set_warning_notification_if_needed(EWarning::ToolpathOutside);
}*/

void GLCanvas3D::bind_event_handlers()
{
    if (m_canvas != nullptr) {
        m_canvas->Bind(wxEVT_SIZE, &GLCanvas3D::on_size, this);
        m_canvas->Bind(wxEVT_IDLE, &GLCanvas3D::on_idle, this);
        m_canvas->Bind(wxEVT_CHAR, &GLCanvas3D::on_char, this);
        m_canvas->Bind(wxEVT_KEY_DOWN, &GLCanvas3D::on_key, this);
        m_canvas->Bind(wxEVT_KEY_UP, &GLCanvas3D::on_key, this);
        m_canvas->Bind(wxEVT_MOUSEWHEEL, &GLCanvas3D::on_mouse_wheel, this);
        m_canvas->Bind(wxEVT_TIMER, &GLCanvas3D::on_timer, this);
        m_canvas->Bind(EVT_GLCANVAS_RENDER_TIMER, &GLCanvas3D::on_render_timer, this);
        m_toolbar_highlighter.set_timer_owner(m_canvas, 0);
        m_canvas->Bind(EVT_GLCANVAS_TOOLBAR_HIGHLIGHTER_TIMER, [this](wxTimerEvent&) { m_toolbar_highlighter.blink(); });
        m_gizmo_highlighter.set_timer_owner(m_canvas, 0);
        m_canvas->Bind(EVT_GLCANVAS_GIZMO_HIGHLIGHTER_TIMER, [this](wxTimerEvent&) { m_gizmo_highlighter.blink(); });
        m_canvas->Bind(wxEVT_LEFT_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_LEFT_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_MIDDLE_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_MIDDLE_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_RIGHT_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_RIGHT_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_MOTION, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_ENTER_WINDOW, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_LEAVE_WINDOW, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_LEFT_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_MIDDLE_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_RIGHT_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Bind(wxEVT_PAINT, &GLCanvas3D::on_paint, this);
        m_canvas->Bind(wxEVT_SET_FOCUS, &GLCanvas3D::on_set_focus, this);
        m_canvas->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& evt) {
                ImGui::SetWindowFocus(nullptr);
                render();
                evt.Skip();
            });
        m_event_handlers_bound = true;

        m_canvas->Bind(wxEVT_GESTURE_PAN, &GLCanvas3D::on_gesture, this);
        m_canvas->Bind(wxEVT_GESTURE_ZOOM, &GLCanvas3D::on_gesture, this);
        m_canvas->Bind(wxEVT_GESTURE_ROTATE, &GLCanvas3D::on_gesture, this);
        m_canvas->EnableTouchEvents(wxTOUCH_ZOOM_GESTURE | wxTOUCH_ROTATE_GESTURE);
#if __WXOSX__
        initGestures(m_canvas->GetHandle(), m_canvas); // for UIPanGestureRecognizer allowedScrollTypesMask
#endif
    }
}

void GLCanvas3D::unbind_event_handlers()
{
    if (m_canvas != nullptr && m_event_handlers_bound) {
        m_canvas->Unbind(wxEVT_SIZE, &GLCanvas3D::on_size, this);
        m_canvas->Unbind(wxEVT_IDLE, &GLCanvas3D::on_idle, this);
        m_canvas->Unbind(wxEVT_CHAR, &GLCanvas3D::on_char, this);
        m_canvas->Unbind(wxEVT_KEY_DOWN, &GLCanvas3D::on_key, this);
        m_canvas->Unbind(wxEVT_KEY_UP, &GLCanvas3D::on_key, this);
        m_canvas->Unbind(wxEVT_MOUSEWHEEL, &GLCanvas3D::on_mouse_wheel, this);
        m_canvas->Unbind(wxEVT_TIMER, &GLCanvas3D::on_timer, this);
        m_canvas->Unbind(EVT_GLCANVAS_RENDER_TIMER, &GLCanvas3D::on_render_timer, this);
        m_canvas->Unbind(wxEVT_LEFT_DOWN, &GLCanvas3D::on_mouse, this);
		m_canvas->Unbind(wxEVT_LEFT_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_MIDDLE_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_MIDDLE_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_RIGHT_DOWN, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_RIGHT_UP, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_MOTION, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_ENTER_WINDOW, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_LEAVE_WINDOW, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_LEFT_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_MIDDLE_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_RIGHT_DCLICK, &GLCanvas3D::on_mouse, this);
        m_canvas->Unbind(wxEVT_PAINT, &GLCanvas3D::on_paint, this);
        m_canvas->Unbind(wxEVT_SET_FOCUS, &GLCanvas3D::on_set_focus, this);
        m_event_handlers_bound = false;

        m_canvas->Unbind(wxEVT_GESTURE_PAN, &GLCanvas3D::on_gesture, this);
        m_canvas->Unbind(wxEVT_GESTURE_ZOOM, &GLCanvas3D::on_gesture, this);
        m_canvas->Unbind(wxEVT_GESTURE_ROTATE, &GLCanvas3D::on_gesture, this);
    }
}

void GLCanvas3D::on_size(wxSizeEvent& evt)
{
    m_dirty = true;
}

void GLCanvas3D::on_idle(wxIdleEvent& evt)
{
    if (!m_initialized)
        return;

    m_dirty |= m_main_toolbar.update_items_state();
    //BBS: GUI refactor: GLToolbar
    m_dirty |= m_assemble_view_toolbar.update_items_state();
    // BBS
    //m_dirty |= wxGetApp().plater()->get_view_toolbar().update_items_state();
    m_dirty |= wxGetApp().plater()->get_collapse_toolbar().update_items_state();
    _update_imgui_select_plate_toolbar();
    bool mouse3d_controller_applied = wxGetApp().plater()->get_mouse3d_controller().apply(wxGetApp().plater()->get_camera());
    m_dirty |= mouse3d_controller_applied;
    m_dirty |= wxGetApp().plater()->get_notification_manager()->update_notifications(*this);
    auto gizmo = wxGetApp().plater()->get_view3D_canvas3D()->get_gizmos_manager().get_current();
    if (gizmo != nullptr) m_dirty |= gizmo->update_items_state();
#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
    // ImGuiWrapper::m_requires_extra_frame may have been set by a render made outside of the OnIdle mechanism
    bool imgui_requires_extra_frame = wxGetApp().imgui()->requires_extra_frame();
    m_dirty |= imgui_requires_extra_frame;
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
    m_dirty |= GLTexture::Compressor::has_compressed_texture_to_refresh();

    if (!m_dirty)
        return;

#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
    // this needs to be done here.
    // during the render launched by the refresh the value may be set again
    wxGetApp().imgui()->reset_requires_extra_frame();
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT

    _refresh_if_shown_on_screen();

#if ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
    if (m_extra_frame_requested || mouse3d_controller_applied || imgui_requires_extra_frame || wxGetApp().imgui()->requires_extra_frame()) {
#else
    if (m_extra_frame_requested || mouse3d_controller_applied) {
        m_dirty = true;
#endif // ENABLE_ENHANCED_IMGUI_SLIDER_FLOAT
        m_extra_frame_requested = false;
        evt.RequestMore();
    }
    else
        m_dirty = false;
}

void GLCanvas3D::on_char(wxKeyEvent& evt)
{
    if (!m_initialized)
        return;

    // see include/wx/defs.h enum wxKeyCode
    int keyCode = evt.GetKeyCode();
    int ctrlMask = wxMOD_CONTROL;
    int shiftMask = wxMOD_SHIFT;

    auto imgui = wxGetApp().imgui();
    if (imgui->update_key_data(evt)) {
        render();
        return;
    }

    bool is_in_painting_mode = false;
    GLGizmoPainterBase *current_gizmo_painter = dynamic_cast<GLGizmoPainterBase *>(get_gizmos_manager().get_current());
    if (current_gizmo_painter != nullptr) {
        is_in_painting_mode = true;
    }

    //BBS: add orient deactivate logic
    if (keyCode == WXK_ESCAPE
        && (_deactivate_arrange_menu() || _deactivate_orient_menu()))
        return;

    if (m_gizmos.on_char(evt))
        return;

    if ((evt.GetModifiers() & ctrlMask) != 0) {
        // CTRL is pressed
        switch (keyCode) {
#ifdef __APPLE__
        case 'a':
        case 'A':
#else /* __APPLE__ */
        case WXK_CONTROL_A:
#endif /* __APPLE__ */
            if (!is_in_painting_mode && !m_layers_editing.is_enabled())
                post_event(SimpleEvent(EVT_GLCANVAS_SELECT_ALL));
        break;
#ifdef __APPLE__
        case 'c':
        case 'C':
#else /* __APPLE__ */
        case WXK_CONTROL_C:
#endif /* __APPLE__ */
            if (!is_in_painting_mode)
                post_event(SimpleEvent(EVT_GLTOOLBAR_COPY));
        break;
#ifdef __APPLE__
        case 'm':
        case 'M':
#else /* __APPLE__ */
        case WXK_CONTROL_M:
#endif /* __APPLE__ */
        {
#ifdef _WIN32
            if (wxGetApp().app_config->get("use_legacy_3DConnexion") == "true") {
#endif //_WIN32
#ifdef __APPLE__
            // On OSX use Cmd+Shift+M to "Show/Hide 3Dconnexion devices settings dialog"
            if ((evt.GetModifiers() & shiftMask) != 0) {
#endif // __APPLE__
                Mouse3DController& controller = wxGetApp().plater()->get_mouse3d_controller();
                controller.show_settings_dialog(!controller.is_settings_dialog_shown());
                m_dirty = true;
#ifdef __APPLE__
            }
            else
            // and Cmd+M to minimize application
                wxGetApp().mainframe->Iconize();
#endif // __APPLE__
#ifdef _WIN32
            }
#endif //_WIN32
            break;
        }
#ifdef __APPLE__
        case 'v':
        case 'V':
#else /* __APPLE__ */
        case WXK_CONTROL_V:
#endif /* __APPLE__ */
            if (!is_in_painting_mode)
                post_event(SimpleEvent(EVT_GLTOOLBAR_PASTE));
        break;

#ifdef __APPLE__
        case 'x':
        case 'X':
#else /* __APPLE__ */
        case WXK_CONTROL_X:
#endif /* __APPLE__ */
            if (!is_in_painting_mode)
                post_event(SimpleEvent(EVT_GLTOOLBAR_CUT));
        break;

#ifdef __APPLE__
        case 'f':
        case 'F':
#else /* __APPLE__ */
        case WXK_CONTROL_F:
#endif /* __APPLE__ */
            break;


#ifdef __APPLE__
        case 'y':
        case 'Y':
#else /* __APPLE__ */
        case WXK_CONTROL_Y:
#endif /* __APPLE__ */
            if (m_canvas_type == CanvasView3D || m_canvas_type == CanvasAssembleView) {
                post_event(SimpleEvent(EVT_GLCANVAS_REDO));
            }
        break;
#ifdef __APPLE__
        case 'z':
        case 'Z':
#else /* __APPLE__ */
        case WXK_CONTROL_Z:
#endif /* __APPLE__ */
            // only support redu/undo in CanvasView3D
            if (m_canvas_type == CanvasView3D || m_canvas_type == CanvasAssembleView) {
                post_event(SimpleEvent(EVT_GLCANVAS_UNDO));
            }
        break;

        // BBS
#ifdef __APPLE__
        case 'E':
        case 'e':
#else /* __APPLE__ */
        case WXK_CONTROL_E:
#endif /* __APPLE__ */
        { m_labels.show(!m_labels.is_shown()); m_dirty = true; break; }
        case '0': {
            select_view("plate");
            zoom_to_bed();
            break; }
        case '1': { select_view("top"); break; }
        case '2': { select_view("bottom"); break; }
        case '3': { select_view("front"); break; }
        case '4': { select_view("rear"); break; }
        case '5': { select_view("left"); break; }
        case '6': { select_view("right"); break; }
        case '7': { select_plate(); break; }

        //case WXK_BACK:
        //case WXK_DELETE:
#ifdef __APPLE__
        case 'd':
        case 'D':
#else /* __APPLE__ */
        case WXK_CONTROL_D:
#endif /* __APPLE__ */
            post_event(SimpleEvent(EVT_GLTOOLBAR_DELETE_ALL));
            break;
#ifdef __APPLE__
        case 'k':
        case 'K':
#else /* __APPLE__ */
        case WXK_CONTROL_K:
#endif /* __APPLE__ */
            post_event(SimpleEvent(EVT_GLTOOLBAR_CLONE));
            break;
        default:            evt.Skip();
        }
    } else {
        auto obj_list = wxGetApp().obj_list();
        switch (keyCode)
        {
        //case WXK_BACK:
        case WXK_DELETE: { post_event(SimpleEvent(EVT_GLTOOLBAR_DELETE)); break; }
        // BBS
#ifdef __APPLE__
        case WXK_BACK: { post_event(SimpleEvent(EVT_GLTOOLBAR_DELETE)); break; }
#endif
        case WXK_ESCAPE: { deselect_all(); break; }
        case WXK_F5: {
            if (wxGetApp().mainframe->is_printer_view())
                wxGetApp().mainframe->load_printer_url();

            //if ((wxGetApp().is_editor() && !wxGetApp().plater()->model().objects.empty()) ||
            //    (wxGetApp().is_gcode_viewer() && !wxGetApp().plater()->get_last_loaded_gcode().empty()))
            //    post_event(SimpleEvent(EVT_GLCANVAS_RELOAD_FROM_DISK));
            break;
        }

        // BBS: use keypad to change extruder
        case '1': {
            if (!m_timer_set_color.IsRunning()) {
                m_timer_set_color.StartOnce(500);
                break;
            }
        }
        case '0':   //Color logic for material 10
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9': {
            if (m_timer_set_color.IsRunning()) {
                if (keyCode < '7')  keyCode += 10;
                m_timer_set_color.Stop();
            }
            if (m_gizmos.get_current_type() != GLGizmosManager::MmSegmentation)
                obj_list->set_extruder_for_selected_items(keyCode - '0');
            break;
        }

        case '+': {
            if (dynamic_cast<Preview*>(m_canvas->GetParent()) != nullptr)
                post_event(wxKeyEvent(EVT_GLCANVAS_EDIT_COLOR_CHANGE, evt));
            else
                post_event(Event<int>(EVT_GLCANVAS_INCREASE_INSTANCES, +1));
            break;
        }
        case '-': {
            if (dynamic_cast<Preview*>(m_canvas->GetParent()) != nullptr)
                post_event(wxKeyEvent(EVT_GLCANVAS_EDIT_COLOR_CHANGE, evt));
            else
                post_event(Event<int>(EVT_GLCANVAS_INCREASE_INSTANCES, -1));
            break;
        }
        case '?': { post_event(SimpleEvent(EVT_GLCANVAS_QUESTION_MARK)); break; }
        case 'A':
        case 'a':
            {
                if ((evt.GetModifiers() & shiftMask) != 0)
                    post_event(SimpleEvent(EVT_GLCANVAS_ARRANGE_PARTPLATE));
                else
                    post_event(SimpleEvent(EVT_GLCANVAS_ARRANGE));
                break;
            }
        //case 'B':
        //case 'b': { zoom_to_bed(); break; }
        case 'C':
        case 'c': { wxGetApp().toggle_show_gcode_window(); m_dirty = true; request_extra_frame(); break; }
        //case 'G':
        //case 'g': {
        //    if ((evt.GetModifiers() & shiftMask) != 0) {
        //        if (dynamic_cast<Preview*>(m_canvas->GetParent()) != nullptr)
        //            post_event(wxKeyEvent(EVT_GLCANVAS_JUMP_TO, evt));
        //    }
        //    break;
        //}
        case 'I':
        case 'i': { _update_camera_zoom(1.0); break; }
        //case 'K':
        //case 'k': { wxGetApp().plater()->get_camera().select_next_type(); m_dirty = true; break; }
        //case 'L':
        //case 'l': {
            //if (!m_main_toolbar.is_enabled()) {
            //    m_gcode_viewer.enable_legend(!m_gcode_viewer.is_legend_enabled());
            //    m_dirty = true;
            //    wxGetApp().plater()->update_preview_bottom_toolbar();
            //}
            //break;
        //}
        case 'O':
        case 'o': { _update_camera_zoom(-1.0); break; }
        case 'q':
        case 'Q':
            {
                if ((evt.GetModifiers() & shiftMask) != 0)
                    post_event(SimpleEvent(EVT_GLCANVAS_ORIENT_PARTPLATE));
                else
                    post_event(SimpleEvent(EVT_GLCANVAS_ORIENT));
                break;
            }
        //case 'Z':
        //case 'z': {
        //    if (!m_selection.is_empty())
        //        zoom_to_selection();
        //    else {
        //        if (!m_volumes.empty())
        //            zoom_to_volumes();
        //        else
        //            _zoom_to_box(m_gcode_viewer.get_paths_bounding_box());
        //    }
        //    break;
        //}
        default:  { evt.Skip(); break; }
        }
    }
}

class TranslationProcessor
{
    using UpAction = std::function<void(void)>;
    using DownAction = std::function<void(const Vec3d&, bool, bool)>;

    UpAction m_up_action{ nullptr };
    DownAction m_down_action{ nullptr };

    bool m_running{ false };
    Vec3d m_direction{ Vec3d::UnitX() };

public:
    TranslationProcessor(UpAction up_action, DownAction down_action)
        : m_up_action(up_action), m_down_action(down_action)
    {
    }

    void process(wxKeyEvent& evt)
    {
        const int keyCode = evt.GetKeyCode();
        wxEventType type = evt.GetEventType();
        if (type == wxEVT_KEY_UP) {
            switch (keyCode)
            {
            case WXK_NUMPAD_LEFT:  case WXK_LEFT:
            case WXK_NUMPAD_RIGHT: case WXK_RIGHT:
            case WXK_NUMPAD_UP:    case WXK_UP:
            case WXK_NUMPAD_DOWN:  case WXK_DOWN:
            {
                m_running = false;
                m_up_action();
                break;
            }
            default: { break; }
            }
        }
        else if (type == wxEVT_KEY_DOWN) {
            bool apply = false;

            switch (keyCode)
            {
            case WXK_SHIFT:
            {
                if (m_running)
                    apply = true;

                break;
            }
            case WXK_NUMPAD_LEFT:
            case WXK_LEFT:
            {
                m_direction = -Vec3d::UnitX();
                apply = true;
                break;
            }
            case WXK_NUMPAD_RIGHT:
            case WXK_RIGHT:
            {
                m_direction = Vec3d::UnitX();
                apply = true;
                break;
            }
            case WXK_NUMPAD_UP:
            case WXK_UP:
            {
                m_direction = Vec3d::UnitY();
                apply = true;
                break;
            }
            case WXK_NUMPAD_DOWN:
            case WXK_DOWN:
            {
                m_direction = -Vec3d::UnitY();
                apply = true;
                break;
            }
            default: { break; }
            }

            if (apply) {
                m_running = true;
                m_down_action(m_direction, evt.ShiftDown(), evt.CmdDown());
            }
        }
    }
};

void GLCanvas3D::on_key(wxKeyEvent& evt)
{
    static GLCanvas3D const * thiz = nullptr;
    static TranslationProcessor translationProcessor(nullptr, nullptr);
    if (thiz != this) {
        thiz = this;
        translationProcessor = TranslationProcessor(
        [this]() {
            do_move(L("Tool Move"));
            m_gizmos.update_data();

            // BBS
            //wxGetApp().obj_manipul()->set_dirty();
            // Let the plater know that the dragging finished, so a delayed refresh
            // of the scene with the background processing data should be performed.
            post_event(SimpleEvent(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED));
            // updates camera target constraints
            refresh_camera_scene_box();
            m_dirty = true;
        },
        [this](const Vec3d& direction, bool slow, bool camera_space) {
            m_selection.setup_cache();
            double multiplier = slow ? 1.0 : 10.0;

            Vec3d displacement;
            if (camera_space) {
                Eigen::Matrix<double, 3, 3, Eigen::DontAlign> inv_view_3x3 = wxGetApp().plater()->get_camera().get_view_matrix().inverse().matrix().block(0, 0, 3, 3);
                displacement = multiplier * (inv_view_3x3 * direction);
                displacement.z() = 0.0;
            }
            else
                displacement = multiplier * direction;

            TransformationType trafo_type;
            trafo_type.set_relative();
            m_selection.translate(displacement, trafo_type);
            m_dirty = true;
        }
    );}

    const int keyCode = evt.GetKeyCode();

    auto imgui = wxGetApp().imgui();
    if (imgui->update_key_data(evt)) {
        render();
    }
    else
    {
        if (!m_gizmos.on_key(evt)) {
            if (evt.GetEventType() == wxEVT_KEY_UP) {
                if (evt.ShiftDown() && evt.ControlDown() && keyCode == WXK_SPACE) {
#if !BBL_RELEASE_TO_PUBLIC
                    wxGetApp().plater()->toggle_render_statistic_dialog();
                    m_dirty = true;
#endif
                } else if ((evt.ShiftDown() && evt.ControlDown() && keyCode == WXK_RETURN) ||
                    (evt.ShiftDown() && evt.AltDown() && keyCode == WXK_RETURN)) {
                    wxGetApp().plater()->toggle_show_wireframe();
                    m_dirty = true;
                }
                else if (m_tab_down && keyCode == WXK_TAB && !evt.HasAnyModifiers()) {
                    // Enable switching between 3D and Preview with Tab
                    // m_canvas->HandleAsNavigationKey(evt);   // XXX: Doesn't work in some cases / on Linux
                    post_event(SimpleEvent(EVT_GLCANVAS_TAB));
                }
                else if (keyCode == WXK_TAB && evt.ShiftDown() && !evt.ControlDown() && ! wxGetApp().is_gcode_viewer()) {
                    // Collapse side-panel with Shift+Tab
                    post_event(SimpleEvent(EVT_GLCANVAS_COLLAPSE_SIDEBAR));
                }
                else if (keyCode == WXK_SHIFT) {
                    translationProcessor.process(evt);

                    if (m_picking_enabled && m_rectangle_selection.is_dragging()) {
                        _update_selection_from_hover();
                        m_rectangle_selection.stop_dragging();
                        m_mouse.ignore_left_up = true;
                        m_dirty = true;
                    }
//                    set_cursor(Standard);
                }
                else if (keyCode == WXK_ALT) {
                    if (m_picking_enabled && m_rectangle_selection.is_dragging()) {
                        _update_selection_from_hover();
                        m_rectangle_selection.stop_dragging();
                        m_mouse.ignore_left_up = true;
                        m_dirty = true;
                    }
//                    set_cursor(Standard);
#ifdef __WXMSW__
                    if (m_camera_movement && m_is_touchpad_navigation) {
                        m_camera_movement = false;
                        m_mouse.set_start_position_3D_as_invalid();
                    }
#endif
                }
                else if (keyCode == WXK_CONTROL)
                    m_dirty = true;
                else if (m_gizmos.is_enabled() && !m_selection.is_empty() && m_canvas_type != CanvasAssembleView) {
                    translationProcessor.process(evt);

                    switch (keyCode)
                    {
                    case WXK_NUMPAD_PAGEUP:   case WXK_PAGEUP:
                    case WXK_NUMPAD_PAGEDOWN: case WXK_PAGEDOWN:
                    {
                        do_rotate(L("Tool Rotate"));
                        m_gizmos.update_data();

                        // BBS
                        //wxGetApp().obj_manipul()->set_dirty();
                        // Let the plater know that the dragging finished, so a delayed refresh
                        // of the scene with the background processing data should be performed.
                        post_event(SimpleEvent(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED));
                        // updates camera target constraints
                        refresh_camera_scene_box();
                        m_dirty = true;

                        break;
                    }
                    default: { break; }
                    }
                }

                // BBS: add select view logic
                if (evt.ControlDown()) {
                    switch (keyCode) {
                        case '0':
                        case WXK_NUMPAD0: //0 on numpad
                            { select_view("plate");
                              zoom_to_bed();
                            break;
                        }
                        case '1':
                        case WXK_NUMPAD1: //1 on numpad
                            { select_view("top"); break; }
                        case '2':
                        case WXK_NUMPAD2: //2 on numpad
                            { select_view("bottom"); break; }
                        case '3':
                        case WXK_NUMPAD3: //3 on numpad
                            { select_view("front"); break; }
                        case '4':
                        case WXK_NUMPAD4: //4 on numpad
                            { select_view("rear"); break; }
                        case '5':
                        case WXK_NUMPAD5: //5 on numpad
                            { select_view("left"); break; }
                        case '6':
                        case WXK_NUMPAD6: //6 on numpad
                            { select_view("right"); break; }
                        case '7':
                        case WXK_NUMPAD7: //7 on numpad
                            { select_plate(); break; }
                        default: break;
                    }
                }
            }
            else if (evt.GetEventType() == wxEVT_KEY_DOWN) {
                m_tab_down = keyCode == WXK_TAB && !evt.HasAnyModifiers();
                if (keyCode == WXK_SHIFT) {
                    translationProcessor.process(evt);

                    if (m_picking_enabled /*&& (m_gizmos.get_current_type() != GLGizmosManager::SlaSupports)*/)
                    {
                        m_mouse.ignore_left_up = false;
//                        set_cursor(Cross);
                    }
                }
                else if (keyCode == WXK_ALT) {
                    if (m_picking_enabled /*&& (m_gizmos.get_current_type() != GLGizmosManager::SlaSupports)*/)
                    {
                        m_mouse.ignore_left_up = false;
//                        set_cursor(Cross);
                    }
                }
                else if (keyCode == WXK_CONTROL)
                    m_dirty = true;
                else if (m_gizmos.is_enabled() && !m_selection.is_empty() && m_canvas_type != CanvasAssembleView) {
                    auto _do_rotate = [this](double angle_z_rad) {
                        m_selection.setup_cache();
                        m_selection.rotate(Vec3d(0.0, 0.0, angle_z_rad), TransformationType(TransformationType::World_Relative_Joint));
                        m_dirty = true;
//                        wxGetApp().obj_manipul()->set_dirty();
                    };

                    translationProcessor.process(evt);

                    switch (keyCode)
                    {
                    case WXK_NUMPAD_PAGEUP:   case WXK_PAGEUP:   { _do_rotate(0.25 * M_PI); break; }
                    case WXK_NUMPAD_PAGEDOWN: case WXK_PAGEDOWN: { _do_rotate(-0.25 * M_PI); break; }
                    default: { break; }
                    }
                } else if (!m_gizmos.is_enabled()) {
                    // DoubleSlider navigation in Preview
                    if (m_canvas_type == CanvasPreview) {
                        IMSlider *m_layers_slider = get_gcode_viewer().get_layers_slider();
                        IMSlider *m_moves_slider  = get_gcode_viewer().get_moves_slider();
                        int increment = (evt.CmdDown() || evt.ShiftDown()) ? 5 : 1;
                        if ((evt.CmdDown() || evt.ShiftDown()) && evt.GetKeyCode() == 'G') {
                            m_layers_slider->show_go_to_layer(true);
                        }
                        else if (keyCode == WXK_UP || keyCode == WXK_DOWN) {
                            int new_pos;
                            if (m_layers_slider->GetSelection() == ssHigher) {
                                new_pos = keyCode == WXK_UP ? m_layers_slider->GetHigherValue() + increment : m_layers_slider->GetHigherValue() - increment;
                                m_layers_slider->SetHigherValue(new_pos);
                                m_moves_slider->SetHigherValue(m_moves_slider->GetMaxValue());
                            }
                            else if (m_layers_slider->GetSelection() == ssLower) {
                                new_pos = keyCode == WXK_UP ? m_layers_slider->GetLowerValue() + increment : m_layers_slider->GetLowerValue() - increment;
                                m_layers_slider->SetLowerValue(new_pos);
                            }
                        } else if (keyCode == WXK_LEFT) {
                            if (m_moves_slider->GetHigherValue() == m_moves_slider->GetMinValue() && (m_layers_slider->GetHigherValue() > m_layers_slider->GetMinValue())) {
                                m_layers_slider->SetHigherValue(m_layers_slider->GetHigherValue() - 1);
                                m_moves_slider->SetHigherValue(m_moves_slider->GetMaxValue());
                            } else {
                                m_moves_slider->SetHigherValue(m_moves_slider->GetHigherValue() - increment);
                            }
                        } else if (keyCode == WXK_RIGHT) {
                            if (m_moves_slider->GetHigherValue() == m_moves_slider->GetMaxValue() && (m_layers_slider->GetHigherValue() < m_layers_slider->GetMaxValue())) {
                                m_layers_slider->SetHigherValue(m_layers_slider->GetHigherValue() + 1);
                                m_moves_slider->SetHigherValue(m_moves_slider->GetMinValue());
                            } else {
                                m_moves_slider->SetHigherValue(m_moves_slider->GetHigherValue() + increment);
                            }
                        } else if (keyCode == WXK_HOME || keyCode == WXK_END) {
                            const int new_pos = keyCode == WXK_HOME ? m_moves_slider->GetMinValue() : m_moves_slider->GetMaxValue();
                            m_moves_slider->SetHigherValue(new_pos);
                            m_moves_slider->set_as_dirty();
                        }

                        if (m_layers_slider->is_dirty() && m_layers_slider->is_one_layer())
                            m_layers_slider->SetLowerValue(m_layers_slider->GetHigherValue());

                        m_dirty = true;
                    }
                }
            }
        }
        else return;
    }

    if (keyCode != WXK_TAB
        && keyCode != WXK_LEFT
        && keyCode != WXK_UP
        && keyCode != WXK_RIGHT
        && keyCode != WXK_DOWN) {
        evt.Skip();   // Needed to have EVT_CHAR generated as well
    }
}

void GLCanvas3D::on_mouse_wheel(wxMouseEvent& evt)
{
#ifdef WIN32
    // Try to filter out spurious mouse wheel events comming from 3D mouse.
    if (wxGetApp().plater()->get_mouse3d_controller().process_mouse_wheel())
        return;
#endif

    if (!m_initialized)
        return;

    // Ignore the wheel events if the middle button is pressed.
    if (evt.MiddleIsDown())
        return;

#if ENABLE_RETINA_GL
    const float scale = m_retina_helper->get_scale_factor();
    evt.SetX(evt.GetX() * scale);
    evt.SetY(evt.GetY() * scale);
#endif

    if (wxGetApp().imgui()->update_mouse_data(evt)) {
        if (m_canvas_type == CanvasPreview) {
            IMSlider* m_layers_slider = get_gcode_viewer().get_layers_slider();
            IMSlider* m_moves_slider = get_gcode_viewer().get_moves_slider();
            m_layers_slider->on_mouse_wheel(evt);
            m_moves_slider->on_mouse_wheel(evt);
        }
        render();
        m_dirty = true;
        return;
    }

#ifdef __WXMSW__
	// For some reason the Idle event is not being generated after the mouse scroll event in case of scrolling with the two fingers on the touch pad,
	// if the event is not allowed to be passed further.
    // evt.Skip() used to trigger the needed screen refresh, but it does no more. wxWakeUpIdle() seem to work now.
    wxWakeUpIdle();
#endif /* __WXMSW__ */

    // Performs layers editing updates, if enabled
    if (is_layers_editing_enabled()) {
        int object_idx_selected = m_selection.get_object_idx();
        if (object_idx_selected != -1) {
            // A volume is selected. Test, whether hovering over a layer thickness bar.
            if (m_layers_editing.bar_rect_contains(*this, (float)evt.GetX(), (float)evt.GetY())) {
                // Adjust the width of the selection.
                m_layers_editing.band_width = std::max(std::min(m_layers_editing.band_width * (1.0f + 0.1f * (float)evt.GetWheelRotation() / (float)evt.GetWheelDelta()), 10.0f), 1.5f);
                if (m_canvas != nullptr)
                    m_canvas->Refresh();

                return;
            }
        }
    }

    // Inform gizmos about the event so they have the opportunity to react.
    if (m_gizmos.on_mouse_wheel(evt))
        return;

    if (m_canvas_type == CanvasAssembleView && (evt.AltDown() || evt.CmdDown())) {
        float rotation = (float)evt.GetWheelRotation() / (float)evt.GetWheelDelta();
        if (evt.AltDown()) {
            auto clp_dist = m_gizmos.m_assemble_view_data->model_objects_clipper()->get_position();
            clp_dist = rotation < 0.f
                ? std::max(0., clp_dist - 0.01)
                : std::min(1., clp_dist + 0.01);
            m_gizmos.m_assemble_view_data->model_objects_clipper()->set_position(clp_dist, true);
        }
        else if (evt.CmdDown()) {
            m_explosion_ratio = rotation < 0.f
                ? std::max(1., m_explosion_ratio - 0.01)
                : std::min(3., m_explosion_ratio + 0.01);
            if (m_explosion_ratio != GLVolume::explosion_ratio) {
                for (GLVolume* volume : m_volumes.volumes) {
                    volume->set_bounding_boxes_as_dirty();
                }
                GLVolume::explosion_ratio = m_explosion_ratio;
            }
        }
        return;
    }
    // Calculate the zoom delta and apply it to the current zoom factor
    double direction_factor = wxGetApp().app_config->get_bool("reverse_mouse_wheel_zoom") ? -1.0 : 1.0;
    auto delta = direction_factor * (double)evt.GetWheelRotation() / (double)evt.GetWheelDelta();
    bool zoom_to_mouse = wxGetApp().app_config->get("zoom_to_mouse") == "true";
    if (!zoom_to_mouse) {// zoom to center
        _update_camera_zoom(delta);
    }
    else {
        auto cnv_size = get_canvas_size();
        float z{0.f};
        auto screen_center_3d_pos = _mouse_to_3d({ cnv_size.get_width() * 0.5, cnv_size.get_height() * 0.5 }, &z);
        auto mouse_3d_pos = _mouse_to_3d({evt.GetX(), evt.GetY()}, &z);
        Vec3d displacement = mouse_3d_pos - screen_center_3d_pos;

        wxGetApp().plater()->get_camera().translate(displacement);
        auto origin_zoom = wxGetApp().plater()->get_camera().get_zoom();
        _update_camera_zoom(delta);
        auto new_zoom = wxGetApp().plater()->get_camera().get_zoom();
        wxGetApp().plater()->get_camera().translate((-displacement) / (new_zoom / origin_zoom));
    }
}

void GLCanvas3D::on_timer(wxTimerEvent& evt)
{
    if (m_layers_editing.state == LayersEditing::Editing)
        _perform_layer_editing_action();
}

void GLCanvas3D::on_render_timer(wxTimerEvent& evt)
{
    // no need to wake up idle
    // right after this event, idle event is fired
    // m_dirty = true;
    // wxWakeUpIdle();
}

void GLCanvas3D::on_set_color_timer(wxTimerEvent& evt)
{
    auto obj_list = wxGetApp().obj_list();
    if (m_gizmos.get_current_type() != GLGizmosManager::MmSegmentation)
        obj_list->set_extruder_for_selected_items(1);
    m_timer_set_color.Stop();
}


void GLCanvas3D::schedule_extra_frame(int miliseconds)
{
    // Schedule idle event right now
    if (miliseconds == 0)
    {
        // We want to wakeup idle evnt but most likely this is call inside render cycle so we need to wait
        if (m_in_render)
            miliseconds = 33;
        else {
            m_dirty = true;
            wxWakeUpIdle();
            return;
        }
    }
    int remaining_time = m_render_timer.GetInterval();
    // Timer is not running
    if (!m_render_timer.IsRunning()) {
        m_render_timer.StartOnce(miliseconds);
    // Timer is running - restart only if new period is shorter than remaning period
    } else {
        if (miliseconds + 20 < remaining_time) {
            m_render_timer.Stop();
            m_render_timer.StartOnce(miliseconds);
        }
    }
}

#ifndef NDEBUG
// #define SLIC3R_DEBUG_MOUSE_EVENTS
#endif

#ifdef SLIC3R_DEBUG_MOUSE_EVENTS
std::string format_mouse_event_debug_message(const wxMouseEvent &evt)
{
	static int idx = 0;
	char buf[2048];
	std::string out;
	sprintf(buf, "Mouse Event %d - ", idx ++);
	out = buf;

	if (evt.Entering())
		out += "Entering ";
	if (evt.Leaving())
		out += "Leaving ";
	if (evt.Dragging())
		out += "Dragging ";
	if (evt.Moving())
		out += "Moving ";
	if (evt.Magnify())
		out += "Magnify ";
	if (evt.LeftDown())
		out += "LeftDown ";
	if (evt.LeftUp())
		out += "LeftUp ";
	if (evt.LeftDClick())
		out += "LeftDClick ";
	if (evt.MiddleDown())
		out += "MiddleDown ";
	if (evt.MiddleUp())
		out += "MiddleUp ";
	if (evt.MiddleDClick())
		out += "MiddleDClick ";
	if (evt.RightDown())
		out += "RightDown ";
	if (evt.RightUp())
		out += "RightUp ";
	if (evt.RightDClick())
		out += "RightDClick ";
    if (evt.AltDown())
        out += "AltDown ";
    if (evt.ShiftDown())
        out += "ShiftDown ";
    if (evt.ControlDown())
        out += "ControlDown ";

	sprintf(buf, "(%d, %d)", evt.GetX(), evt.GetY());
	out += buf;
	return out;
}
#endif /* SLIC3R_DEBUG_MOUSE_EVENTS */

void GLCanvas3D::on_gesture(wxGestureEvent &evt)
{
    if (!m_initialized || !_set_current())
        return;

    auto & camera = wxGetApp().plater()->get_camera();
    if (evt.GetEventType() == wxEVT_GESTURE_PAN) {
        auto p = evt.GetPosition();
        auto d = static_cast<wxPanGestureEvent&>(evt).GetDelta();
        float z = 0;
        const Vec3d &p2 = _mouse_to_3d({p.x, p.y}, &z);
        const Vec3d &p1 = _mouse_to_3d({p.x - d.x, p.y - d.y}, &z);
        camera.set_target(camera.get_target() + p1 - p2);
    } else if (evt.GetEventType() == wxEVT_GESTURE_ZOOM) {
        static float zoom_start = 1;
        if (evt.IsGestureStart())
            zoom_start = camera.get_zoom();
        camera.set_zoom(zoom_start * static_cast<wxZoomGestureEvent&>(evt).GetZoomFactor());
    } else if (evt.GetEventType() == wxEVT_GESTURE_ROTATE) {
        PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
        bool rotate_limit = current_printer_technology() != ptSLA;
        static double last_rotate = 0;
        if (evt.IsGestureStart())
            last_rotate = 0;
        auto rotate = static_cast<wxRotateGestureEvent&>(evt).GetRotationAngle() - last_rotate;
        last_rotate += rotate;
        if (plate)
            camera.rotate_on_sphere_with_target(-rotate, 0, rotate_limit, plate->get_bounding_box().center());
        else
            camera.rotate_on_sphere(-rotate, 0, rotate_limit);
        camera.auto_type(Camera::EType::Perspective);
    }
    m_dirty = true;
}

void GLCanvas3D::on_mouse(wxMouseEvent& evt)
{
    if (!m_initialized || !_set_current())
        return;

    // BBS: single snapshot
    Plater::SingleSnapshot single(wxGetApp().plater());

#if ENABLE_RETINA_GL
    const float scale = m_retina_helper->get_scale_factor();
    evt.SetX(evt.GetX() * scale);
    evt.SetY(evt.GetY() * scale);
#endif

    Point pos(evt.GetX(), evt.GetY());

    ImGuiWrapper* imgui = wxGetApp().imgui();
    if (m_tooltip.is_in_imgui() && evt.LeftUp())
        // ignore left up events coming from imgui windows and not processed by them
        m_mouse.ignore_left_up = true;
    m_tooltip.set_in_imgui(false);
    if (imgui->update_mouse_data(evt)) {
        if ((evt.LeftDown() || (evt.Moving() && (evt.AltDown() || evt.ShiftDown()))) && m_canvas != nullptr)
            m_canvas->SetFocus();
        m_mouse.position = evt.Leaving() ? Vec2d(-1.0, -1.0) : pos.cast<double>();
        m_tooltip.set_in_imgui(true);
        render();
#ifdef SLIC3R_DEBUG_MOUSE_EVENTS
        printf((format_mouse_event_debug_message(evt) + " - Consumed by ImGUI\n").c_str());
#endif /* SLIC3R_DEBUG_MOUSE_EVENTS */
        m_dirty = true;
        // do not return if dragging or tooltip not empty to allow for tooltip update
        // also, do not return if the mouse is moving and also is inside MM gizmo to allow update seed fill selection
        if (!m_mouse.dragging && m_tooltip.is_empty() && (m_gizmos.get_current_type() != GLGizmosManager::MmSegmentation || !evt.Moving()))
            return;
    }

#ifdef __WXMSW__
	bool on_enter_workaround = false;
    if (! evt.Entering() && ! evt.Leaving() && m_mouse.position.x() == -1.0) {
        // Workaround for SPE-832: There seems to be a mouse event sent to the window before evt.Entering()
        m_mouse.position = pos.cast<double>();
        render();
#ifdef SLIC3R_DEBUG_MOUSE_EVENTS
		printf((format_mouse_event_debug_message(evt) + " - OnEnter workaround\n").c_str());
#endif /* SLIC3R_DEBUG_MOUSE_EVENTS */
		on_enter_workaround = true;
    } else
#endif /* __WXMSW__ */
    {
#ifdef SLIC3R_DEBUG_MOUSE_EVENTS
		printf((format_mouse_event_debug_message(evt) + " - other\n").c_str());
#endif /* SLIC3R_DEBUG_MOUSE_EVENTS */
    }
    const int selected_object_idx      = m_selection.get_object_idx();
    const int layer_editing_object_idx = is_layers_editing_enabled() ? selected_object_idx : -1;
    const bool mouse_in_layer_editing  = layer_editing_object_idx != -1 && m_layers_editing.bar_rect_contains(*this, pos(0), pos(1));

    if (!mouse_in_layer_editing && m_main_toolbar.on_mouse(evt, *this)) {
        if (m_main_toolbar.is_any_item_pressed())
            m_gizmos.reset_all_states();
        if (evt.LeftUp() || evt.MiddleUp() || evt.RightUp())
            mouse_up_cleanup();
        m_mouse.set_start_position_3D_as_invalid();
        return;
    }

    //BBS: GUI refactor: GLToolbar
    if (!mouse_in_layer_editing && m_assemble_view_toolbar.on_mouse(evt, *this)) {
        if (evt.LeftUp() || evt.MiddleUp() || evt.RightUp())
            mouse_up_cleanup();
        m_mouse.set_start_position_3D_as_invalid();
        return;
    }

    if (!mouse_in_layer_editing && wxGetApp().plater()->get_collapse_toolbar().on_mouse(evt, *this)) {
        if (evt.LeftUp() || evt.MiddleUp() || evt.RightUp())
            mouse_up_cleanup();
        m_mouse.set_start_position_3D_as_invalid();
        return;
    }

    // BBS
#if 0
    if (wxGetApp().plater()->get_view_toolbar().on_mouse(evt, *this)) {
        if (evt.LeftUp() || evt.MiddleUp() || evt.RightUp())
            mouse_up_cleanup();
        m_mouse.set_start_position_3D_as_invalid();
        return;
    }
#endif

    for (GLVolume* volume : m_volumes.volumes) {
        volume->force_sinking_contours = false;
    }

    auto show_sinking_contours = [this]() {
        const Selection::IndicesList& idxs = m_selection.get_volume_idxs();
        for (unsigned int idx : idxs) {
            m_volumes.volumes[idx]->force_sinking_contours = true;
        }
        m_dirty = true;
    };

    if (!mouse_in_layer_editing && m_gizmos.on_mouse(evt)) {
        if (m_gizmos.is_running()) {
            _deactivate_arrange_menu();
            _deactivate_orient_menu();
            _deactivate_layersediting_menu();
        }
        if (wxWindow::FindFocus() != m_canvas)
            // Grab keyboard focus for input in gizmo dialogs.
            m_canvas->SetFocus();

        if (evt.LeftDown()) {
            // Clear hover state in main toolbar
            wxMouseEvent evt2 = evt;
            evt2.SetEventType(wxEVT_MOTION);
            evt2.SetLeftDown(false);
            m_main_toolbar.on_mouse(evt2, *this);
        }

        if (evt.LeftUp() || evt.MiddleUp() || evt.RightUp())
            mouse_up_cleanup();

        m_mouse.set_start_position_3D_as_invalid();
        m_mouse.position = pos.cast<double>();

        // It should be detection of volume change
        // Not only detection of some modifiers !!!
        if (evt.Dragging()) {
            GLGizmosManager::EType c = m_gizmos.get_current_type();
            if (current_printer_technology() == ptFFF &&
                (fff_print()->config().print_sequence == PrintSequence::ByObject)) {
                if (can_sequential_clearance_show_in_gizmo())
                    update_sequential_clearance();
            } else {
                if (c == GLGizmosManager::EType::Move ||
                    c == GLGizmosManager::EType::Scale ||
                    c == GLGizmosManager::EType::Rotate)
                    show_sinking_contours();
            }
        }
        else if (evt.LeftUp() &&
            m_gizmos.get_current_type() == GLGizmosManager::EType::Scale &&
            m_gizmos.get_current()->get_state() == GLGizmoBase::EState::On) {
            wxGetApp().obj_list()->selection_changed();
        }

        return;
    }

    bool any_gizmo_active = m_gizmos.get_current() != nullptr;
    bool swap_mouse_buttons = wxGetApp().app_config->get_bool("swap_mouse_buttons");

    if (m_mouse.drag.move_requires_threshold && m_mouse.is_move_start_threshold_position_2D_defined() && m_mouse.is_move_threshold_met(pos)) {
        m_mouse.drag.move_requires_threshold = false;
        m_mouse.set_move_start_threshold_position_2D_as_invalid();
    }

    if (evt.ButtonDown() && wxWindow::FindFocus() != m_canvas)
        // Grab keyboard focus on any mouse click event.
        m_canvas->SetFocus();

    if (evt.Entering()) {
//#if defined(__WXMSW__) || defined(__linux__)
//        // On Windows and Linux needs focus in order to catch key events
        // Set focus in order to remove it from sidebar fields
        if (m_canvas != nullptr) {
            // Only set focus, if the top level window of this canvas is active.
            auto p = dynamic_cast<wxWindow*>(evt.GetEventObject());
            while (p->GetParent())
                p = p->GetParent();
            auto *top_level_wnd = dynamic_cast<wxTopLevelWindow*>(p);
            m_mouse.position = pos.cast<double>();
            m_tooltip_enabled = false;
            // 1) forces a frame render to ensure that m_hover_volume_idxs is updated even when the user right clicks while
            // the context menu is shown, ensuring it to disappear if the mouse is outside any volume and to
            // change the volume hover state if any is under the mouse
            // 2) when switching between 3d view and preview the size of the canvas changes if the side panels are visible,
            // so forces a resize to avoid multiple renders with different sizes (seen as flickering)
            _refresh_if_shown_on_screen();
            m_tooltip_enabled = true;
        }
        m_mouse.set_start_position_2D_as_invalid();
//#endif
    }
    else if (evt.Leaving()) {
        // to remove hover on objects when the mouse goes out of this canvas
        m_mouse.position = Vec2d(-1.0, -1.0);
        m_dirty = true;
    }
    else if (evt.LeftDClick()) {
        // switch to object panel if double click on object, otherwise switch to global panel if double click on background
        if (selected_object_idx >= 0)
            post_event(SimpleEvent(EVT_GLCANVAS_SWITCH_TO_OBJECT));
        else
            post_event(SimpleEvent(EVT_GLCANVAS_SWITCH_TO_GLOBAL));
    }
    else if (evt.LeftDown() || evt.RightDown() || evt.MiddleDown()) {
        //BBS: add orient deactivate logic
        if (!m_gizmos.on_mouse(evt)) {
            if (_deactivate_arrange_menu() || _deactivate_orient_menu())
                return;
        }

        // If user pressed left or right button we first check whether this happened on a volume or not.
        m_layers_editing.state = LayersEditing::Unknown;
        if (mouse_in_layer_editing) {
            // A volume is selected and the mouse is inside the layer thickness bar.
            // Start editing the layer height.
            m_layers_editing.state = LayersEditing::Editing;
            _perform_layer_editing_action(&evt);
        }

        else {
            // BBS: define Alt key to enable volume selection mode
            m_selection.set_volume_selection_mode(evt.AltDown() ? Selection::Volume : Selection::Instance);
            if (evt.LeftDown() && evt.ShiftDown() && m_picking_enabled && m_layers_editing.state != LayersEditing::Editing) {
                if (/*m_gizmos.get_current_type() != GLGizmosManager::SlaSupports
                    &&*/ m_gizmos.get_current_type() != GLGizmosManager::FdmSupports
                    && m_gizmos.get_current_type() != GLGizmosManager::Seam
                    && m_gizmos.get_current_type() != GLGizmosManager::Cut
                    && m_gizmos.get_current_type() != GLGizmosManager::MmSegmentation
                    && m_gizmos.get_current_type() != GLGizmosManager::FuzzySkin) {
                    m_rectangle_selection.start_dragging(m_mouse.position, evt.ShiftDown() ? GLSelectionRectangle::Select : GLSelectionRectangle::Deselect);
                    m_dirty = true;
                }
            }
            else {
                // Select volume in this 3D canvas.
                // Don't deselect a volume if layer editing is enabled or any gizmo is active. We want the object to stay selected
                // during the scene manipulation.

                if (m_picking_enabled && (!any_gizmo_active || !evt.CmdDown()) && (!m_hover_volume_idxs.empty())) {
                    if (evt.LeftDown() && !m_hover_volume_idxs.empty()) {
                        int volume_idx = get_first_hover_volume_idx();
                        bool already_selected = m_selection.contains_volume(volume_idx);
                        bool ctrl_down = evt.CmdDown();
                        bool alt_down  = evt.AltDown();
                        Selection::IndicesList curr_idxs = m_selection.get_volume_idxs();

                        if (already_selected && ctrl_down)
                            m_selection.remove(volume_idx);
                        else if (alt_down) {
                            Selection::EMode mode = Selection::Volume;
                            if (already_selected) {
                                std::vector<unsigned int> volume_idxs;
                                for (auto idx : curr_idxs) { volume_idxs.emplace_back(idx); }
                                m_selection.remove_volumes(mode, volume_idxs);
                            }
                            std::vector<unsigned int> add_volume_idxs;
                            add_volume_idxs.emplace_back(volume_idx);
                            m_selection.add_volumes(mode, add_volume_idxs, true);
                        }
                        else {
                            m_selection.add(volume_idx, !ctrl_down, true);
                            m_mouse.drag.move_requires_threshold = !already_selected;
                            if (already_selected)
                                m_mouse.set_move_start_threshold_position_2D_as_invalid();
                            else
                                m_mouse.drag.move_start_threshold_position_2D = pos;
                        }

                        // propagate event through callback
                        if (curr_idxs != m_selection.get_volume_idxs()) {
                            if (m_selection.is_empty())
                                m_gizmos.reset_all_states();
                            else
                                m_gizmos.refresh_on_off_state();

                            m_gizmos.update_data();
                            post_event(SimpleEvent(EVT_GLCANVAS_OBJECT_SELECT));
                            m_dirty = true;
                        }
                    }
                }

                if (!m_hover_volume_idxs.empty()) {
                    if (evt.LeftDown() && m_moving_enabled && m_mouse.drag.move_volume_idx == -1) {
                        // Only accept the initial position, if it is inside the volume bounding box.
                        int volume_idx = get_first_hover_volume_idx();
                        BoundingBoxf3 volume_bbox = m_volumes.volumes[volume_idx]->transformed_bounding_box();
                        volume_bbox.offset(1.0);
                        const bool is_cut_connector_selected = m_selection.is_any_connector();
                        if ((!any_gizmo_active || !evt.CmdDown()) && volume_bbox.contains(m_mouse.scene_position) && !is_cut_connector_selected) {
                            m_volumes.volumes[volume_idx]->hover = GLVolume::HS_None;
                            // The dragging operation is initiated.
                            m_mouse.drag.move_volume_idx = volume_idx;
                            m_selection.setup_cache();
                            m_mouse.drag.start_position_3D = m_mouse.scene_position;
                            m_sequential_print_clearance_first_displacement = true;
                            m_moving = true;
                        }
                    }
                }
            }
        }
    }
    else if (evt.Dragging() && evt.LeftIsDown() && m_mouse.drag.move_volume_idx != -1 && m_layers_editing.state == LayersEditing::Unknown) {
        if (m_canvas_type != ECanvasType::CanvasAssembleView) {
            if (!m_mouse.drag.move_requires_threshold) {
                m_mouse.dragging = true;
                Vec3d cur_pos = m_mouse.drag.start_position_3D;
                // we do not want to translate objects if the user just clicked on an object while pressing shift to remove it from the selection and then drag
                if (m_selection.contains_volume(get_first_hover_volume_idx())) {
                    const Camera& camera = wxGetApp().plater()->get_camera();
                    if (std::abs(camera.get_dir_forward()(2)) < EPSILON) {
                        // side view -> move selected volumes orthogonally to camera view direction
                        Linef3 ray = mouse_ray(pos);
                        Vec3d dir = ray.unit_vector();
                        // finds the intersection of the mouse ray with the plane parallel to the camera viewport and passing throught the starting position
                        // use ray-plane intersection see i.e. https://en.wikipedia.org/wiki/Line%E2%80%93plane_intersection algebric form
                        // in our case plane normal and ray direction are the same (orthogonal view)
                        // when moving to perspective camera the negative z unit axis of the camera needs to be transformed in world space and used as plane normal
                        Vec3d inters = ray.a + (m_mouse.drag.start_position_3D - ray.a).dot(dir) / dir.squaredNorm() * dir;
                        // vector from the starting position to the found intersection
                        Vec3d inters_vec = inters - m_mouse.drag.start_position_3D;

                        Vec3d camera_right = camera.get_dir_right();
                        Vec3d camera_up = camera.get_dir_up();

                        // finds projection of the vector along the camera axes
                        double projection_x = inters_vec.dot(camera_right);
                        double projection_z = inters_vec.dot(camera_up);

                        // apply offset
                        cur_pos = m_mouse.drag.start_position_3D + projection_x * camera_right + projection_z * camera_up;
                    }
                    else {
                        // Generic view
                        // Get new position at the same Z of the initial click point.
                        float z0 = 0.0f;
                        float z1 = 1.0f;
                        cur_pos = Linef3(_mouse_to_3d(pos, &z0), _mouse_to_3d(pos, &z1)).intersect_plane(m_mouse.drag.start_position_3D(2));
                    }
                }

                TransformationType trafo_type;
                trafo_type.set_relative();
                m_selection.translate(cur_pos - m_mouse.drag.start_position_3D, trafo_type);
                if (current_printer_technology() == ptFFF && (fff_print()->config().print_sequence == PrintSequence::ByObject))
                    update_sequential_clearance();
                // BBS
                //wxGetApp().obj_manipul()->set_dirty();
                m_dirty = true;
            }
        }
    }
    else if (evt.Dragging() && evt.LeftIsDown() && m_picking_enabled && m_rectangle_selection.is_dragging()) {
        //BBS not in assemble view
        if (m_canvas_type != ECanvasType::CanvasAssembleView) {
            m_rectangle_selection.dragging(pos.cast<double>());
            m_dirty = true;
        }
    }
    else if (evt.Dragging() || is_camera_rotate(evt, swap_mouse_buttons) || is_camera_pan(evt, swap_mouse_buttons)) {
        m_mouse.dragging = true;

        if (m_layers_editing.state != LayersEditing::Unknown && layer_editing_object_idx != -1) {
            if (m_layers_editing.state == LayersEditing::Editing) {
                _perform_layer_editing_action(&evt);
                m_mouse.position = pos.cast<double>();
            }
        }
        // do not process the dragging if the left mouse was set down in another canvas
        else if (is_camera_rotate(evt, swap_mouse_buttons)) {
            // Orca: Sphere rotation for painting view
            // if dragging over blank area with left button or button functions swapped then rotate
            if ((any_gizmo_active || swap_mouse_buttons || m_hover_volume_idxs.empty()) && m_mouse.is_start_position_3D_defined()) {
                Camera& camera = wxGetApp().plater()->get_camera();
                auto mult_pref = wxGetApp().app_config->get("camera_orbit_mult");
                const double mult = mult_pref.empty() ? 1.0 : std::stod(mult_pref);
                const Vec3d rot = (Vec3d(pos.x(), pos.y(), 0.) - m_mouse.drag.start_position_3D) * (PI * TRACKBALLSIZE / 180.) * mult;
                if (this->m_canvas_type == ECanvasType::CanvasAssembleView || m_gizmos.get_current_type() == GLGizmosManager::FdmSupports ||
                    m_gizmos.get_current_type() == GLGizmosManager::Seam || m_gizmos.get_current_type() == GLGizmosManager::MmSegmentation ||
                    m_gizmos.get_current_type() == GLGizmosManager::FuzzySkin) {
                    Vec3d rotate_target = Vec3d::Zero();
                    if (!m_selection.is_empty())
                        rotate_target = m_selection.get_bounding_box().center();
                    else
                        rotate_target = volumes_bounding_box().center();
                    camera.rotate_on_sphere_with_target(rot.x(), rot.y(), false, rotate_target);
                }
                else {
                    if (wxGetApp().app_config->get_bool("use_free_camera"))
                        // Virtual track ball (similar to the 3DConnexion mouse).
                        camera.rotate_local_around_target(Vec3d(rot.y(), rot.x(), 0.));
                    else {
                        // Forces camera right vector to be parallel to XY plane in case it has been misaligned using the 3D mouse free rotation.
                        // It is cheaper to call this function right away instead of testing wxGetApp().plater()->get_mouse3d_controller().connected(),
                        // which checks an atomics (flushes CPU caches).
                        // See GH issue #3816.
                        bool rotate_limit = current_printer_technology() != ptSLA;

                        camera.recover_from_free_camera();
                        if (evt.ControlDown() || evt.CmdDown()) {
                            if ((m_rotation_center.x() == 0.f) && (m_rotation_center.y() == 0.f) && (m_rotation_center.z() == 0.f)) {
                                auto canvas_w = float(get_canvas_size().get_width());
                                auto canvas_h = float(get_canvas_size().get_height());
                                Point screen_center(canvas_w/2, canvas_h/2);
                                m_rotation_center = _mouse_to_3d(screen_center);
                                m_rotation_center(2) = 0.f;
                            }
                            camera.rotate_on_sphere_with_target(rot.x(), rot.y(), rotate_limit, m_rotation_center);
                        } else {
                            Vec3d rotate_target = Vec3d::Zero();
                            if (m_canvas_type == ECanvasType::CanvasPreview) {
                                PartPlate *plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
                                if (plate)
                                    rotate_target = plate->get_bounding_box().center();
                            }
                            else {
                                if (!m_selection.is_empty())
                                    rotate_target = m_selection.get_bounding_box().center();
                                else {
                                    // Rotate around the center of objects on current plate
                                    auto bbox = volumes_bounding_box(true);
                                    if (!bbox.defined) {
                                        // Rotate around current plate center if current plate is empty
                                        bbox = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_bounding_box();
                                    }
                                    rotate_target = bbox.center();
                                }
                            }

                            if (!rotate_target.isZero())
                                camera.rotate_on_sphere_with_target(rot.x(), rot.y(), rotate_limit, rotate_target);
                            else
                                camera.rotate_on_sphere(rot.x(), rot.y(), rotate_limit);
                        }
                    }
                }

                camera.auto_type(Camera::EType::Perspective);
                m_dirty = true;
                m_mouse.ignore_right_up = true;  // will be reset on button up event even if not right button is pressed
            }

            m_camera_movement = true;
            m_mouse.drag.start_position_3D = Vec3d((double)pos(0), (double)pos(1), 0.0);
        }
        else if (is_camera_pan(evt, swap_mouse_buttons)) {
            // if dragging with right button or if button functions swapped and dragging with left button over blank area then pan
            if (m_mouse.is_start_position_2D_defined()) {
                // get point in model space at Z = 0
                float z = 0.0f;
                const Vec3d& cur_pos = _mouse_to_3d(pos, &z);
                Vec3d orig = _mouse_to_3d(m_mouse.drag.start_position_2D, &z);
                Camera& camera = wxGetApp().plater()->get_camera();
                if (this->m_canvas_type != ECanvasType::CanvasAssembleView) {
                    if (wxGetApp().app_config->get_bool("use_free_camera"))
                        // Forces camera right vector to be parallel to XY plane in case it has been misaligned using the 3D mouse free rotation.
                        // It is cheaper to call this function right away instead of testing wxGetApp().plater()->get_mouse3d_controller().connected(),
                        // which checks an atomics (flushes CPU caches).
                        // See GH issue #3816.
                        camera.recover_from_free_camera();
                }

                camera.set_target(camera.get_target() + orig - cur_pos);
                m_dirty = true;
                m_mouse.ignore_right_up = true;  // will be reset on button up event even if not right button is pressed
            }

            m_camera_movement = true;
            m_mouse.drag.start_position_2D = pos;
        }
    }
    else if ((evt.LeftUp() || evt.MiddleUp() || evt.RightUp()) ||
               (m_camera_movement && !is_camera_rotate(evt, swap_mouse_buttons) && !is_camera_pan(evt, swap_mouse_buttons))) {
        m_mouse.position = pos.cast<double>();

        if (swap_mouse_buttons ? evt.RightUp() : evt.LeftUp()) {
            m_rotation_center(0) = m_rotation_center(1) = m_rotation_center(2) = 0.f;
        }

        if (m_layers_editing.state != LayersEditing::Unknown) {
            m_layers_editing.state = LayersEditing::Unknown;
            _stop_timer();
            m_layers_editing.accept_changes(*this);
        }
        else if (m_mouse.drag.move_volume_idx != -1 && m_mouse.dragging) {
            do_move(L("Move Object"));
            // BBS
            //wxGetApp().obj_manipul()->set_dirty();
            // Let the plater know that the dragging finished, so a delayed refresh
            // of the scene with the background processing data should be performed.
            post_event(SimpleEvent(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED));
        }
        else if (evt.LeftUp() && m_picking_enabled && m_rectangle_selection.is_dragging() && m_layers_editing.state != LayersEditing::Editing) {
            //BBS: don't use alt as de-select
            //if (evt.ShiftDown() || evt.AltDown())
            if (evt.ShiftDown())
                _update_selection_from_hover();

            m_rectangle_selection.stop_dragging();
        }
        else if (evt.LeftUp() && !m_mouse.ignore_left_up && !m_mouse.dragging && m_hover_volume_idxs.empty() && m_hover_plate_idxs.empty() && !is_layers_editing_enabled()) {
            // deselect and propagate event through callback
            if (!evt.ShiftDown() && (!any_gizmo_active || !evt.CmdDown()) && m_picking_enabled)
                deselect_all();
        }
        //BBS Select plate in this 3D canvas.
        else if (evt.LeftUp() && !m_mouse.dragging && m_picking_enabled && !m_hover_plate_idxs.empty() && (m_canvas_type == CanvasView3D) && !is_layers_editing_enabled())
        {
                int hover_idx = m_hover_plate_idxs.front();
                wxGetApp().plater()->select_plate_by_hover_id(hover_idx);
                //wxGetApp().plater()->get_partplate_list().select_plate_view();
                //deselect all the objects
                if (m_gizmos.get_current_type() != GLGizmosManager::MeshBoolean && m_hover_volume_idxs.empty())
                    deselect_all();
        }
        else if (evt.RightUp() && !is_layers_editing_enabled()) {
            // forces a frame render to ensure that m_hover_volume_idxs is updated even when the user right clicks while
            // the context menu is already shown
            render();
            if (!m_hover_volume_idxs.empty()) {
                // if right clicking on volume, propagate event through callback (shows context menu)
                int volume_idx = get_first_hover_volume_idx();
                if (!m_volumes.volumes[volume_idx]->is_wipe_tower // no context menu for the wipe tower
                    /*&& m_gizmos.get_current_type() != GLGizmosManager::SlaSupports*/)  // disable context menu when the gizmo is open
                {
                    // forces the selection of the volume
                    /* m_selection.add(volume_idx); // #et_FIXME_if_needed
                     * To avoid extra "Add-Selection" snapshots,
                     * call add() with check_for_already_contained=true
                     * */
                    m_selection.add(volume_idx, true, true);
                    m_gizmos.refresh_on_off_state();
                    post_event(SimpleEvent(EVT_GLCANVAS_OBJECT_SELECT));
                    m_gizmos.update_data();
                    // BBS
                    //wxGetApp().obj_manipul()->set_dirty();
                    // forces a frame render to update the view before the context menu is shown
                    render();
                }
            }

            //BBS change plate selection
            if (!m_hover_plate_idxs.empty() && (m_canvas_type == CanvasView3D) && !m_mouse.dragging) {
                int hover_idx = m_hover_plate_idxs.front();
                wxGetApp().plater()->select_plate_by_hover_id(hover_idx, true);
                if (m_hover_volume_idxs.empty())
                    deselect_all();
                render();
            }

            //ORCA allow right click on empty space while an object selected
            if (m_hover_plate_idxs.empty() && m_hover_volume_idxs.empty() && (m_canvas_type == CanvasView3D) && !m_mouse.dragging) {
                deselect_all();
                render();
            }

            Vec2d logical_pos = pos.cast<double>();
#if ENABLE_RETINA_GL
            const float factor = m_retina_helper->get_scale_factor();
            logical_pos = logical_pos.cwiseQuotient(Vec2d(factor, factor));
#endif // ENABLE_RETINA_GL

            if (!m_mouse.ignore_right_up && m_gizmos.get_current_type() == GLGizmosManager::EType::Undefined) {
                //BBS post right click event
                if (!m_hover_plate_idxs.empty()) {
                    post_event(RBtnPlateEvent(EVT_GLCANVAS_PLATE_RIGHT_CLICK, { logical_pos, m_hover_plate_idxs.front() }));
                }
                else {
                    // do not post the event if the user is panning the scene
                    // or if right click was done over the wipe tower
                    bool post_right_click_event = m_hover_volume_idxs.empty() || !m_volumes.volumes[get_first_hover_volume_idx()]->is_wipe_tower;
                    if (post_right_click_event)
                        post_event(RBtnEvent(EVT_GLCANVAS_RIGHT_CLICK, { logical_pos, m_hover_volume_idxs.empty() }));
                }
            }
        }

        mouse_up_cleanup();
    }
    else if (evt.Moving()) {
        m_mouse.position = pos.cast<double>();

        // updates gizmos overlay
        if (m_selection.is_empty())
            m_gizmos.reset_all_states();

        m_dirty = true;
    }
    else
        evt.Skip();

    // Detection of doubleclick on text to open emboss edit window
    auto type = m_gizmos.get_current_type();
    if (evt.LeftDClick() && !m_hover_volume_idxs.empty() &&
        (type == GLGizmosManager::EType::Undefined ||
         type == GLGizmosManager::EType::Move ||
         type == GLGizmosManager::EType::Rotate ||
         type == GLGizmosManager::EType::Scale ||
         type == GLGizmosManager::EType::Emboss ||
         type == GLGizmosManager::EType::Svg) ) {
        for (int hover_volume_id : m_hover_volume_idxs) {
            const GLVolume &hover_gl_volume = *m_volumes.volumes[hover_volume_id];
            int object_idx = hover_gl_volume.object_idx();
            if (object_idx < 0 || static_cast<size_t>(object_idx) >= m_model->objects.size()) continue;
            const ModelObject* hover_object = m_model->objects[object_idx];
            int hover_volume_idx = hover_gl_volume.volume_idx();
            if (hover_volume_idx < 0 || static_cast<size_t>(hover_volume_idx) >= hover_object->volumes.size()) continue;
            const ModelVolume* hover_volume = hover_object->volumes[hover_volume_idx];

            if (hover_volume->text_configuration.has_value()) {
                m_selection.add_volumes(Selection::EMode::Volume, {(unsigned) hover_volume_id});
                if (type != GLGizmosManager::EType::Emboss)
                    m_gizmos.open_gizmo(GLGizmosManager::EType::Emboss);
                wxGetApp().obj_list()->update_selections();
                return;
            } else if (hover_volume->emboss_shape.has_value()) {
                m_selection.add_volumes(Selection::EMode::Volume, {(unsigned) hover_volume_id});
                if (type != GLGizmosManager::EType::Svg)
                    m_gizmos.open_gizmo(GLGizmosManager::EType::Svg);
                wxGetApp().obj_list()->update_selections();
                return;
            }
        }
    }

    if (m_moving)
        show_sinking_contours();

#ifdef __WXMSW__
	if (on_enter_workaround)
		m_mouse.position = Vec2d(-1., -1.);
#endif /* __WXMSW__ */
}

void GLCanvas3D::on_paint(wxPaintEvent& evt)
{
    if (m_initialized)
        m_dirty = true;
    else
        // Call render directly, so it gets initialized immediately, not from On Idle handler.
        this->render();
}

void GLCanvas3D::force_set_focus() {
    m_canvas->SetFocus();
};

void GLCanvas3D::on_set_focus(wxFocusEvent& evt)
{
    m_tooltip_enabled = false;
    if (m_canvas_type == ECanvasType::CanvasPreview) {
        // update thumbnails and update plate toolbar
        wxGetApp().plater()->update_all_plate_thumbnails();
        _update_imgui_select_plate_toolbar();
    }
    _refresh_if_shown_on_screen();
    m_tooltip_enabled = true;
    m_is_touchpad_navigation = wxGetApp().app_config->get_bool("camera_navigation_style");
}

bool GLCanvas3D::is_camera_rotate(const wxMouseEvent& evt, const bool buttonsSwapped) const
{
    if (m_is_touchpad_navigation) {
        return evt.Moving() && evt.AltDown() && !evt.ShiftDown();
    } else {
        return evt.Dragging() && (buttonsSwapped ? evt.RightIsDown() : evt.LeftIsDown());
    }
}

bool GLCanvas3D::is_camera_pan(const wxMouseEvent& evt, const bool buttonsSwapped) const
{
    if (m_is_touchpad_navigation) {
        return evt.Moving() && evt.ShiftDown() && !evt.AltDown();
    } else {
        return evt.Dragging() && (evt.MiddleIsDown() || (buttonsSwapped ? evt.LeftIsDown() : evt.RightIsDown()));
    }
}

Size GLCanvas3D::get_canvas_size() const
{
    int w = 0;
    int h = 0;

    if (m_canvas != nullptr)
        m_canvas->GetSize(&w, &h);

#if ENABLE_RETINA_GL
    const float factor = m_retina_helper->get_scale_factor();
    w *= factor;
    h *= factor;
#else
    const float factor = 1.0f;
#endif

    return Size(w, h, factor);
}

Vec2d GLCanvas3D::get_local_mouse_position() const
{
    if (m_canvas == nullptr)
		return Vec2d::Zero();

    wxPoint mouse_pos = m_canvas->ScreenToClient(wxGetMousePosition());
    const double factor =
#if ENABLE_RETINA_GL
        m_retina_helper->get_scale_factor();
#else
        1.0;
#endif
    return Vec2d(factor * mouse_pos.x, factor * mouse_pos.y);
}

void GLCanvas3D::set_tooltip(const std::string& tooltip)
{
    if (m_canvas != nullptr)
        m_tooltip.set_text(tooltip);
}

void GLCanvas3D::do_move(const std::string& snapshot_type)
{
    if (m_model == nullptr)
        return;

    if (!snapshot_type.empty())
        wxGetApp().plater()->take_snapshot(snapshot_type);

    std::set<std::pair<int, int>> done;  // keeps track of modified instances
    bool object_moved = false;

    // BBS: support wipe-tower for multi-plates
    int n_plates = wxGetApp().plater()->get_partplate_list().get_plate_count();
    std::vector<Vec3d> wipe_tower_origins(n_plates, Vec3d::Zero());

    Selection::EMode selection_mode = m_selection.get_mode();

    for (const GLVolume* v : m_volumes.volumes) {
        int object_idx = v->object_idx();
        int instance_idx = v->instance_idx();
        int volume_idx = v->volume_idx();

        if (volume_idx < 0)
            continue;

        std::pair<int, int> done_id(object_idx, instance_idx);

        if (0 <= object_idx && object_idx < (int)m_model->objects.size()) {
            done.insert(done_id);

            // Move instances/volumes
            ModelObject* model_object = m_model->objects[object_idx];
            if (model_object != nullptr) {
                if (selection_mode == Selection::Instance) {
                    if (m_canvas_type == GLCanvas3D::ECanvasType::CanvasAssembleView) {
                        if ((model_object->instances[instance_idx]->get_assemble_offset() - v->get_instance_offset()).norm() > 1e-2) {
                            model_object->instances[instance_idx]->set_assemble_transformation(v->get_instance_transformation());
                        }
                    } else {
                        model_object->instances[instance_idx]->set_transformation(v->get_instance_transformation());
                    }
                }
                else if (selection_mode == Selection::Volume) {
                    auto cur_mv = model_object->volumes[volume_idx];
                    if (cur_mv->get_transformation() != v->get_volume_transformation()) {
                        cur_mv->set_transformation(v->get_volume_transformation());
                        // BBS: backup
                        Slic3r::save_object_mesh(*model_object);
                    }
                }

                object_moved = true;
                model_object->invalidate_bounding_box();
            }
        }
        else if (object_idx >= 1000 && object_idx < 1000 + n_plates) {
            // Move a wipe tower proxy.
            wipe_tower_origins[object_idx - 1000] = v->get_volume_offset();
        }
    }

    //BBS: notify instance updates to part plater list
    m_selection.notify_instance_update(-1, 0);

    // Fixes flying instances
    for (const std::pair<int, int>& i : done) {
        ModelObject* m = m_model->objects[i.first];
        const double shift_z = m->get_instance_min_z(i.second);
        //BBS: don't call translate if the z is zero
        if ((current_printer_technology() == ptSLA || shift_z > SINKING_Z_THRESHOLD) && (shift_z != 0.0f)) {
            const Vec3d shift(0.0, 0.0, -shift_z);
            m_selection.translate(i.first, i.second, shift);
            m->translate_instance(i.second, shift);
            //BBS: notify instance updates to part plater list
            m_selection.notify_instance_update(i.first, i.second);
        }
        wxGetApp().obj_list()->update_info_items(static_cast<size_t>(i.first));
    }
    //BBS: nofity object list to update
    wxGetApp().plater()->sidebar().obj_list()->update_plate_values_for_items();

    if (object_moved)
        post_event(SimpleEvent(EVT_GLCANVAS_INSTANCE_MOVED));

    // BBS: support wipe-tower for multi-plates
    for (int plate_id = 0; plate_id < wipe_tower_origins.size(); plate_id++) {
        Vec3d& wipe_tower_origin = wipe_tower_origins[plate_id];
        if (wipe_tower_origin == Vec3d::Zero())
            continue;

        PartPlateList& ppl = wxGetApp().plater()->get_partplate_list();
        DynamicConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
        Vec3d plate_origin = ppl.get_plate(plate_id)->get_origin();
        ConfigOptionFloat wipe_tower_x(wipe_tower_origin(0) - plate_origin(0));
        ConfigOptionFloat wipe_tower_y(wipe_tower_origin(1) - plate_origin(1));

        ConfigOptionFloats* wipe_tower_x_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_x", true);
        ConfigOptionFloats* wipe_tower_y_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_y", true);
        wipe_tower_x_opt->set_at(&wipe_tower_x, plate_id, 0);
        wipe_tower_y_opt->set_at(&wipe_tower_y, plate_id, 0);
    }

    reset_sequential_print_clearance();

    m_dirty = true;
}

void GLCanvas3D::do_rotate(const std::string& snapshot_type)
{
    if (m_model == nullptr)
        return;

    if (!snapshot_type.empty())
        wxGetApp().plater()->take_snapshot(snapshot_type);

    // stores current min_z of instances
    std::map<std::pair<int, int>, double> min_zs;
    for (int i = 0; i < static_cast<int>(m_model->objects.size()); ++i) {
        const ModelObject* obj = m_model->objects[i];
        for (int j = 0; j < static_cast<int>(obj->instances.size()); ++j) {
            if (snapshot_type == L("Gizmo-Place on Face") && m_selection.get_object_idx() == i) {
                // This means we are flattening this object. In that case pretend
                // that it is not sinking (even if it is), so it is placed on bed
                // later on (whatever is sinking will be left sinking).
                min_zs[{ i, j }] = SINKING_Z_THRESHOLD;
            }
            else
                min_zs[{ i, j }] = obj->instance_bounding_box(j).min.z();

        }
    }

    std::set<std::pair<int, int>> done;  // keeps track of modified instances

    Selection::EMode selection_mode = m_selection.get_mode();

    for (const GLVolume* v : m_volumes.volumes) {
        const int object_idx = v->object_idx();
        if (object_idx < 0 || (int)m_model->objects.size() <= object_idx)
            continue;

        const int instance_idx = v->instance_idx();
        const int volume_idx = v->volume_idx();

        if (volume_idx < 0)
            continue;

        done.insert(std::pair<int, int>(object_idx, instance_idx));

        // Rotate instances/volumes.
        ModelObject* model_object = m_model->objects[object_idx];
        if (model_object != nullptr) {
            if (selection_mode == Selection::Instance) {
                if (m_canvas_type == GLCanvas3D::ECanvasType::CanvasAssembleView) {
                    model_object->instances[instance_idx]->set_assemble_from_transform(v->get_instance_transformation().get_matrix());
                } else {
                    model_object->instances[instance_idx]->set_transformation(v->get_instance_transformation());
                }
            }
            else if (selection_mode == Selection::Volume) {
                auto cur_mv = model_object->volumes[volume_idx];
                if (cur_mv->get_transformation() != v->get_volume_transformation()) {
                    cur_mv->set_transformation(v->get_volume_transformation());
                    // BBS: backup
                    Slic3r::save_object_mesh(*model_object);
                }
            }
            model_object->invalidate_bounding_box();
        }
    }

    //BBS: notify instance updates to part plater list
    m_selection.notify_instance_update(-1, -1);
    if (m_canvas_type != CanvasAssembleView) {
        // Fixes sinking/flying instances
        for (const std::pair<int, int> &i : done) {
            ModelObject *m = m_model->objects[i.first];

            // BBS: don't call translate if the z is zero
            const double shift_z = m->get_instance_min_z(i.second);
            // leave sinking instances as sinking
            if ((min_zs.find({i.first, i.second})->second >= SINKING_Z_THRESHOLD || shift_z > SINKING_Z_THRESHOLD) && (shift_z != 0.0f)) {
                const Vec3d shift(0.0, 0.0, -shift_z);
                m_selection.translate(i.first, i.second, shift);
                m->translate_instance(i.second, shift);
                // BBS: notify instance updates to part plater list
                m_selection.notify_instance_update(i.first, i.second);
            }

            wxGetApp().obj_list()->update_info_items(static_cast<size_t>(i.first));
        }
    }
    //BBS: nofity object list to update
    wxGetApp().plater()->sidebar().obj_list()->update_plate_values_for_items();

    if (!done.empty())
        post_event(SimpleEvent(EVT_GLCANVAS_INSTANCE_ROTATED));

    m_dirty = true;
}

void GLCanvas3D::do_scale(const std::string& snapshot_type)
{
    if (m_model == nullptr)
        return;

    if (!snapshot_type.empty())
        wxGetApp().plater()->take_snapshot(snapshot_type);

    // stores current min_z of instances
    std::map<std::pair<int, int>, double> min_zs;
    if (!snapshot_type.empty()) {
        for (int i = 0; i < static_cast<int>(m_model->objects.size()); ++i) {
            const ModelObject* obj = m_model->objects[i];
            for (int j = 0; j < static_cast<int>(obj->instances.size()); ++j) {
                min_zs[{ i, j }] = obj->instance_bounding_box(j).min.z();
            }
        }
    }

    std::set<std::pair<int, int>> done;  // keeps track of modified instances

    Selection::EMode selection_mode = m_selection.get_mode();

    for (const GLVolume* v : m_volumes.volumes) {
        const int object_idx = v->object_idx();
        if (object_idx < 0 || (int)m_model->objects.size() <= object_idx)
            continue;

        const int instance_idx = v->instance_idx();
        const int volume_idx = v->volume_idx();

        if (volume_idx < 0)
            continue;

        done.insert(std::pair<int, int>(object_idx, instance_idx));

        // Rotate instances/volumes
        ModelObject* model_object = m_model->objects[object_idx];
        if (model_object != nullptr) {
            if (selection_mode == Selection::Instance) {
                model_object->instances[instance_idx]->set_transformation(v->get_instance_transformation());
            }
            else if (selection_mode == Selection::Volume) {
                auto cur_mv = model_object->volumes[volume_idx];
                if (cur_mv->get_transformation() != v->get_volume_transformation()) {
                    model_object->instances[instance_idx]->set_transformation(v->get_instance_transformation());
                    cur_mv->set_transformation(v->get_volume_transformation());
                    // BBS: backup
                    Slic3r::save_object_mesh(*model_object);
                }
            }
            model_object->invalidate_bounding_box();
        }
    }

    //BBS: notify instance updates to part plater list
    m_selection.notify_instance_update(-1, -1);

    // Fixes sinking/flying instances
    for (const std::pair<int, int>& i : done) {
        ModelObject* m = m_model->objects[i.first];

        //BBS: don't call translate if the z is zero
        double shift_z = m->get_instance_min_z(i.second);
        // leave sinking instances as sinking
        if ((min_zs.empty() || min_zs.find({ i.first, i.second })->second >= SINKING_Z_THRESHOLD || shift_z > SINKING_Z_THRESHOLD) && (shift_z != 0.0f)) {
            Vec3d shift(0.0, 0.0, -shift_z);
            m_selection.translate(i.first, i.second, shift);
            m->translate_instance(i.second, shift);
            //BBS: notify instance updates to part plater list
            m_selection.notify_instance_update(i.first, i.second);
        }
        wxGetApp().obj_list()->update_info_items(static_cast<size_t>(i.first));
    }
    //BBS: nofity object list to update
    wxGetApp().plater()->sidebar().obj_list()->update_plate_values_for_items();
    //BBS: notify object info update
    wxGetApp().plater()->show_object_info();

    if (!done.empty())
        post_event(SimpleEvent(EVT_GLCANVAS_INSTANCE_SCALED));

    m_dirty = true;
}

void GLCanvas3D::do_center()
{
    if (m_model == nullptr)
        return;

    m_selection.center();
}

void GLCanvas3D::do_drop()
{
    if (m_model == nullptr)
        return;

    m_selection.drop();
}

void GLCanvas3D::do_center_plate(const int plate_idx) {
    if (m_model == nullptr)
        return;

    m_selection.center_plate(plate_idx);
}

void GLCanvas3D::do_mirror(const std::string& snapshot_type)
{
    if (m_model == nullptr)
        return;

    if (!snapshot_type.empty())
        wxGetApp().plater()->take_snapshot(snapshot_type);

    // stores current min_z of instances
    std::map<std::pair<int, int>, double> min_zs;
    if (!snapshot_type.empty()) {
        for (int i = 0; i < static_cast<int>(m_model->objects.size()); ++i) {
            const ModelObject* obj = m_model->objects[i];
            for (int j = 0; j < static_cast<int>(obj->instances.size()); ++j) {
                min_zs[{ i, j }] = obj->instance_bounding_box(j).min.z();
            }
        }
    }

    std::set<std::pair<int, int>> done;  // keeps track of modified instances

    Selection::EMode selection_mode = m_selection.get_mode();

    for (const GLVolume* v : m_volumes.volumes) {
        int object_idx = v->object_idx();
        if (object_idx < 0 || (int)m_model->objects.size() <= object_idx)
            continue;

        int instance_idx = v->instance_idx();
        int volume_idx = v->volume_idx();

        done.insert(std::pair<int, int>(object_idx, instance_idx));

        // Mirror instances/volumes
        ModelObject* model_object = m_model->objects[object_idx];
        if (model_object != nullptr) {
            if (selection_mode == Selection::Instance)
                model_object->instances[instance_idx]->set_transformation(v->get_instance_transformation());
            else if (selection_mode == Selection::Volume) {
                if (model_object->volumes[volume_idx]->get_transformation() != v->get_volume_transformation()) {
                    model_object->volumes[volume_idx]->set_transformation(v->get_volume_transformation());
                    // BBS: backup
                    Slic3r::save_object_mesh(*model_object);
                }
            }

            model_object->invalidate_bounding_box();
        }
    }

    //BBS: notify instance updates to part plater list
    m_selection.notify_instance_update(-1, -1);

    // Fixes sinking/flying instances
    for (const std::pair<int, int>& i : done) {
        ModelObject* m = m_model->objects[i.first];

        //BBS: don't call translate if the z is zero
        double shift_z = m->get_instance_min_z(i.second);
        // leave sinking instances as sinking
        if ((min_zs.empty() || min_zs.find({ i.first, i.second })->second >= SINKING_Z_THRESHOLD || shift_z > SINKING_Z_THRESHOLD)&&(shift_z != 0.0f)) {
            Vec3d shift(0.0, 0.0, -shift_z);
            m_selection.translate(i.first, i.second, shift);
            m->translate_instance(i.second, shift);
            //BBS: notify instance updates to part plater list
            m_selection.notify_instance_update(i.first, i.second);
        }
        wxGetApp().obj_list()->update_info_items(static_cast<size_t>(i.first));

        //BBS: notify instance updates to part plater list
        PartPlateList &plate_list = wxGetApp().plater()->get_partplate_list();
        plate_list.notify_instance_update(i.first, i.second);

        //BBS: nofity object list to update
        wxGetApp().plater()->sidebar().obj_list()->update_plate_values_for_items();
    }
    //BBS: nofity object list to update
    wxGetApp().plater()->sidebar().obj_list()->update_plate_values_for_items();

    post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));

    m_dirty = true;
}

void GLCanvas3D::update_gizmos_on_off_state()
{
    set_as_dirty();
    m_gizmos.update_data();
    m_gizmos.refresh_on_off_state();
}

void GLCanvas3D::handle_sidebar_focus_event(const std::string& opt_key, bool focus_on)
{
    m_sidebar_field = focus_on ? opt_key : "";

    //BBS: this event was sent from gizmo now, no need to clear gizmo
    //if (!m_sidebar_field.empty())
    //    m_gizmos.reset_all_states();

    m_dirty = true;
}

void GLCanvas3D::handle_layers_data_focus_event(const t_layer_height_range range, const EditorType type)
{
    std::string field = "layer_" + std::to_string(type) + "_" + std::to_string(range.first) + "_" + std::to_string(range.second);
    m_gizmos.reset_all_states();
    handle_sidebar_focus_event(field, true);
}

void GLCanvas3D::update_ui_from_settings()
{
    m_dirty = true;

#if __APPLE__
    // Update OpenGL scaling on OSX after the user toggled the "use_retina_opengl" settings in Preferences dialog.
    const float orig_scaling = m_retina_helper->get_scale_factor();

    const bool use_retina = true;
    BOOST_LOG_TRIVIAL(debug) << "GLCanvas3D: Use Retina OpenGL: " << use_retina;
    m_retina_helper->set_use_retina(use_retina);
    const float new_scaling = m_retina_helper->get_scale_factor();

    if (new_scaling != orig_scaling) {
        BOOST_LOG_TRIVIAL(debug) << "GLCanvas3D: Scaling factor: " << new_scaling;

        Camera& camera = wxGetApp().plater()->get_camera();
        camera.set_zoom(camera.get_zoom() * new_scaling / orig_scaling);
        _refresh_if_shown_on_screen();
    }
#endif // ENABLE_RETINA_GL
}

// BBS: add partplate logic
GLCanvas3D::WipeTowerInfo GLCanvas3D::get_wipe_tower_info(int plate_idx) const
{
    WipeTowerInfo wti;

    for (const GLVolume* vol : m_volumes.volumes) {
        if (vol->is_wipe_tower && vol->object_idx() - 1000 == plate_idx) {
            DynamicPrintConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
            wti.m_pos = Vec2d(proj_cfg.opt<ConfigOptionFloats>("wipe_tower_x")->get_at(plate_idx),
                              proj_cfg.opt<ConfigOptionFloats>("wipe_tower_y")->get_at(plate_idx));
            // BBS: don't support rotation
            //wti.m_rotation = (M_PI/180.) * proj_cfg->opt_float("wipe_tower_rotation_angle");

            auto& preset = wxGetApp().preset_bundle->prints.get_edited_preset();
            float wt_brim_width = preset.config.opt_float("prime_tower_brim_width");

            const BoundingBoxf3& bb = vol->bounding_box();
            if (wt_brim_width < 0) wt_brim_width = WipeTower::get_auto_brim_by_height((float)bb.max.z());
            wti.m_bb = BoundingBoxf{to_2d(bb.min), to_2d(bb.max)};
            wti.m_bb.offset(wt_brim_width);

            float brim_width = wxGetApp().preset_bundle->prints.get_edited_preset().config.opt_float("prime_tower_brim_width");
            if (brim_width < 0) brim_width = WipeTower::get_auto_brim_by_height((float) bb.max.z());
            wti.m_bb.offset((brim_width));

            // BBS: the wipe tower pos might be outside bed
            PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_plate(plate_idx);
            Vec2d plate_size = plate->get_size();
            wti.m_pos.x() = std::clamp(wti.m_pos.x(), 0.0, plate_size(0) - wti.m_bb.size().x());
            wti.m_pos.y() = std::clamp(wti.m_pos.y(), 0.0, plate_size(1) - wti.m_bb.size().y());

            // BBS: add partplate logic
            wti.m_plate_idx = plate_idx;
            break;
        }
    }

    return wti;
}

Linef3 GLCanvas3D::mouse_ray(const Point& mouse_pos)
{
    float z0 = 0.0f;
    float z1 = 1.0f;
    return Linef3(_mouse_to_3d(mouse_pos, &z0), _mouse_to_3d(mouse_pos, &z1));
}

double GLCanvas3D::get_size_proportional_to_max_bed_size(double factor) const
{
    const BoundingBoxf& bbox = m_bed.build_volume().bounding_volume2d();
    return factor * std::max(bbox.size()[0], bbox.size()[1]);
}

//BBS
std::vector<Vec2f> GLCanvas3D::get_empty_cells(const Vec2f start_point, const Vec2f step)
{
    PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    BoundingBoxf3 build_volume = plate->get_build_volume();
    Vec2d vmin(build_volume.min.x(), build_volume.min.y()), vmax(build_volume.max.x(), build_volume.max.y());
    BoundingBoxf bbox(vmin, vmax);
    std::vector<Vec2f> cells;
    std::vector<Vec2f> cells_ret;
    auto min_x = start_point.x() - step(0) * int((start_point.x() - bbox.min.x()) / step(0));
    auto min_y = start_point.y() - step(1) * int((start_point.y() - bbox.min.y()) / step(1));
    cells.reserve(((bbox.max.x() - min_x) / step(0)) * ((bbox.max.y() - min_y) / step(1)));
    cells_ret.reserve(cells.size());
    for (float x = min_x; x < bbox.max.x() - step(0) / 2; x += step(0))
        for (float y = min_y; y < bbox.max.y() - step(1) / 2; y += step(1))
        {
            cells.emplace_back(x, y);
        }
    for (size_t i = 0; i < m_model->objects.size(); ++i) {
        ModelObject* model_object = m_model->objects[i];
        auto id = model_object->id().id;
        ModelInstance* model_instance0 = model_object->instances.front();
        Polygon hull_2d = model_object->convex_hull_2d(Geometry::assemble_transform({ 0.0, 0.0, model_instance0->get_offset().z() }, model_instance0->get_rotation(),
            model_instance0->get_scaling_factor(), model_instance0->get_mirror()));
        if (hull_2d.empty())
            continue;

        const auto& instances = model_object->instances;
        double rotation_z0 = instances.front()->get_rotation().z();
        for (const auto& instance : instances) {
            Geometry::Transformation transformation;
            const Vec3d& offset = instance->get_offset();
            transformation.set_offset({ scale_(offset.x()), scale_(offset.y()), 0.0 });
            transformation.set_rotation(Z, instance->get_rotation().z() - rotation_z0);
            const Transform3d& trafo = transformation.get_matrix();
            Polygon inst_hull_2d = hull_2d.transform(trafo);

            for (auto it = cells.begin(); it != cells.end(); )
            {
                if (!inst_hull_2d.contains(Point(scale_(it->x()), scale_(it->y()))))
                    cells_ret.push_back(*it);

                it++;
            }
        }
    }

    Vec2f start = start_point;
    if (start_point(0) < 0 && start_point(1) < 0) {
        start(0) = bbox.center()(0);
        start(1) = bbox.center()(1);
    }
    std::sort(cells_ret.begin(), cells_ret.end(), [start](const Vec2f& cell1, const Vec2f& cell2) {return (cell1 - start).norm() < (cell2 - start).norm(); });
    return cells_ret;
}

Vec2f GLCanvas3D::get_nearest_empty_cell(const Vec2f start_point, const Vec2f step)
{
    std::vector<Vec2f> empty_cells = get_empty_cells(start_point, step);
    if (!empty_cells.empty())
        return empty_cells.front();
    else {
        double offset = get_size_proportional_to_max_bed_size(0.05);
        return { start_point(0) + offset, start_point(1) + offset };
    }
}

void GLCanvas3D::set_cursor(ECursorType type)
{
    if ((m_canvas != nullptr) && (m_cursor_type != type))
    {
        switch (type)
        {
        case Standard: { m_canvas->SetCursor(*wxSTANDARD_CURSOR); break; }
        case Cross: { m_canvas->SetCursor(*wxCROSS_CURSOR); break; }
        }

        m_cursor_type = type;
    }
}

void GLCanvas3D::msw_rescale()
{
}

bool GLCanvas3D::has_toolpaths_to_export() const
{
    return m_gcode_viewer.can_export_toolpaths();
}

void GLCanvas3D::export_toolpaths_to_obj(const char* filename) const
{
    m_gcode_viewer.export_toolpaths_to_obj(filename);
}

void GLCanvas3D::mouse_up_cleanup()
{
    m_moving = false;
    m_camera_movement = false;
    m_mouse.drag.move_volume_idx = -1;
    m_mouse.set_start_position_3D_as_invalid();
    m_mouse.set_start_position_2D_as_invalid();
    m_mouse.dragging = false;
    m_mouse.ignore_left_up = false;
    m_mouse.ignore_right_up = false;
    m_dirty = true;

    if (m_canvas->HasCapture())
        m_canvas->ReleaseMouse();
}

bool GLCanvas3D::can_sequential_clearance_show_in_gizmo() {
    switch (m_gizmos.get_current_type()) {
    case GLGizmosManager::EType::Move:
    case GLGizmosManager::EType::Scale:
    case GLGizmosManager::EType::Rotate: {
        return true;
    }
    }
    return false;
}

void GLCanvas3D::update_sequential_clearance()
{
    if (current_printer_technology() != ptFFF || (fff_print()->config().print_sequence == PrintSequence::ByLayer))
        return;

    if (m_gizmos.is_dragging())
        return;

    // collects instance transformations from volumes
    // first define temporary cache
    unsigned int instances_count = 0;
    std::vector<std::vector<std::optional<Geometry::Transformation>>> instance_transforms;
    for (size_t obj = 0; obj < m_model->objects.size(); ++obj) {
        instance_transforms.emplace_back(std::vector<std::optional<Geometry::Transformation>>());
        const ModelObject* model_object = m_model->objects[obj];
        for (size_t i = 0; i < model_object->instances.size(); ++i) {
            instance_transforms[obj].emplace_back(std::optional<Geometry::Transformation>());
            ++instances_count;
        }
    }

    //if (instances_count == 1)
    //    return;

    // second fill temporary cache with data from volumes
    for (const GLVolume* v : m_volumes.volumes) {
        if (v->is_modifier || v->is_wipe_tower)
            continue;

        auto& transform = instance_transforms[v->object_idx()][v->instance_idx()];
        if (!transform.has_value())
            transform = v->get_instance_transformation();
    }

    // calculates objects 2d hulls (see also: Print::sequential_print_horizontal_clearance_valid())
    // this is done only the first time this method is called while moving the mouse,
    // the results are then cached for following displacements
    if (m_sequential_print_clearance_first_displacement) {
        m_sequential_print_clearance.m_hull_2d_cache.clear();
        auto [object_skirt_offset, _] = fff_print()->object_skirt_offset();
        float shrink_factor;
        if (fff_print()->is_all_objects_are_short())
            shrink_factor = scale_(std::max(0.5f * MAX_OUTER_NOZZLE_DIAMETER, object_skirt_offset) - 0.1);
        else
            shrink_factor = static_cast<float>(scale_(0.5 * fff_print()->config().extruder_clearance_radius.value + object_skirt_offset - 0.1));

        double mitter_limit = scale_(0.1);
        m_sequential_print_clearance.m_hull_2d_cache.reserve(m_model->objects.size());
        for (size_t i = 0; i < m_model->objects.size(); ++i) {
            ModelObject* model_object = m_model->objects[i];
            ModelInstance* model_instance0 = model_object->instances.front();
            Polygon hull_no_offset = model_object->convex_hull_2d(Geometry::assemble_transform({ 0.0, 0.0, model_instance0->get_offset().z() }, model_instance0->get_rotation(),
                model_instance0->get_scaling_factor(), model_instance0->get_mirror()));
            auto tmp = offset(hull_no_offset,
                // Shrink the extruder_clearance_radius a tiny bit, so that if the object arrangement algorithm placed the objects
                // exactly by satisfying the extruder_clearance_radius, this test will not trigger collision.
                shrink_factor,
                jtRound, mitter_limit);
            Polygon hull_2d = !tmp.empty() ? tmp.front() : hull_no_offset;// tmp may be empty due to clipper's bug, see STUDIO-2452

            Pointf3s& cache_hull_2d = m_sequential_print_clearance.m_hull_2d_cache.emplace_back(Pointf3s());
            cache_hull_2d.reserve(hull_2d.points.size());
            for (const Point& p : hull_2d.points) {
                cache_hull_2d.emplace_back(unscale<double>(p.x()), unscale<double>(p.y()), 0.0);
            }
        }
        m_sequential_print_clearance_first_displacement = false;
    }

    // calculates instances 2d hulls (see also: Print::sequential_print_horizontal_clearance_valid())
    //BBS: add the height logic
    PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    Polygons polygons;
    std::vector<std::pair<Polygon, float>> height_polygons;
    polygons.reserve(instances_count);
    height_polygons.reserve(instances_count);
    std::vector<struct height_info> convex_and_bounding_boxes;
    struct height_info
    {
        double         instance_height;
        BoundingBox    bounding_box;
        Polygon        hull_polygon;
    };
    for (size_t i = 0; i < instance_transforms.size(); ++i) {
        const auto& instances = instance_transforms[i];
        double rotation_z0 = instances.front()->get_rotation().z();
        int index = 0;
        for (const auto& instance : instances) {
            Geometry::Transformation transformation;
            const Vec3d& offset = instance->get_offset();
            transformation.set_offset({ offset.x(), offset.y(), 0.0 });
            transformation.set_rotation(Z, instance->get_rotation().z() - rotation_z0);
            const Transform3d& trafo = transformation.get_matrix();
            const Pointf3s& hull_2d = m_sequential_print_clearance.m_hull_2d_cache[i];
            Points inst_pts;
            inst_pts.reserve(hull_2d.size());
            for (size_t j = 0; j < hull_2d.size(); ++j) {
                const Vec3d p = trafo * hull_2d[j];
                inst_pts.emplace_back(scaled<double>(p.x()), scaled<double>(p.y()));
            }
            Polygon convex_hull(std::move(inst_pts));
            BoundingBox bouding_box = convex_hull.bounding_box();
            BoundingBox plate_bb = plate->get_bounding_box_crd();
            double instance_height = m_model->objects[i]->get_instance_max_z(index++);
            //skip the object for not current plate
            if (!plate_bb.overlap(bouding_box))
                continue;
            convex_and_bounding_boxes.push_back({instance_height, bouding_box, convex_hull});
            polygons.emplace_back(std::move(convex_hull));
        }
    }

    //sort the print instance
    std::sort(convex_and_bounding_boxes.begin(), convex_and_bounding_boxes.end(),
        [](auto &l, auto &r) {
            auto ly1 = l.bounding_box.min.y();
            auto ly2 = l.bounding_box.max.y();
            auto ry1 = r.bounding_box.min.y();
            auto ry2 = r.bounding_box.max.y();
            auto inter_min = std::max(ly1, ry1);
            auto inter_max = std::min(ly2, ry2);
            auto lx = l.bounding_box.min.x();
            auto rx = r.bounding_box.min.x();
            if (inter_max - inter_min > 0)
                return (lx < rx) || ((lx == rx)&&(ly1 < ry1));
            else
                return (ly1 < ry1);
        });

    /*bool has_interlaced_objects = false;
    for (int k = 0; k < bounding_box_count; k++)
    {
        Polygon& convex = convex_and_bounding_boxes[k].hull_polygon;
        BoundingBox& bbox = convex_and_bounding_boxes[k].bounding_box;
        auto iy1 = bbox.min.y();
        auto iy2 = bbox.max.y();

        for (int i = k+1; i < bounding_box_count; i++)
        {
            Polygon&     next_convex = convex_and_bounding_boxes[i].hull_polygon;
            BoundingBox& next_bbox   = convex_and_bounding_boxes[i].bounding_box;
            auto py1 = next_bbox.min.y();
            auto py2 = next_bbox.max.y();
            auto inter_min = std::max(iy1, py1); // min y of intersection
            auto inter_max = std::min(iy2, py2); // max y of intersection. length=max_y-min_y>0 means intersection exists
            if (inter_max - inter_min > 0) {
                has_interlaced_objects = true;
                break;
            }
        }
        if (has_interlaced_objects)
            break;
    }*/

    int bounding_box_count = convex_and_bounding_boxes.size();
    double printable_height = fff_print()->config().printable_height;
    double hc1 = fff_print()->config().extruder_clearance_height_to_lid;
    double hc2 = fff_print()->config().extruder_clearance_height_to_rod;
    for (int k = 0; k < bounding_box_count; k++)
    {
        Polygon& convex = convex_and_bounding_boxes[k].hull_polygon;
        BoundingBox& bbox = convex_and_bounding_boxes[k].bounding_box;
        auto iy1 = bbox.min.y();
        auto iy2 = bbox.max.y();
        double height = (k == (bounding_box_count - 1))?printable_height:hc1;

        /*if (has_interlaced_objects) {
            if ((k < (bounding_box_count - 1)) && (convex_and_bounding_boxes[k].instance_height > hc2)) {
                height_polygons.emplace_back(std::make_pair(convex, hc2));
            }
        }
        else {
            if ((k < (bounding_box_count - 1)) && (convex_and_bounding_boxes[k].instance_height > hc1)) {
                height_polygons.emplace_back(std::make_pair(convex, hc1));
            }
        }*/

        for (int i = k+1; i < bounding_box_count; i++)
        {
            Polygon&     next_convex = convex_and_bounding_boxes[i].hull_polygon;
            BoundingBox& next_bbox   = convex_and_bounding_boxes[i].bounding_box;
            auto py1 = next_bbox.min.y();
            auto py2 = next_bbox.max.y();
            auto inter_min = std::max(iy1, py1); // min y of intersection
            auto inter_max = std::min(iy2, py2); // max y of intersection. length=max_y-min_y>0 means intersection exists
            if (inter_max - inter_min > 0) {
                height = hc2;
                break;
            }
        }
        if (height < convex_and_bounding_boxes[k].instance_height)
            height_polygons.emplace_back(std::make_pair(convex, height));
    }

    // sends instances 2d hulls to be rendered
    set_sequential_print_clearance_visible(true);
    set_sequential_print_clearance_render_fill(false);
    set_sequential_print_clearance_polygons(polygons, height_polygons);
}

bool GLCanvas3D::is_object_sinking(int object_idx) const
{
    for (const GLVolume* v : m_volumes.volumes) {
        if (v->object_idx() == object_idx && (v->is_sinking() || (!v->is_modifier && v->is_below_printbed())))
            return true;
    }
    return false;
}

void GLCanvas3D::apply_retina_scale(Vec2d &screen_coordinate) const
{
#if ENABLE_RETINA_GL
    double scale = static_cast<double>(m_retina_helper->get_scale_factor());
    screen_coordinate *= scale;
#endif // ENABLE_RETINA_GL
}

bool GLCanvas3D::_is_shown_on_screen() const
{
    return (m_canvas != nullptr) ? m_canvas->IsShownOnScreen() : false;
}

// Getter for the const char*[]
static bool string_getter(const bool is_undo, int idx, const char** out_text)
{
    return wxGetApp().plater()->undo_redo_string_getter(is_undo, idx, out_text);
}

// Getter for the const char*[] for the search list
static bool search_string_getter(int idx, const char** label, const char** tooltip)
{
    return wxGetApp().plater()->search_string_getter(idx, label, tooltip);
}

//BBS: GUI refactor: adjust main toolbar position
bool GLCanvas3D::_render_orient_menu(float left, float right, float bottom, float top)
{
    ImGuiWrapper* imgui = wxGetApp().imgui();

    auto canvas_w = float(get_canvas_size().get_width());
    auto canvas_h = float(get_canvas_size().get_height());
    //BBS: GUI refactor: move main toolbar to the right
    //original use center as {0.0}, and top is (canvas_h/2), bottom is (-canvas_h/2), also plus inv_camera
    //now change to left_up as {0,0}, and top is 0, bottom is canvas_h
#if BBS_TOOLBAR_ON_TOP
    const float x = (1 + left) * canvas_w / 2;
    ImGuiWrapper::push_toolbar_style(get_scale());
    imgui->set_next_window_pos(x, m_main_toolbar.get_height(), ImGuiCond_Always, 0.5f, 0.0f);
#else
    const float x = canvas_w - m_main_toolbar.get_width();
    const float y = 0.5f * canvas_h - top * float(wxGetApp().plater()->get_camera().get_zoom());
    imgui->set_next_window_pos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif

    imgui->begin(_L("Auto Orientation options"), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

    OrientSettings settings = get_orient_settings();
    OrientSettings& settings_out = get_orient_settings();

    auto& appcfg = wxGetApp().app_config;
    PrinterTechnology ptech = current_printer_technology();

    bool settings_changed = false;
    float angle_min = 45.f;
    std::string angle_key = "overhang_angle", rot_key = "enable_rotation";
    std::string key_min_area = "min_area";
    std::string postfix = "_fff";

    if (ptech == ptSLA) {
        angle_min = 45.f;
        postfix = "_sla";
    }


    angle_key += postfix;
    rot_key += postfix;

    //if (imgui->slider_float(_L("Overhang Angle"), &settings.overhang_angle, angle_min, 90.0f, "%5.2f") || angle_min > settings.overhang_angle) {
    //    settings.overhang_angle = std::max(angle_min, settings.overhang_angle);
    //    settings_out.overhang_angle = settings.overhang_angle;
    //    appcfg->set("orient", angle_key, std::to_string(settings_out.overhang_angle));
    //    settings_changed = true;
    //}

    if (imgui->checkbox(_L("Enable rotation"), settings.enable_rotation)) {
        settings_out.enable_rotation = settings.enable_rotation;
        appcfg->set("orient", rot_key, settings_out.enable_rotation ? "1" : "0");
        settings_changed = true;
    }

    if (imgui->checkbox(_L("Optimize support interface area"), settings.min_area)) {
        settings_out.min_area = settings.min_area;
        appcfg->set("orient", key_min_area, settings_out.min_area ? "1" : "0");
        settings_changed = true;
    }

    ImGui::Separator();

    if (imgui->button(_L("Orient"))) {
        wxGetApp().plater()->set_prepare_state(Job::PREPARE_STATE_DEFAULT);
        wxGetApp().plater()->orient();
    }

    ImGui::SameLine();

    if (imgui->button(_L("Reset"))) {
        settings_out = OrientSettings{};
        settings_out.overhang_angle = 60.f;
        appcfg->set("orient", angle_key, std::to_string(settings_out.overhang_angle));
        appcfg->set("orient", rot_key, settings_out.enable_rotation ? "1" : "0");
        appcfg->set("orient", key_min_area, settings_out.min_area? "1" : "0");
        settings_changed = true;
    }

    imgui->end();
    ImGuiWrapper::pop_toolbar_style();
    return settings_changed;
}

//BBS: GUI refactor: adjust main toolbar position
bool GLCanvas3D::_render_arrange_menu(float left, float right, float bottom, float top)
{
    ImGuiWrapper *imgui = wxGetApp().imgui();

    auto canvas_w = float(get_canvas_size().get_width());
    auto canvas_h = float(get_canvas_size().get_height());
    //BBS: GUI refactor: move main toolbar to the right
    //original use center as {0.0}, and top is (canvas_h/2), bottom is (-canvas_h/2), also plus inv_camera
    //now change to left_up as {0,0}, and top is 0, bottom is canvas_h
#if BBS_TOOLBAR_ON_TOP
    float left_pos = m_main_toolbar.get_item("arrange")->render_left_pos;
    const float x = (1 + left_pos) * canvas_w / 2;
    imgui->set_next_window_pos(x, m_main_toolbar.get_height(), ImGuiCond_Always, 0.0f, 0.0f);

#else
    const float x = canvas_w - m_main_toolbar.get_width();
    const float y = 0.5f * canvas_h - top * float(wxGetApp().plater()->get_camera().get_zoom());
    imgui->set_next_window_pos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif

    //BBS
    ImGuiWrapper::push_toolbar_style(get_scale());

    imgui->begin(_L("Arrange options"), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    ArrangeSettings settings = get_arrange_settings();
    ArrangeSettings &settings_out = get_arrange_settings();
    const float slider_icon_width = imgui->get_slider_icon_size().x;
    const float cursor_slider_left = imgui->calc_text_size(_L("Spacing")).x + imgui->scaled(1.5f);
    const float minimal_slider_width = imgui->scaled(4.f);
    float window_width  = minimal_slider_width + 2 * slider_icon_width;
    auto &appcfg = wxGetApp().app_config;
    PrinterTechnology ptech = current_printer_technology();

    bool settings_changed = false;
    float dist_min = 0.f;  // 0 means auto
    std::string dist_key = "min_object_distance", rot_key = "enable_rotation";
    std::string bed_shrink_x_key = "bed_shrink_x", bed_shrink_y_key = "bed_shrink_y";
    std::string multi_material_key = "allow_multi_materials_on_same_plate";
    std::string avoid_extrusion_key = "avoid_extrusion_cali_region";
    std::string align_to_y_axis_key = "align_to_y_axis";
    std::string postfix;
    //BBS:
    bool seq_print = false;

    if (ptech == ptSLA) {
        postfix      = "_sla";
    } else if (ptech == ptFFF) {
        seq_print = &settings == &m_arrange_settings_fff_seq_print;
        if (seq_print) {
            postfix      = "_fff_seq_print";
        } else {
            postfix     = "_fff";
        }
    }

    dist_key += postfix;
    rot_key  += postfix;
    bed_shrink_x_key += postfix;
    bed_shrink_y_key += postfix;

    ImGui::AlignTextToFramePadding();
    imgui->text(_L("Spacing"));
    ImGui::SameLine(1.2 * cursor_slider_left);
    ImGui::PushItemWidth(window_width - slider_icon_width);
    bool b_Spacing = imgui->bbl_slider_float_style("##Spacing", &settings.distance, dist_min, 100.0f, "%5.2f") || dist_min > settings.distance;
    ImGui::SameLine(window_width - slider_icon_width + 1.3 * cursor_slider_left);
    ImGui::PushItemWidth(1.5 * slider_icon_width);
    bool b_spacing_input = ImGui::BBLDragFloat("##spacing_input", &settings.distance, 0.05f, 0.0f, 0.0f, "%.2f");
    if (b_Spacing || b_spacing_input)
    {
        settings.distance = std::max(dist_min, settings.distance);
        settings_out.distance = settings.distance;
        appcfg->set("arrange", dist_key.c_str(), float_to_string_decimal_point(settings_out.distance));
        settings_changed = true;
    }
    imgui->text(_L("0 means auto spacing."));

    ImGui::Separator();
    if (imgui->bbl_checkbox(_L("Auto rotate for arrangement"), settings.enable_rotation)) {
        settings_out.enable_rotation = settings.enable_rotation;
        appcfg->set("arrange", rot_key.c_str(), settings_out.enable_rotation);
        settings_changed = true;
    }

    if (imgui->bbl_checkbox(_L("Allow multiple materials on same plate"), settings.allow_multi_materials_on_same_plate)) {
        settings_out.allow_multi_materials_on_same_plate = settings.allow_multi_materials_on_same_plate;
        appcfg->set("arrange", multi_material_key.c_str(), settings_out.allow_multi_materials_on_same_plate );
        settings_changed = true;
    }

    // only show this option if the printer has micro Lidar and can do first layer scan
    DynamicPrintConfig &current_config = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const bool has_lidar = wxGetApp().preset_bundle->is_bbl_vendor();
    auto                op             = current_config.option("scan_first_layer");
    if (has_lidar && op && op->getBool()) {
        if (imgui->bbl_checkbox(_L("Avoid extrusion calibration region"), settings.avoid_extrusion_cali_region)) {
            settings_out.avoid_extrusion_cali_region = settings.avoid_extrusion_cali_region;
            appcfg->set("arrange", avoid_extrusion_key.c_str(), settings_out.avoid_extrusion_cali_region ? "1" : "0");
            settings_changed = true;
        }
    } else {
        settings_out.avoid_extrusion_cali_region = false;
    }

    // Align to Y axis. Only enable this option when auto rotation not enabled
    {
        if (settings_out.enable_rotation) {  // do not allow align to Y axis if rotation is enabled
            imgui->disabled_begin(true);
            settings_out.align_to_y_axis = false;
        }

        if (imgui->bbl_checkbox(_L("Align to Y axis"), settings.align_to_y_axis)) {
            settings_out.align_to_y_axis = settings.align_to_y_axis;
            appcfg->set("arrange", align_to_y_axis_key, settings_out.align_to_y_axis ? "1" : "0");
            settings_changed = true;
        }

        if (settings_out.enable_rotation == true) { imgui->disabled_end(); }
    }

    ImGui::Separator();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(15.0f, 10.0f));
    if (imgui->button(_L("Arrange"))) {
        wxGetApp().plater()->set_prepare_state(Job::PREPARE_STATE_DEFAULT);
        wxGetApp().plater()->arrange();
    }

    ImGui::SameLine();

    if (imgui->button(_L("Reset"))) {
        settings_out = ArrangeSettings{};
        settings_out.distance = std::max(dist_min, settings_out.distance);
        //BBS: add specific arrange settings
        if (seq_print) settings_out.is_seq_print = true;

        if (auto printer_structure_opt = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionEnum<PrinterStructure>>("printer_structure")) {
            settings_out.align_to_y_axis = (printer_structure_opt->value == PrinterStructure::psI3);
        }
        else
            settings_out.align_to_y_axis = false;

        appcfg->set("arrange", dist_key, float_to_string_decimal_point(settings_out.distance));
        appcfg->set("arrange", rot_key, settings_out.enable_rotation ? "1" : "0");
        appcfg->set("arrange", align_to_y_axis_key, settings_out.align_to_y_axis ? "1" : "0");
        settings_changed = true;
    }
    ImGui::PopStyleVar(1);
    imgui->end();

    //BBS
    ImGuiWrapper::pop_toolbar_style();

    return settings_changed;
}

static const float cameraProjection[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};

void GLCanvas3D::_render_3d_navigator()
{
    if (!wxGetApp().show_3d_navigator()) {
        m_canvas_toolbar_pos[0] = 0;
        return;
    }

    ImGuizmo::BeginFrame();

    auto& style                                = ImGuizmo::GetStyle();
    style.Colors[ImGuizmo::COLOR::DIRECTION_X] = ImGuiWrapper::to_ImVec4(ColorRGBA::Y());
    style.Colors[ImGuizmo::COLOR::DIRECTION_Y] = ImGuiWrapper::to_ImVec4(ColorRGBA::Z());
    style.Colors[ImGuizmo::COLOR::DIRECTION_Z] = ImGuiWrapper::to_ImVec4(ColorRGBA::X());
    style.Colors[ImGuizmo::COLOR::TEXT] = m_is_dark ? ImVec4(224 / 255.f, 224 / 255.f, 224 / 255.f, 1.f) : ImVec4(.2f, .2f, .2f, 1.0f);
    style.Colors[ImGuizmo::COLOR::FACE]        = m_is_dark ? ImVec4(0.23f, 0.23f, 0.23f, 1.f) : ImVec4(0.77f, 0.77f, 0.77f, 1);
    strcpy(style.AxisLabels[ImGuizmo::Axis::Axis_X], "Y"); // ORCA use uppercase to match text on tranform widgets
    strcpy(style.AxisLabels[ImGuizmo::Axis::Axis_Y], "Z"); // ORCA use uppercase to match text on tranform widgets
    strcpy(style.AxisLabels[ImGuizmo::Axis::Axis_Z], "X"); // ORCA use uppercase to match text on tranform widgets
    strcpy(style.FaceLabels[ImGuizmo::FACES::FACE_FRONT], _utf8("Front").c_str());
    strcpy(style.FaceLabels[ImGuizmo::FACES::FACE_BACK], _utf8("Back").c_str());
    strcpy(style.FaceLabels[ImGuizmo::FACES::FACE_TOP], _utf8("Top").c_str());
    strcpy(style.FaceLabels[ImGuizmo::FACES::FACE_BOTTOM], _utf8("Bottom").c_str());
    strcpy(style.FaceLabels[ImGuizmo::FACES::FACE_LEFT], _CTX_utf8(L_CONTEXT("Left", "Camera"), "Camera").c_str());
    strcpy(style.FaceLabels[ImGuizmo::FACES::FACE_RIGHT], _CTX_utf8(L_CONTEXT("Right", "Camera"), "Camera").c_str());

    float sc = get_scale();
#ifdef WIN32
    const int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
    sc *= (float) dpi / (float) DPI_DEFAULT;
#endif // WIN32

    const ImGuiIO& io              = ImGui::GetIO();
    const float viewManipulateLeft = 0;
    const float viewManipulateTop  = io.DisplaySize.y;
    const float camDistance        = 8.f;

    Camera&     camera           = wxGetApp().plater()->get_camera();
    Transform3d m                = Transform3d::Identity();
    m.matrix().block(0, 0, 3, 3) = camera.get_view_rotation().toRotationMatrix();
    // Rotate along X and Z axis for 90 degrees to have Y-up
    const auto coord_mapping_transform = Geometry::rotation_transform(Vec3d(0.5 * PI, 0, 0.5 * PI));
    m = m * coord_mapping_transform;
    float cameraView[16];
    for (unsigned int c = 0; c < 4; ++c) {
        for (unsigned int r = 0; r < 4; ++r) {
            cameraView[c * 4 + r] = m(r, c);
        }
    }

    const float size  = 128 * sc;
    m_canvas_toolbar_pos[0] = size;
    const auto result = ImGuizmo::ViewManipulate(cameraView, cameraProjection, ImGuizmo::OPERATION::ROTATE, ImGuizmo::MODE::WORLD, nullptr,
                                                 camDistance, ImVec2(viewManipulateLeft, viewManipulateTop - size), ImVec2(size, size),
                                                 0x00101010);

    if (result.changed) {
        for (unsigned int c = 0; c < 4; ++c) {
            for (unsigned int r = 0; r < 4; ++r) {
                m(r, c) = cameraView[c * 4 + r];
            }
        }
        // Rotate back
        m = m * (coord_mapping_transform.inverse());
        camera.set_rotation(m);

        if (result.dragging) {
            // Switch back to perspective view when normal dragging
            camera.auto_type(Camera::EType::Perspective);
        } else if (result.clicked_box >= 0) {
            switch (result.clicked_box) {
            case 4: // back
            case 10: // top
            case 12: // right
            case 14: // left
            case 16: // bottom
            case 22: // front
                // Automatically switch to orthographic view when click the center face of the navigator
                camera.auto_type(Camera::EType::Ortho); break;
            default:
                // Otherwise switch back to perspective view
                camera.auto_type(Camera::EType::Perspective); break;
            }
        }

        request_extra_frame();
    }
}

#define ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT 0
#if ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT
static void debug_output_thumbnail(const ThumbnailData& thumbnail_data)
{
    // debug export of generated image
    wxImage image(thumbnail_data.width, thumbnail_data.height);
    image.InitAlpha();

    for (unsigned int r = 0; r < thumbnail_data.height; ++r)
    {
        unsigned int rr = (thumbnail_data.height - 1 - r) * thumbnail_data.width;
        for (unsigned int c = 0; c < thumbnail_data.width; ++c)
        {
            unsigned char* px = (unsigned char*)thumbnail_data.pixels.data() + 4 * (rr + c);
            image.SetRGB((int)c, (int)r, px[0], px[1], px[2]);
            image.SetAlpha((int)c, (int)r, px[3]);
        }
    }

    image.SaveFile("C:/bambu/test/test.png", wxBITMAP_TYPE_PNG);
}
#endif // ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT

void GLCanvas3D::render_thumbnail_internal(ThumbnailData& thumbnail_data, const ThumbnailsParams& thumbnail_params,
    PartPlateList& partplate_list, ModelObjectPtrs& model_objects, const GLVolumeCollection& volumes, std::vector<ColorRGBA>& extruder_colors,
                                           GLShaderProgram *                  shader,
                                           Camera::EType                      camera_type,
                                           Camera::ViewAngleType              camera_view_angle_type,
                                           bool                               for_picking,
                                           bool                               ban_light)
{
    //BBS modify visible calc function
    static ColorRGBA                  curr_color;
    static const std::array<float, 4> orange = {0.923f, 0.504f, 0.264f, 1.0f};
    static const std::array<float, 4> gray   = {0.64f, 0.64f, 0.64f, 1.0f};
    GLVolumePtrs                      visible_volumes;
    BoundingBoxf3                     plate_build_volume;
    if (thumbnail_params.use_plate_box) {
        int           plate_idx          = thumbnail_params.plate_id;
        PartPlate *   plate              = partplate_list.get_plate(plate_idx);
        plate_build_volume = plate->get_build_volume();
        plate_build_volume.min(0) -= Slic3r::BuildVolume::SceneEpsilon;
        plate_build_volume.min(1) -= Slic3r::BuildVolume::SceneEpsilon;
        plate_build_volume.min(2) -= Slic3r::BuildVolume::SceneEpsilon;
        plate_build_volume.max(0) += Slic3r::BuildVolume::SceneEpsilon;
        plate_build_volume.max(1) += Slic3r::BuildVolume::SceneEpsilon;
        plate_build_volume.max(2) += Slic3r::BuildVolume::SceneEpsilon;
        /*if (m_config != nullptr) {
            double h = m_config->opt_float("printable_height");
            plate_build_volume.min(2) = std::min(plate_build_volume.min(2), -h);
            plate_build_volume.max(2) = std::max(plate_build_volume.max(2), h);
        }*/

        auto is_visible = [plate_idx, plate_build_volume](const GLVolume &v) {
            bool ret = v.printable;
            if (plate_idx >= 0) {
                bool          contained          = false;
                BoundingBoxf3 plate_bbox         = plate_build_volume;
                plate_bbox.min(2)                = -1e10;
                const BoundingBoxf3 &volume_bbox = v.transformed_convex_hull_bounding_box();
                if (plate_bbox.contains(volume_bbox) && (volume_bbox.max(2) > 0)) { contained = true; }
                ret &= contained;
            } else {
                ret &= (!v.shader_outside_printer_detection_enabled || !v.is_outside);
            }
            return ret;
        };

        for (GLVolume *vol : volumes.volumes) {
            if (!vol->is_modifier && !vol->is_wipe_tower && (!thumbnail_params.parts_only || vol->composite_id.volume_id >= 0)) {
                if (is_visible(*vol)) { visible_volumes.emplace_back(vol); }
            }
        }
        BOOST_LOG_TRIVIAL(info) << boost::format("render_thumbnail: plate_idx %1% volumes size %2%, shader %3%, use_top_view=%4%, for_picking=%5%") % plate_idx %
                                       visible_volumes.size() % shader % (int)camera_view_angle_type % for_picking;
    } else {
        visible_volumes = volumes.volumes;
    }
    //BoundingBoxf3 volumes_box = plate_build_volume;
    BoundingBoxf3 volumes_box;
    volumes_box.min.z() = 0;
    volumes_box.max.z() = 0;
    if (!visible_volumes.empty()) {
        for (const GLVolume* vol : visible_volumes) {
            volumes_box.merge(vol->transformed_bounding_box());
        }
    }
    volumes_box.min.z() = -Slic3r::BuildVolume::SceneEpsilon;
    double width = volumes_box.max.x() - volumes_box.min.x();
    double depth = volumes_box.max.y() - volumes_box.min.y();
    double height = volumes_box.max.z() - volumes_box.min.z();
    volumes_box.max.x() = volumes_box.max.x() + width * 0.01f;
    volumes_box.min.x() = volumes_box.min.x() - width * 0.01f;
    volumes_box.max.y() = volumes_box.max.y() + depth * 0.01f;
    volumes_box.min.y() = volumes_box.min.y() - depth * 0.01f;
    volumes_box.max.z() = volumes_box.max.z() + height * 0.02f;
    volumes_box.min.z() = volumes_box.min.z() - height * 0.02f;

    Camera camera;
    camera.set_type(camera_type);
    //BBS modify scene box to plate scene bounding box
    //plate_build_volume.min(2) = - plate_build_volume.max(2);
    if (thumbnail_params.use_plate_box) {
        camera.set_scene_box(plate_build_volume);
    }

    camera.set_viewport(0, 0, thumbnail_data.width, thumbnail_data.height);
    camera.apply_viewport();

    //BoundingBoxf3 plate_box = plate->get_bounding_box(false);
    //plate_box.min.z() = 0.0;
    //plate_box.max.z() = 0.0;

    if (camera_view_angle_type == Camera::ViewAngleType::Top_Plate) {
        float center_x = (plate_build_volume.max(0) + plate_build_volume.min(0))/2;
        float center_y = (plate_build_volume.max(1) + plate_build_volume.min(1))/2;
        float distance_z = plate_build_volume.max(2) - plate_build_volume.min(2);
        Vec3d center(center_x, center_y, 0.f);
        double zoom_ratio, scale_x, scale_y;

        scale_x = ((double)thumbnail_data.width)/(plate_build_volume.max(0) - plate_build_volume.min(0));
        scale_y = ((double)thumbnail_data.height)/(plate_build_volume.max(1) - plate_build_volume.min(1));
        zoom_ratio = (scale_x <= scale_y)?scale_x:scale_y;
        camera.look_at(center + distance_z * Vec3d::UnitZ(), center, Vec3d::UnitY());
        camera.set_zoom(zoom_ratio);
        //camera.select_view("top");
    }
    else {
        camera.select_view(camera_view_angle_type);
        camera.zoom_to_box(volumes_box);
    }

    const Transform3d &view_matrix = camera.get_view_matrix();
    if (thumbnail_params.use_plate_box) {
        camera.apply_projection(plate_build_volume);
    }
    else {
        camera.apply_projection(volumes_box);
    }

    //GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
    if (!for_picking && (shader == nullptr)) {
        BOOST_LOG_TRIVIAL(info) <<  boost::format("render_thumbnail with no picking: shader is null, return directly");
        return;
    }

    //if (thumbnail_params.transparent_background)
    glsafe(::glClearColor(0.f, 0.f, 0.f, 0.f));


    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));
    if (ban_light) {
        glsafe(::glDisable(GL_BLEND));
    }
    const Transform3d &projection_matrix = camera.get_projection_matrix();

    if (for_picking) {
        //if (OpenGLManager::can_multisample())
              // This flag is often ignored by NVIDIA drivers if rendering into a screen buffer.
        //    glsafe(::glDisable(GL_MULTISAMPLE));
        shader->start_using();

        glsafe(::glDisable(GL_BLEND));

        static const GLfloat INV_255 = 1.0f / 255.0f;

        // do not cull backfaces to show broken geometry, if any
        glsafe(::glDisable(GL_CULL_FACE));

        for (GLVolume* vol : visible_volumes) {
            // Object picking mode. Render the object with a color encoding the object index.
            // we reserve color = (0,0,0) for occluders (as the printbed)
            // so we shift volumes' id by 1 to get the proper color
            //BBS: remove the bed picking logic
            unsigned int id = vol->model_object_ID;
            //unsigned int id = 1 + volume.second.first;
            unsigned int r = (id & (0x000000FF << 0)) >> 0;
            unsigned int g = (id & (0x000000FF << 8)) >> 8;
            unsigned int b = (id & (0x000000FF << 16)) >> 16;
            unsigned int a = 0xFF;
            vol->model.set_color({(GLfloat)r * INV_255, (GLfloat)g * INV_255, (GLfloat)b * INV_255, (GLfloat)a * INV_255});
            /*curr_color[0] = (GLfloat)r * INV_255;
            curr_color[1] = (GLfloat)g * INV_255;
            curr_color[2] = (GLfloat)b * INV_255;
            curr_color[3] = (GLfloat)a * INV_255;
            shader->set_uniform("uniform_color", curr_color);*/

            const bool is_active = vol->is_active;
            vol->is_active = true;
            vol->picking   = true;

            const Transform3d model_matrix = vol->world_matrix();
            shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
            shader->set_uniform("projection_matrix", projection_matrix);
            vol->simple_render(shader, model_objects, extruder_colors);
            vol->is_active = is_active;
            vol->picking   = false;
        }

        //glsafe(::glDisableClientState(GL_NORMAL_ARRAY));
        //glsafe(::glDisableClientState(GL_VERTEX_ARRAY));

        glsafe(::glEnable(GL_CULL_FACE));

        //if (OpenGLManager::can_multisample())
        //    glsafe(::glEnable(GL_MULTISAMPLE));
    }
    else {
        shader->start_using();
        shader->set_uniform("emission_factor", 0.1f);
        shader->set_uniform("ban_light", ban_light);
        for (GLVolume* vol : visible_volumes) {
            //BBS set render color for thumbnails
            curr_color = vol->color;

            ColorRGBA new_color = adjust_color_for_rendering(curr_color);
            if (ban_light) {
                new_color[3] =(255 - (vol->extruder_id -1))/255.0f;
            }
            vol->model.set_color(new_color);
            shader->set_uniform("volume_world_matrix", vol->world_matrix());
            //BBS set all volume to orange
            //shader->set_uniform("uniform_color", orange);
            /*if (plate_idx > 0) {
                shader->set_uniform("uniform_color", orange);
            }
            else {
                shader->set_uniform("uniform_color", (vol->printable && !vol->is_outside) ? orange : gray);
            }*/
            // the volume may have been deactivated by an active gizmo
            const bool is_active = vol->is_active;
            vol->is_active = true;
            const Transform3d model_matrix = vol->world_matrix();
            shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
            shader->set_uniform("projection_matrix", projection_matrix);
            const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
            shader->set_uniform("view_normal_matrix", view_normal_matrix);
            vol->simple_render(shader,  model_objects, extruder_colors, ban_light);
            vol->is_active = is_active;
        }
        shader->stop_using();
    }

    glsafe(::glDisable(GL_DEPTH_TEST));

    //don't render plate in thumbnail
    //plate->render( false, true, true);

    // restore background color
    //if (thumbnail_params.transparent_background)
    //    glsafe(::glClearColor(1.0f, 1.0f, 1.0f, 1.0f));
    BOOST_LOG_TRIVIAL(info) << boost::format("render_thumbnail: finished");
}

void GLCanvas3D::render_thumbnail_framebuffer(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params,
    PartPlateList& partplate_list, ModelObjectPtrs& model_objects, const GLVolumeCollection& volumes, std::vector<ColorRGBA>& extruder_colors,
                                              GLShaderProgram *                  shader,
                                              Camera::EType                      camera_type,
                                              Camera::ViewAngleType              camera_view_angle_type,
                                              bool                               for_picking,
                                              bool                               ban_light)
{
    thumbnail_data.set(w, h);
    if (!thumbnail_data.is_valid())
        return;

    bool multisample = OpenGLManager::can_multisample();
    if (for_picking)
        multisample = false;
    //if (!multisample)
    //    glsafe(::glEnable(GL_MULTISAMPLE));

    GLint max_samples;
    glsafe(::glGetIntegerv(GL_MAX_SAMPLES, &max_samples));
    GLsizei num_samples = max_samples / 2;

    GLuint render_fbo;
    glsafe(::glGenFramebuffers(1, &render_fbo));
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, render_fbo));

    BOOST_LOG_TRIVIAL(info) << boost::format("render_thumbnail prepare: w %1%, h %2%, max_samples  %3%, render_fbo %4%") %w %h %max_samples % render_fbo;
    GLuint render_tex = 0;
    GLuint render_tex_buffer = 0;
    if (multisample) {
        // use renderbuffer instead of texture to avoid the need to use glTexImage2DMultisample which is available only since OpenGL 3.2
        glsafe(::glGenRenderbuffers(1, &render_tex_buffer));
        glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, render_tex_buffer));
        glsafe(::glRenderbufferStorageMultisample(GL_RENDERBUFFER, num_samples, GL_RGBA8, w, h));
        glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, render_tex_buffer));
    }
    else {
        glsafe(::glGenTextures(1, &render_tex));
        glsafe(::glBindTexture(GL_TEXTURE_2D, render_tex));
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, render_tex, 0));
    }

    GLuint render_depth;
    glsafe(::glGenRenderbuffers(1, &render_depth));
    glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, render_depth));
    if (multisample)
        glsafe(::glRenderbufferStorageMultisample(GL_RENDERBUFFER, num_samples, GL_DEPTH_COMPONENT24, w, h));
    else
        glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, w, h));

    glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, render_depth));

    GLenum drawBufs[] = { GL_COLOR_ATTACHMENT0 };
    glsafe(::glDrawBuffers(1, drawBufs));

    if (::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        render_thumbnail_internal(thumbnail_data, thumbnail_params, partplate_list, model_objects, volumes, extruder_colors, shader, camera_type, camera_view_angle_type,
                                  for_picking, ban_light);

        if (multisample) {
            GLuint resolve_fbo;
            glsafe(::glGenFramebuffers(1, &resolve_fbo));
            glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo));

            GLuint resolve_tex;
            glsafe(::glGenTextures(1, &resolve_tex));
            glsafe(::glBindTexture(GL_TEXTURE_2D, resolve_tex));
            glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
            glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolve_tex, 0));

            glsafe(::glDrawBuffers(1, drawBufs));

            if (::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
                glsafe(::glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fbo));
                glsafe(::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo));
                glsafe(::glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR));

                glsafe(::glBindFramebuffer(GL_READ_FRAMEBUFFER, resolve_fbo));
                glsafe(::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void*)thumbnail_data.pixels.data()));
            }

            glsafe(::glDeleteTextures(1, &resolve_tex));
            glsafe(::glDeleteFramebuffers(1, &resolve_fbo));
        }
        else
            glsafe(::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void*)thumbnail_data.pixels.data()));

#if ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT
        debug_output_thumbnail(thumbnail_data);
#endif // ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT
    }
    else {
        BOOST_LOG_TRIVIAL(info) << boost::format("render_thumbnail prepare: GL_FRAMEBUFFER not complete");
    }

    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, 0));
    glsafe(::glDeleteRenderbuffers(1, &render_depth));
    if (render_tex_buffer != 0)
        glsafe(::glDeleteRenderbuffers(1, &render_tex_buffer));
    if (render_tex != 0)
        glsafe(::glDeleteTextures(1, &render_tex));
    glsafe(::glDeleteFramebuffers(1, &render_fbo));

    //if (!multisample)
    //    glsafe(::glDisable(GL_MULTISAMPLE));
    BOOST_LOG_TRIVIAL(info) << boost::format("render_thumbnail prepare: finished");
}

void GLCanvas3D::render_thumbnail_framebuffer_ext(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params,
    PartPlateList& partplate_list, ModelObjectPtrs& model_objects, const GLVolumeCollection& volumes, std::vector<ColorRGBA>& extruder_colors,
                                                  GLShaderProgram *                  shader,
                                                  Camera::EType                      camera_type,
                                                  Camera::ViewAngleType              camera_view_angle_type,
                                                  bool                               for_picking,
                                                  bool                               ban_light)
{
    thumbnail_data.set(w, h);
    if (!thumbnail_data.is_valid())
        return;

    bool multisample = OpenGLManager::can_multisample();
    if (for_picking)
        multisample = false;
    //if (!multisample)
    //    glsafe(::glEnable(GL_MULTISAMPLE));

    GLint max_samples;
    glsafe(::glGetIntegerv(GL_MAX_SAMPLES_EXT, &max_samples));
    GLsizei num_samples = max_samples / 2;

    GLuint render_fbo;
    glsafe(::glGenFramebuffersEXT(1, &render_fbo));
    glsafe(::glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, render_fbo));

    GLuint render_tex = 0;
    GLuint render_tex_buffer = 0;
    if (multisample) {
        // use renderbuffer instead of texture to avoid the need to use glTexImage2DMultisample which is available only since OpenGL 3.2
        glsafe(::glGenRenderbuffersEXT(1, &render_tex_buffer));
        glsafe(::glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, render_tex_buffer));
        glsafe(::glRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER_EXT, num_samples, GL_RGBA8, w, h));
        glsafe(::glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, render_tex_buffer));
    }
    else {
        glsafe(::glGenTextures(1, &render_tex));
        glsafe(::glBindTexture(GL_TEXTURE_2D, render_tex));
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, render_tex, 0));
    }

    GLuint render_depth;
    glsafe(::glGenRenderbuffersEXT(1, &render_depth));
    glsafe(::glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, render_depth));
    if (multisample)
        glsafe(::glRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER_EXT, num_samples, GL_DEPTH_COMPONENT24, w, h));
    else
        glsafe(::glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT, w, h));

    glsafe(::glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, render_depth));

    GLenum drawBufs[] = { GL_COLOR_ATTACHMENT0 };
    glsafe(::glDrawBuffers(1, drawBufs));

    if (::glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT) {
        render_thumbnail_internal(thumbnail_data, thumbnail_params, partplate_list, model_objects, volumes, extruder_colors, shader, camera_type, camera_view_angle_type,
                                  for_picking,
                                  ban_light);

        if (multisample) {
            GLuint resolve_fbo;
            glsafe(::glGenFramebuffersEXT(1, &resolve_fbo));
            glsafe(::glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, resolve_fbo));

            GLuint resolve_tex;
            glsafe(::glGenTextures(1, &resolve_tex));
            glsafe(::glBindTexture(GL_TEXTURE_2D, resolve_tex));
            glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
            glsafe(::glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, resolve_tex, 0));

            glsafe(::glDrawBuffers(1, drawBufs));

            if (::glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT) {
                glsafe(::glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, render_fbo));
                glsafe(::glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, resolve_fbo));
                glsafe(::glBlitFramebufferEXT(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR));

                glsafe(::glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, resolve_fbo));
                glsafe(::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void*)thumbnail_data.pixels.data()));
            }

            glsafe(::glDeleteTextures(1, &resolve_tex));
            glsafe(::glDeleteFramebuffersEXT(1, &resolve_fbo));
        }
        else
            glsafe(::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void*)thumbnail_data.pixels.data()));

#if ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT
        debug_output_thumbnail(thumbnail_data);
#endif // ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT
    }

    glsafe(::glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0));
    glsafe(::glDeleteRenderbuffersEXT(1, &render_depth));
    if (render_tex_buffer != 0)
        glsafe(::glDeleteRenderbuffersEXT(1, &render_tex_buffer));
    if (render_tex != 0)
        glsafe(::glDeleteTextures(1, &render_tex));
    glsafe(::glDeleteFramebuffersEXT(1, &render_fbo));

    //if (!multisample)
    //    glsafe(::glDisable(GL_MULTISAMPLE));
}

void GLCanvas3D::render_thumbnail_legacy(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params, PartPlateList &partplate_list, ModelObjectPtrs& model_objects, const GLVolumeCollection& volumes, std::vector<ColorRGBA>& extruder_colors, GLShaderProgram* shader, Camera::EType camera_type,
                                         Camera::ViewAngleType              camera_view_angle_type,
                                         bool                               for_picking,
                                         bool                               ban_light)
{
    // check that thumbnail size does not exceed the default framebuffer size
    /*const Size& cnv_size = get_canvas_size();
    unsigned int cnv_w = (unsigned int)cnv_size.get_width();
    unsigned int cnv_h = (unsigned int)cnv_size.get_height();
    if (w > cnv_w || h > cnv_h) {
        float ratio = std::min((float)cnv_w / (float)w, (float)cnv_h / (float)h);
        w = (unsigned int)(ratio * (float)w);
        h = (unsigned int)(ratio * (float)h);
    }*/

    thumbnail_data.set(w, h);
    if (!thumbnail_data.is_valid())
        return;

    render_thumbnail_internal(thumbnail_data, thumbnail_params, partplate_list, model_objects, volumes, extruder_colors, shader, camera_type, camera_view_angle_type, for_picking,
                              ban_light);

    glsafe(::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void*)thumbnail_data.pixels.data()));
#if ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT
    debug_output_thumbnail(thumbnail_data);
#endif // ENABLE_THUMBNAIL_GENERATOR_DEBUG_OUTPUT

    // restore the default framebuffer size to avoid flickering on the 3D scene
    //wxGetApp().plater()->get_camera().apply_viewport();
}

//BBS: GUI refractor

void GLCanvas3D::_update_slice_error_status()
{
    _set_warning_notification_if_needed(EWarning::ToolHeightOutside);
    _set_warning_notification_if_needed(EWarning::ToolpathOutside);
    _set_warning_notification_if_needed(EWarning::GCodeConflict);
    _set_warning_notification_if_needed(EWarning::MultiExtruderPrintableError);
    _set_warning_notification_if_needed(EWarning::MultiExtruderHeightOutside);
    _set_warning_notification_if_needed(EWarning::FilamentUnPrintableOnFirstLayer);
}

void GLCanvas3D::_switch_toolbars_icon_filename()
{
    BackgroundTexture::Metadata background_data;
    background_data.filename = m_is_dark ? "toolbar_background_dark.png" : "toolbar_background.png";
    background_data.left = 16;
    background_data.top = 16;
    background_data.right = 16;
    background_data.bottom = 16;
    m_main_toolbar.init(background_data);
    m_assemble_view_toolbar.init(background_data);
    m_separator_toolbar.init(background_data);
    wxGetApp().plater()->get_collapse_toolbar().init(background_data);

    // main toolbar
    {
        GLToolbarItem* item;
        item = m_main_toolbar.get_item("add");
        item->set_icon_filename(m_is_dark ? "toolbar_open_dark.svg" : "toolbar_open.svg");

        item = m_main_toolbar.get_item("addplate");
        item->set_icon_filename(m_is_dark ? "toolbar_add_plate_dark.svg" : "toolbar_add_plate.svg");

        item = m_main_toolbar.get_item("orient");
        item->set_icon_filename(m_is_dark ? "toolbar_orient_dark.svg" : "toolbar_orient.svg");

        item = m_main_toolbar.get_item("addplate");
        item->set_icon_filename(m_is_dark ? "toolbar_add_plate_dark.svg" : "toolbar_add_plate.svg");

        item = m_main_toolbar.get_item("arrange");
        item->set_icon_filename(m_is_dark ? "toolbar_arrange_dark.svg" : "toolbar_arrange.svg");

        item = m_main_toolbar.get_item("more");
        item->set_icon_filename(m_is_dark ? "instance_add_dark.svg" : "instance_add.svg");

        item = m_main_toolbar.get_item("fewer");
        item->set_icon_filename(m_is_dark ? "instance_remove_dark.svg" : "instance_remove.svg");

        item = m_main_toolbar.get_item("splitobjects");
        item->set_icon_filename(m_is_dark ? "split_objects_dark.svg" : "split_objects.svg");

        item = m_main_toolbar.get_item("splitvolumes");
        item->set_icon_filename(m_is_dark ? "split_parts_dark.svg" : "split_parts.svg");

        item = m_main_toolbar.get_item("layersediting");
        item->set_icon_filename(m_is_dark ? "toolbar_variable_layer_height_dark.svg" : "toolbar_variable_layer_height.svg");
    }

    // assemble view toolbar
    {
        GLToolbarItem* item;
        item = m_assemble_view_toolbar.get_item("assembly_view");
        item->set_icon_filename(m_is_dark ? "toolbar_assemble_dark.svg" : "toolbar_assemble.svg");
    }
}
bool GLCanvas3D::_init_toolbars()
{
    if (!_init_main_toolbar())
        return false;

    //BBS: GUI refractor
    if (!_init_assemble_view_toolbar())
        return false;

    if (!_init_return_toolbar())
        return false;

    if (!_init_separator_toolbar())
        return false;

    if (!_init_select_plate_toolbar())
        return false;

#if 0
    if (!_init_view_toolbar())
        return false;
#endif

    if (!_init_collapse_toolbar())
        return false;

    return true;
}

//BBS: GUI refactor: GLToolbar
bool GLCanvas3D::_init_main_toolbar()
{
    if (!m_main_toolbar.is_enabled())
        return true;

    BackgroundTexture::Metadata background_data;
    background_data.filename = m_is_dark ? "toolbar_background_dark.png" : "toolbar_background.png";
    background_data.left = 16;
    background_data.top = 16;
    background_data.right = 16;
    background_data.bottom = 16;

    if (!m_main_toolbar.init(background_data))
    {
        // unable to init the toolbar texture, disable it
        m_main_toolbar.set_enabled(false);
        return true;
    }
    // init arrow
    if (!m_main_toolbar.init_arrow("toolbar_arrow.svg"))
        BOOST_LOG_TRIVIAL(error) << "Main toolbar failed to load arrow texture.";

    // m_gizmos is created at constructor, thus we can init arrow here.
    if (!m_gizmos.init_arrow("toolbar_arrow.svg"))
        BOOST_LOG_TRIVIAL(error) << "Gizmos manager failed to load arrow texture.";

    m_main_toolbar.set_layout_type(GLToolbar::Layout::Horizontal);
    //BBS: main toolbar is at the top and left, we don't need the rounded-corner effect at the right side and the top side
    m_main_toolbar.set_horizontal_orientation(GLToolbar::Layout::HO_Right);
    m_main_toolbar.set_vertical_orientation(GLToolbar::Layout::VO_Top);
    m_main_toolbar.set_border(4.0f);
    m_main_toolbar.set_separator_size(4);
    m_main_toolbar.set_gap_size(4);

    m_main_toolbar.del_all_item();

    GLToolbarItem::Data item;

    item.name = "add";
    item.icon_filename = m_is_dark ? "toolbar_open_dark.svg" : "toolbar_open.svg";
    item.tooltip = _utf8(L("Add")) + " [" + GUI::shortkey_ctrl_prefix() + "I]";
    item.sprite_id = 0;
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_ADD)); };
    item.enabling_callback = []()->bool {return wxGetApp().plater()->can_add_model(); };
    if (!m_main_toolbar.add_item(item))
        return false;

    item.name = "addplate";
    item.icon_filename = m_is_dark ? "toolbar_add_plate_dark.svg" : "toolbar_add_plate.svg";
    item.tooltip = _utf8(L("Add plate"));
    item.sprite_id++;
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_ADD_PLATE)); };
    item.enabling_callback = []()->bool {return wxGetApp().plater()->can_add_plate(); };
    if (!m_main_toolbar.add_item(item))
        return false;

    item.name = "orient";
    item.icon_filename = m_is_dark ? "toolbar_orient_dark.svg" : "toolbar_orient.svg";
    item.tooltip = _utf8(L("Auto orient all/selected objects")) + " [Q]\n" + _utf8(L("Auto orient all objects on current plate")) + " [" + _utf8(L("Shift+")) + "Q]";
    item.sprite_id++;
    item.left.render_callback = nullptr;
    item.enabling_callback = []()->bool { return wxGetApp().plater()->can_arrange(); };
    item.left.toggable = false;  // allow right mouse click
    //BBS: GUI refactor: adjust the main toolbar position
    item.left.action_callback = [this]() {
        if (m_canvas != nullptr)
        {
            wxGetApp().plater()->set_prepare_state(Job::PREPARE_STATE_DEFAULT);
            wxGetApp().plater()->orient();
            //BBS do not show orient menu
            //_render_orient_menu(left, right, bottom, top);
            NetworkAgent* agent = GUI::wxGetApp().getAgent();
            if (agent) agent->track_update_property("auto_orient", std::to_string(++auto_orient_count));
        }
    };
    if (!m_main_toolbar.add_item(item))
        return false;

    item.name = "arrange";
    item.icon_filename = m_is_dark ? "toolbar_arrange_dark.svg" : "toolbar_arrange.svg";
    item.tooltip = _utf8(L("Arrange all objects")) + " [A]\n" + _utf8(L("Arrange objects on selected plates")) + " [" + _utf8(L("Shift+")) + "A]";
    item.sprite_id++;
    item.left.action_callback = []() {};
    item.enabling_callback = []()->bool { return wxGetApp().plater()->can_arrange(); };
    item.left.toggable = true;
    //BBS: GUI refactor: adjust the main toolbar position
    item.left.render_callback = [this](float left, float right, float bottom, float top) {
        if (m_canvas != nullptr)
        {
            _render_arrange_menu(left, right, bottom, top);
            //_render_arrange_menu(0.5f * (left + right));
        }
    };
    if (!m_main_toolbar.add_item(item))
        return false;

    item.right.toggable = false;
    item.right.render_callback = GLToolbarItem::Default_Render_Callback;

    if (!m_main_toolbar.add_separator())
        return false;

    item.name = "more";
    item.icon_filename = m_is_dark ? "instance_add_dark.svg" : "instance_add.svg";
    item.tooltip = _utf8(L("Add instance")) + " [+]";
    item.sprite_id++;
    item.left.render_callback = nullptr;
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_MORE)); };
    item.visibility_callback = GLToolbarItem::Default_Visibility_Callback;
    item.left.toggable = false;
    item.enabling_callback = []()->bool { return wxGetApp().plater()->can_increase_instances(); };
    if (!m_main_toolbar.add_item(item))
        return false;

    item.name = "fewer";
    item.icon_filename = m_is_dark ? "instance_remove_dark.svg" : "instance_remove.svg";
    item.tooltip = _utf8(L("Remove instance")) + " [-]";
    item.sprite_id++;
    item.left.render_callback = nullptr;
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_FEWER)); };
    item.visibility_callback = GLToolbarItem::Default_Visibility_Callback;
    item.left.toggable = false;
    item.enabling_callback = []()->bool { return wxGetApp().plater()->can_decrease_instances(); };
    if (!m_main_toolbar.add_item(item))
        return false;

    item.name = "splitobjects";
    item.icon_filename = m_is_dark ? "split_objects_dark.svg" : "split_objects.svg";
    item.tooltip = _utf8(L("Split to objects"));
    item.sprite_id++;
    item.left.render_callback = nullptr;
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_SPLIT_OBJECTS)); };
    item.visibility_callback = GLToolbarItem::Default_Visibility_Callback;
    item.left.toggable = false;
    item.enabling_callback = []()->bool { return wxGetApp().plater()->can_split_to_objects(); };
    if (!m_main_toolbar.add_item(item))
        return false;

    item.name = "splitvolumes";
    item.icon_filename = m_is_dark ? "split_parts_dark.svg" : "split_parts.svg";
    item.tooltip = _utf8(L("Split to parts"));
    item.sprite_id++;
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_SPLIT_VOLUMES)); };
    item.visibility_callback = GLToolbarItem::Default_Visibility_Callback;
    item.enabling_callback = []()->bool { return wxGetApp().plater()->can_split_to_volumes(); };
    if (!m_main_toolbar.add_item(item))
        return false;

    item.name = "layersediting";
    item.icon_filename = m_is_dark ? "toolbar_variable_layer_height_dark.svg" : "toolbar_variable_layer_height.svg";
    item.tooltip = _utf8(L("Variable layer height"));
    item.sprite_id++;
    item.left.toggable = true; // ORCA Closes popup if other toolbar icon clicked and it allows closing popup when clicked its button
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLTOOLBAR_LAYERSEDITING)); };
    item.visibility_callback = [this]()->bool {
        bool res = current_printer_technology() == ptFFF;
        // turns off if changing printer technology
        if (!res && m_main_toolbar.is_item_visible("layersediting") && m_main_toolbar.is_item_pressed("layersediting"))
            force_main_toolbar_left_action(get_main_toolbar_item_id("layersediting"));

        return res;
    };
    item.enabling_callback = []()->bool { return wxGetApp().plater()->can_layers_editing(); };
    if (!m_main_toolbar.add_item(item))
        return false;

    return true;
}

//BBS: GUI refactor: GLToolbar
bool GLCanvas3D::_init_select_plate_toolbar()
{
    std::string path = resources_dir() + "/images/";
    IMToolbarItem* item = new IMToolbarItem();
    // ORCA add dark mode support and load images with 2x resolution to prevent blurry image on hi-dpi screens
    std::string    ext  = m_is_dark ? "_dark.svg" : ".svg";
    bool result      = item->image_stats.load_from_svg_file(   path + "im_all_plates_stats"   + ext, false, false, false, 200);
    result = result && item->image_idle.load_from_svg_file(    path + "im_all_plates_idle"    + ext, false, false, false, 200);
    result = result && item->image_slicing.load_from_svg_file( path + "im_all_plates_slicing" + ext, false, false, false, 200);
    result = result && item->image_failed.load_from_svg_file(  path + "im_all_plates_failed"  + ext, false, false, false, 200);
    m_sel_plate_toolbar.m_all_plates_stats_item = item;

    return result;
}

void GLCanvas3D::_update_select_plate_toolbar_stats_item(bool force_selected) {
    PartPlateList& plate_list = wxGetApp().plater()->get_partplate_list();
    if (plate_list.get_nonempty_plate_list().size() > 1)
        m_sel_plate_toolbar.show_stats_item = true;
    else
        m_sel_plate_toolbar.show_stats_item = false;

    if (force_selected && m_sel_plate_toolbar.show_stats_item)
        m_sel_plate_toolbar.m_all_plates_stats_item->selected = true;
}

bool GLCanvas3D::_update_imgui_select_plate_toolbar()
{
    bool result = true;
    if (!m_sel_plate_toolbar.is_enabled() || m_sel_plate_toolbar.is_render_finish) return false;

    _update_select_plate_toolbar_stats_item();

    m_sel_plate_toolbar.del_all_item();

    PartPlateList& plate_list = wxGetApp().plater()->get_partplate_list();
    for (int i = 0; i < plate_list.get_plate_count(); i++) {
        IMToolbarItem* item = new IMToolbarItem();
        PartPlate* plate = plate_list.get_plate(i);
        if (plate && plate->thumbnail_data.is_valid()) {
            PartPlate* plate = plate_list.get_plate(i);
            item->image_data = plate->thumbnail_data.pixels;
            item->image_width = plate->thumbnail_data.width;
            item->image_height = plate->thumbnail_data.height;
            result = item->generate_texture();
        }
        m_sel_plate_toolbar.m_items.push_back(item);
    }

    m_sel_plate_toolbar.is_display_scrollbar = false;
    return result;
}

//BBS: GUI refactor
//init the assemble view toolbar on the top
bool GLCanvas3D::_init_assemble_view_toolbar()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": enter,  m_assemble_view_toolbar.is_enabled=" << m_assemble_view_toolbar.is_enabled() << "\n";
    if (!m_assemble_view_toolbar.is_enabled())
        return true;

    BackgroundTexture::Metadata background_data;
    background_data.filename = m_is_dark ? "toolbar_background_dark.png" : "toolbar_background.png";
    background_data.left = 16;
    background_data.top = 16;
    background_data.right = 16;
    background_data.bottom = 16;

    if (!m_assemble_view_toolbar.init(background_data))
    {
        // unable to init the toolbar texture, disable it
        m_assemble_view_toolbar.set_enabled(false);
        return true;
    }

    m_assemble_view_toolbar.set_layout_type(GLToolbar::Layout::Horizontal);
    //BBS: assemble toolbar is at the top and right, we don't need the rounded-corner effect at the left side and the top side
    m_assemble_view_toolbar.set_horizontal_orientation(GLToolbar::Layout::HO_Left);
    m_assemble_view_toolbar.set_vertical_orientation(GLToolbar::Layout::VO_Top);
    m_assemble_view_toolbar.set_border(4.0f);
    m_assemble_view_toolbar.set_separator_size(10);
    m_assemble_view_toolbar.set_gap_size(4);

    m_assemble_view_toolbar.del_all_item();

    GLToolbarItem::Data item;
    item.name = "assembly_view";
    item.icon_filename = m_is_dark ? "toolbar_assemble_dark.svg" : "toolbar_assemble.svg";
    item.tooltip = _utf8(L("Assembly View"));
    item.sprite_id = 1;
    item.left.toggable = false;
    item.left.action_callback = [this]() { if (m_canvas != nullptr) wxPostEvent(m_canvas, SimpleEvent(EVT_GLVIEWTOOLBAR_ASSEMBLE)); };
    item.left.render_callback = GLToolbarItem::Default_Render_Callback;
    item.visible = true;
    item.visibility_callback = []()->bool { return true; };
    item.enabling_callback = []()->bool {
        return wxGetApp().plater()->has_assmeble_view();
    };
    if (!m_assemble_view_toolbar.add_item(item))
        return false;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Finished Successfully\n";
    return true;
}

bool GLCanvas3D::_init_return_toolbar()
{
    if (!m_return_toolbar.is_enabled())
        return true;

    return m_return_toolbar.init();
}

bool GLCanvas3D::_init_separator_toolbar()
{
    if (!m_separator_toolbar.is_enabled())
        return true;

    BackgroundTexture::Metadata background_data;
    background_data.filename = m_is_dark ? "toolbar_background_dark.png" : "toolbar_background.png";
    background_data.left = 0;
    background_data.top = 0;
    background_data.right = 0;
    background_data.bottom = 0;

    if (!m_separator_toolbar.init(background_data))
    {
        // unable to init the toolbar texture, disable it
        m_separator_toolbar.set_enabled(false);
        return true;
    }

    m_separator_toolbar.set_layout_type(GLToolbar::Layout::Horizontal);
    //BBS: assemble toolbar is at the top and right, we don't need the rounded-corner effect at the left side and the top side
    m_separator_toolbar.set_horizontal_orientation(GLToolbar::Layout::HO_Left);
    m_separator_toolbar.set_vertical_orientation(GLToolbar::Layout::VO_Top);
    m_separator_toolbar.set_border(4.0f);

    m_separator_toolbar.del_all_item();

    GLToolbarItem::Data sperate_item;
    sperate_item.name = "start_seperator";
    sperate_item.icon_filename = "seperator.svg";
    sperate_item.sprite_id = 0;
    sperate_item.left.action_callback = []() {};
    sperate_item.visibility_callback = []()->bool { return true; };
    sperate_item.enabling_callback = []()->bool { return false; };
    if (!m_separator_toolbar.add_item(sperate_item))
        return false;

     return true;
}


// BBS
#if 0
bool GLCanvas3D::_init_view_toolbar()
{
    return wxGetApp().plater()->init_view_toolbar();
}
#endif

bool GLCanvas3D::_init_collapse_toolbar()
{
    return wxGetApp().plater()->init_collapse_toolbar();
}

bool GLCanvas3D::_set_current()
{
    return m_context != nullptr && m_canvas->SetCurrent(*m_context);
}

void GLCanvas3D::_resize(unsigned int w, unsigned int h)
{
    if (m_canvas == nullptr && m_context == nullptr)
        return;

    const std::array<unsigned int, 2> new_size = { w, h };
    if (m_old_size == new_size)
        return;

    m_old_size = new_size;

    auto* imgui = wxGetApp().imgui();
    imgui->set_display_size(static_cast<float>(w), static_cast<float>(h));

    //BBS reduce render
    if (m_last_w == w && m_last_h == h) {
        return;
    }

    m_last_w = w;
    m_last_h = h;

    float font_size = wxGetApp().em_unit();

#ifdef _WIN32
    // On Windows, if manually scaled here, rendering issues can occur when the system's Display
    // scaling is greater than 300% as the font's size gets to be to large. So, use imgui font
    // scaling instead (see: ImGuiWrapper::init_font() and issue #3401)
    font_size *= (font_size > 30.0f) ? 1.0f : 1.5f;
#else
    font_size *= 1.5f;
#endif

#if ENABLE_RETINA_GL
    imgui->set_scaling(font_size, 1.0f, m_retina_helper->get_scale_factor());
#else
    imgui->set_scaling(font_size, m_canvas->GetContentScaleFactor(), 1.0f);
#endif

    this->request_extra_frame();

    // ensures that this canvas is current
    _set_current();
}

BoundingBoxf3 GLCanvas3D::_max_bounding_box(bool include_gizmos, bool include_bed_model, bool include_plates) const
{
    BoundingBoxf3 bb = volumes_bounding_box();

    // The following is a workaround for gizmos not being taken in account when calculating the tight camera frustrum
    // A better solution would ask the gizmo manager for the bounding box of the current active gizmo, if any
    if (include_gizmos && m_gizmos.is_running())
    {
        BoundingBoxf3 sel_bb = m_selection.get_bounding_box();
        Vec3d sel_bb_center = sel_bb.center();
        Vec3d extend_by = sel_bb.max_size() * Vec3d::Ones();
        bb.merge(BoundingBoxf3(sel_bb_center - extend_by, sel_bb_center + extend_by));
    }

    bb.merge(include_bed_model ? m_bed.extended_bounding_box() : m_bed.build_volume().bounding_volume());
    if (include_plates) {
        bb.merge(wxGetApp().plater()->get_partplate_list().get_bounding_box());
    }

    if (!m_main_toolbar.is_enabled()) {
        const BoundingBoxf3& toolpath_bb = m_gcode_viewer.get_max_bounding_box();
        if (toolpath_bb.max_size() > 0.f)
            bb.merge(toolpath_bb);
        else
            bb.merge(m_gcode_viewer.get_shell_bounding_box());
    }

    if ((m_canvas_type == CanvasView3D) && (fff_print()->config().print_sequence == PrintSequence::ByObject)) {
        float height_to_lid, height_to_rod;
        wxGetApp().plater()->get_partplate_list().get_height_limits(height_to_lid, height_to_rod);
        bb.max.z() = std::max(bb.max.z(), (double)height_to_lid);
    }

    return bb;
}

void GLCanvas3D::_zoom_to_box(const BoundingBoxf3& box, double margin_factor)
{
    wxGetApp().plater()->get_camera().zoom_to_box(box, margin_factor);
    m_dirty = true;
}

void GLCanvas3D::_update_camera_zoom(double zoom)
{
    wxGetApp().plater()->get_camera().update_zoom(zoom);
    m_dirty = true;
}

void GLCanvas3D::_refresh_if_shown_on_screen()
{
    if (_is_shown_on_screen()) {
        const Size& cnv_size = get_canvas_size();
        _resize((unsigned int)cnv_size.get_width(), (unsigned int)cnv_size.get_height());

        // Because of performance problems on macOS, where PaintEvents are not delivered
        // frequently enough, we call render() here directly when we can.
        render();
    }
}

void GLCanvas3D::_picking_pass()
{
    if (!m_picking_enabled || m_mouse.dragging || m_mouse.position == Vec2d(DBL_MAX, DBL_MAX) || m_gizmos.is_dragging()) {
#if ENABLE_RAYCAST_PICKING_DEBUG
        ImGuiWrapper& imgui = *wxGetApp().imgui();
        imgui.begin(std::string("Hit result"), ImGuiWindowFlags_AlwaysAutoResize);
        imgui.text("Picking disabled");
        imgui.end();
#endif // ENABLE_RAYCAST_PICKING_DEBUG
        return;
    }

    m_hover_volume_idxs.clear();
    m_hover_plate_idxs.clear();

    // Orca: ignore clipping plane if not applying
    GLGizmoBase *current_gizmo  = m_gizmos.get_current();
    const ClippingPlane clipping_plane = ((!current_gizmo || current_gizmo->apply_clipping_plane()) ? m_gizmos.get_clipping_plane() :
                                                                                                      ClippingPlane::ClipsNothing())
                                             .inverted_normal();
    const SceneRaycaster::HitResult hit = m_scene_raycaster.hit(m_mouse.position, wxGetApp().plater()->get_camera(), &clipping_plane);
    if (hit.is_valid()) {
        switch (hit.type)
        {
        case SceneRaycaster::EType::Volume:
        {
            if (0 <= hit.raycaster_id && hit.raycaster_id < (int)m_volumes.volumes.size()) {
                const GLVolume* volume = m_volumes.volumes[hit.raycaster_id];
                if (volume->is_active && !volume->disabled && (volume->composite_id.volume_id >= 0 || m_render_sla_auxiliaries)) {
                    // do not add the volume id if any gizmo is active and CTRL is pressed
                    if (m_gizmos.get_current_type() == GLGizmosManager::EType::Undefined || !wxGetKeyState(WXK_CONTROL))
                        m_hover_volume_idxs.emplace_back(hit.raycaster_id);
                    m_gizmos.set_hover_id(-1);
                }
            }
            else
                assert(false);

            break;
        }
        case SceneRaycaster::EType::Gizmo:
        case SceneRaycaster::EType::FallbackGizmo:
        {
            const Size& cnv_size = get_canvas_size();
            const bool inside = 0 <= m_mouse.position.x() && m_mouse.position.x() < cnv_size.get_width() &&
                0 <= m_mouse.position.y() && m_mouse.position.y() < cnv_size.get_height();
            m_gizmos.set_hover_id(inside ? hit.raycaster_id : -1);
            break;
        }
        case SceneRaycaster::EType::Bed:
        {
            // BBS: add plate picking logic
            int plate_hover_id = hit.raycaster_id;
            if (plate_hover_id >= 0 && plate_hover_id < PartPlateList::MAX_PLATES_COUNT * PartPlate::GRABBER_COUNT) {
                wxGetApp().plater()->get_partplate_list().set_hover_id(plate_hover_id);
                m_hover_plate_idxs.emplace_back(plate_hover_id);
            } else {
                wxGetApp().plater()->get_partplate_list().reset_hover_id();
            }
            m_gizmos.set_hover_id(-1);
            break;
        }
        default:
        {
            assert(false);
            break;
        }
        }
    }
    else
        m_gizmos.set_hover_id(-1);

    _update_volumes_hover_state();

#if ENABLE_RAYCAST_PICKING_DEBUG
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    imgui.begin(std::string("Hit result"), ImGuiWindowFlags_AlwaysAutoResize);
    std::string object_type = "None";
    switch (hit.type)
    {
    case SceneRaycaster::EType::Bed:   { object_type = "Bed"; break; }
    case SceneRaycaster::EType::Gizmo: { object_type = "Gizmo element"; break; }
    case SceneRaycaster::EType::FallbackGizmo: { object_type = "Gizmo2 element"; break; }
    case SceneRaycaster::EType::Volume:
    {
        if (m_volumes.volumes[hit.raycaster_id]->is_wipe_tower)
            object_type = "Volume (Wipe tower)";
        else if (m_volumes.volumes[hit.raycaster_id]->volume_idx() == -int(slaposPad))
            object_type = "Volume (SLA pad)";
        else if (m_volumes.volumes[hit.raycaster_id]->volume_idx() == -int(slaposSupportTree))
            object_type = "Volume (SLA supports)";
        else if (m_volumes.volumes[hit.raycaster_id]->is_modifier)
            object_type = "Volume (Modifier)";
        else
            object_type = "Volume (Part)";
        break;
    }
    default: { break; }
    }

    auto add_strings_row_to_table = [&imgui](const std::string& col_1, const ImVec4& col_1_color, const std::string& col_2, const ImVec4& col_2_color,
        const std::string& col_3 = "", const ImVec4& col_3_color = ImGui::GetStyleColorVec4(ImGuiCol_Text)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        imgui.text_colored(col_1_color, col_1.c_str());
        ImGui::TableSetColumnIndex(1);
        imgui.text_colored(col_2_color, col_2.c_str());
        if (!col_3.empty()) {
            ImGui::TableSetColumnIndex(2);
            imgui.text_colored(col_3_color, col_3.c_str());
        }
    };

    char buf[1024];
    if (hit.type != SceneRaycaster::EType::None) {
        if (ImGui::BeginTable("Hit", 2)) {
            add_strings_row_to_table("Object ID", ImGuiWrapper::COL_ORANGE_LIGHT, std::to_string(hit.raycaster_id), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            add_strings_row_to_table("Type", ImGuiWrapper::COL_ORANGE_LIGHT, object_type, ImGui::GetStyleColorVec4(ImGuiCol_Text));
            sprintf(buf, "%.3f, %.3f, %.3f", hit.position.x(), hit.position.y(), hit.position.z());
            add_strings_row_to_table("Position", ImGuiWrapper::COL_ORANGE_LIGHT, std::string(buf), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            sprintf(buf, "%.3f, %.3f, %.3f", hit.normal.x(), hit.normal.y(), hit.normal.z());
            add_strings_row_to_table("Normal", ImGuiWrapper::COL_ORANGE_LIGHT, std::string(buf), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            ImGui::EndTable();
        }
    }
    else
        imgui.text("NO HIT");

    ImGui::Separator();
    imgui.text("Registered for picking:");
    if (ImGui::BeginTable("Raycasters", 2)) {
        sprintf(buf, "%d (%d)", (int)m_scene_raycaster.beds_count(), (int)m_scene_raycaster.active_beds_count());
        add_strings_row_to_table("Beds", ImGuiWrapper::COL_ORANGE_LIGHT, std::string(buf), ImGui::GetStyleColorVec4(ImGuiCol_Text));
        sprintf(buf, "%d (%d)", (int)m_scene_raycaster.volumes_count(), (int)m_scene_raycaster.active_volumes_count());
        add_strings_row_to_table("Volumes", ImGuiWrapper::COL_ORANGE_LIGHT, std::string(buf), ImGui::GetStyleColorVec4(ImGuiCol_Text));
        sprintf(buf, "%d (%d)", (int)m_scene_raycaster.gizmos_count(), (int)m_scene_raycaster.active_gizmos_count());
        add_strings_row_to_table("Gizmo elements", ImGuiWrapper::COL_ORANGE_LIGHT, std::string(buf), ImGui::GetStyleColorVec4(ImGuiCol_Text));
        sprintf(buf, "%d (%d)", (int)m_scene_raycaster.fallback_gizmos_count(), (int)m_scene_raycaster.active_fallback_gizmos_count());
        add_strings_row_to_table("Gizmo2 elements", ImGuiWrapper::COL_ORANGE_LIGHT, std::string(buf), ImGui::GetStyleColorVec4(ImGuiCol_Text));
        ImGui::EndTable();
    }

    std::vector<std::shared_ptr<SceneRaycasterItem>>* gizmo_raycasters = m_scene_raycaster.get_raycasters(SceneRaycaster::EType::Gizmo);
    if (gizmo_raycasters != nullptr && !gizmo_raycasters->empty()) {
        ImGui::Separator();
        imgui.text("Gizmo raycasters IDs:");
        if (ImGui::BeginTable("GizmoRaycasters", 3)) {
            for (size_t i = 0; i < gizmo_raycasters->size(); ++i) {
                add_strings_row_to_table(std::to_string(i), ImGuiWrapper::COL_ORANGE_LIGHT,
                    std::to_string(SceneRaycaster::decode_id(SceneRaycaster::EType::Gizmo, (*gizmo_raycasters)[i]->get_id())), ImGui::GetStyleColorVec4(ImGuiCol_Text),
                    to_string(Geometry::Transformation((*gizmo_raycasters)[i]->get_transform()).get_offset()), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            }
            ImGui::EndTable();
        }
    }

    std::vector<std::shared_ptr<SceneRaycasterItem>>* gizmo2_raycasters = m_scene_raycaster.get_raycasters(SceneRaycaster::EType::FallbackGizmo);
    if (gizmo2_raycasters != nullptr && !gizmo2_raycasters->empty()) {
        ImGui::Separator();
        imgui.text("Gizmo2 raycasters IDs:");
        if (ImGui::BeginTable("Gizmo2Raycasters", 3)) {
            for (size_t i = 0; i < gizmo2_raycasters->size(); ++i) {
                add_strings_row_to_table(std::to_string(i), ImGuiWrapper::COL_ORANGE_LIGHT,
                    std::to_string(SceneRaycaster::decode_id(SceneRaycaster::EType::FallbackGizmo, (*gizmo2_raycasters)[i]->get_id())), ImGui::GetStyleColorVec4(ImGuiCol_Text),
                    to_string(Geometry::Transformation((*gizmo2_raycasters)[i]->get_transform()).get_offset()), ImGui::GetStyleColorVec4(ImGuiCol_Text));
            }
            ImGui::EndTable();
        }
    }

    imgui.end();
#endif // ENABLE_RAYCAST_PICKING_DEBUG
}

void GLCanvas3D::_rectangular_selection_picking_pass()
{
    m_gizmos.set_hover_id(-1);

    std::set<int> idxs;

    if (m_picking_enabled) {
        const size_t width  = std::max<size_t>(m_rectangle_selection.get_width(), 1);
        const size_t height = std::max<size_t>(m_rectangle_selection.get_height(), 1);

        const OpenGLManager::EFramebufferType framebuffers_type = OpenGLManager::get_framebuffers_type();
        bool use_framebuffer = framebuffers_type != OpenGLManager::EFramebufferType::Unknown;

        GLuint render_fbo = 0;
        GLuint render_tex = 0;
        GLuint render_depth = 0;
        if (use_framebuffer) {
            // setup a framebuffer which covers only the selection rectangle
            if (framebuffers_type == OpenGLManager::EFramebufferType::Arb) {
                glsafe(::glGenFramebuffers(1, &render_fbo));
                glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, render_fbo));
            }
            else {
                glsafe(::glGenFramebuffersEXT(1, &render_fbo));
                glsafe(::glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, render_fbo));
            }
            glsafe(::glGenTextures(1, &render_tex));
            glsafe(::glBindTexture(GL_TEXTURE_2D, render_tex));
            glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
            if (framebuffers_type == OpenGLManager::EFramebufferType::Arb) {
                glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, render_tex, 0));
                glsafe(::glGenRenderbuffers(1, &render_depth));
                glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, render_depth));
                glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, width, height));
                glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, render_depth));
            }
            else {
                glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, render_tex, 0));
                glsafe(::glGenRenderbuffersEXT(1, &render_depth));
                glsafe(::glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, render_depth));
                glsafe(::glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT, width, height));
                glsafe(::glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, render_depth));
            }
            const GLenum drawBufs[] = { GL_COLOR_ATTACHMENT0 };
            glsafe(::glDrawBuffers(1, drawBufs));
            if (framebuffers_type == OpenGLManager::EFramebufferType::Arb) {
                if (::glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                    use_framebuffer = false;
            }
            else {
                if (::glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT)
                    use_framebuffer = false;
            }
        }

        if (m_multisample_allowed)
        	// This flag is often ignored by NVIDIA drivers if rendering into a screen buffer.
            glsafe(::glDisable(GL_MULTISAMPLE));

        glsafe(::glDisable(GL_BLEND));
        glsafe(::glEnable(GL_DEPTH_TEST));

        glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));

        Camera& main_camera = wxGetApp().plater()->get_camera();
        Camera framebuffer_camera;
        Camera* camera = &main_camera;
        if (use_framebuffer) {
            // setup a camera which covers only the selection rectangle
            const std::array<int, 4>& viewport = camera->get_viewport();
            const double near_left   = camera->get_near_left();
            const double near_bottom = camera->get_near_bottom();
            const double near_width  = camera->get_near_width();
            const double near_height = camera->get_near_height();

            const double ratio_x = near_width / double(viewport[2]);
            const double ratio_y = near_height / double(viewport[3]);

            const double rect_near_left   = near_left + double(m_rectangle_selection.get_left()) * ratio_x;
            const double rect_near_bottom = near_bottom + (double(viewport[3]) - double(m_rectangle_selection.get_bottom())) * ratio_y;
            double rect_near_right = near_left + double(m_rectangle_selection.get_right()) * ratio_x;
            double rect_near_top   = near_bottom + (double(viewport[3]) - double(m_rectangle_selection.get_top())) * ratio_y;

            if (rect_near_left == rect_near_right)
                rect_near_right = rect_near_left + ratio_x;
            if (rect_near_bottom == rect_near_top)
                rect_near_top = rect_near_bottom + ratio_y;

            framebuffer_camera.look_at(camera->get_position(), camera->get_target(), camera->get_dir_up());
            framebuffer_camera.apply_projection(rect_near_left, rect_near_right, rect_near_bottom, rect_near_top, camera->get_near_z(), camera->get_far_z());
            framebuffer_camera.set_viewport(0, 0, width, height);
            framebuffer_camera.apply_viewport();
            camera = &framebuffer_camera;
        }

        _render_volumes_for_picking(*camera);
        //BBS: remove the bed picking logic
        //_render_bed_for_picking(!wxGetApp().plater()->get_camera().is_looking_downward());

        if (m_multisample_allowed)
            glsafe(::glEnable(GL_MULTISAMPLE));

        const size_t px_count = width * height;

        const size_t left = use_framebuffer ? 0 : (size_t)m_rectangle_selection.get_left();
        const size_t top  = use_framebuffer ? 0 : (size_t)get_canvas_size().get_height() - (size_t)m_rectangle_selection.get_top();
#define USE_PARALLEL 1
#if USE_PARALLEL
            struct Pixel
            {
                std::array<GLubyte, 4> data;
            	// Only non-interpolated colors are valid, those have their lowest three bits zeroed.
                bool valid() const { return picking_checksum_alpha_channel(data[0], data[1], data[2]) == data[3]; }
                // we reserve color = (0,0,0) for occluders (as the printbed)
                // volumes' id are shifted by 1
                // see: _render_volumes_for_picking()
                //BBS: remove the bed picking logic
                int id() const { return data[0] + (data[1] << 8) + (data[2] << 16); }
                //int id() const { return data[0] + (data[1] << 8) + (data[2] << 16) - 1; }
            };

            std::vector<Pixel> frame(px_count);
            glsafe(::glReadPixels(left, top, width, height, GL_RGBA, GL_UNSIGNED_BYTE, (void*)frame.data()));

            tbb::spin_mutex mutex;
            tbb::parallel_for(tbb::blocked_range<size_t>(0, frame.size(), (size_t)width),
                [this, &frame, &idxs, &mutex](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i < range.end(); ++i)
                	if (frame[i].valid()) {
                    	int volume_id = frame[i].id();
                    	if (0 <= volume_id && volume_id < (int)m_volumes.volumes.size()) {
                        	mutex.lock();
                        	idxs.insert(volume_id);
                        	mutex.unlock();
                    	}
                	}
            });
#else
            std::vector<GLubyte> frame(4 * px_count);
            glsafe(::glReadPixels(left, top, width, height, GL_RGBA, GL_UNSIGNED_BYTE, (void*)frame.data()));

            for (int i = 0; i < px_count; ++i)
            {
                int px_id = 4 * i;
                int volume_id = frame[px_id] + (frame[px_id + 1] << 8) + (frame[px_id + 2] << 16);
                if (0 <= volume_id && volume_id < (int)m_volumes.volumes.size())
                    idxs.insert(volume_id);
            }
#endif // USE_PARALLEL
            if (camera != &main_camera)
                main_camera.apply_viewport();

            if (framebuffers_type == OpenGLManager::EFramebufferType::Arb) {
                glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, 0));
                if (render_depth != 0)
                    glsafe(::glDeleteRenderbuffers(1, &render_depth));
                if (render_fbo != 0)
                    glsafe(::glDeleteFramebuffers(1, &render_fbo));
            }
            else if (framebuffers_type == OpenGLManager::EFramebufferType::Ext) {
                glsafe(::glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0));
                if (render_depth != 0)
                    glsafe(::glDeleteRenderbuffersEXT(1, &render_depth));
                if (render_fbo != 0)
                    glsafe(::glDeleteFramebuffersEXT(1, &render_fbo));
            }

            if (render_tex != 0)
                glsafe(::glDeleteTextures(1, &render_tex));
    }

    m_hover_volume_idxs.assign(idxs.begin(), idxs.end());
    _update_volumes_hover_state();
}

void GLCanvas3D::_render_background()
{
    bool use_error_color = false;
    if (wxGetApp().is_editor()) {
        use_error_color = m_dynamic_background_enabled &&
        (current_printer_technology() != ptSLA || !m_volumes.empty());

        if (!m_volumes.empty())
            use_error_color &= _is_any_volume_outside();
        else {
            //BBS: use current plater's bounding box
            //BoundingBoxf3 test_volume = (m_config != nullptr) ? print_volume(*m_config) : BoundingBoxf3();
            BoundingBoxf3 test_volume = (const_cast<GLCanvas3D*>(this))->_get_current_partplate_print_volume();
            const BoundingBoxf3& path_bounding_box = m_gcode_viewer.get_paths_bounding_box();
            if (empty(path_bounding_box))
                use_error_color = false;
            else
                //BBS: use previous result
                use_error_color = (test_volume.radius() > 0.0) ? m_toolpath_outside : false;
            //use_error_color &= (test_volume.radius() > 0.0) ? !test_volume.contains(path_bounding_box) : false;
        }
    }

    // Draws a bottom to top gradient over the complete screen.
    glsafe(::glDisable(GL_DEPTH_TEST));

    ColorRGBA background_color = m_is_dark ? DEFAULT_BG_LIGHT_COLOR_DARK : DEFAULT_BG_LIGHT_COLOR;
    ColorRGBA error_background_color = m_is_dark ? ERROR_BG_LIGHT_COLOR_DARK : ERROR_BG_LIGHT_COLOR;
    const ColorRGBA bottom_color = use_error_color ? error_background_color : background_color;

    if (!m_background.is_initialized()) {
        m_background.reset();

        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P2T2 };
        init_data.reserve_vertices(4);
        init_data.reserve_indices(6);

        // vertices
        init_data.add_vertex(Vec2f(-1.0f, -1.0f), Vec2f(0.0f, 0.0f));
        init_data.add_vertex(Vec2f(1.0f, -1.0f),  Vec2f(1.0f, 0.0f));
        init_data.add_vertex(Vec2f(1.0f, 1.0f),   Vec2f(1.0f, 1.0f));
        init_data.add_vertex(Vec2f(-1.0f, 1.0f),  Vec2f(0.0f, 1.0f));

        // indices
        init_data.add_triangle(0, 1, 2);
        init_data.add_triangle(2, 3, 0);

        m_background.init_from(std::move(init_data));
    }

    GLShaderProgram* shader = wxGetApp().get_shader("background");
    if (shader != nullptr) {
        shader->start_using();
        shader->set_uniform("top_color", bottom_color);
        shader->set_uniform("bottom_color", bottom_color);
        m_background.render();
        shader->stop_using();
    }

    glsafe(::glEnable(GL_DEPTH_TEST));
}

void GLCanvas3D::_render_bed(const Transform3d& view_matrix, const Transform3d& projection_matrix, bool bottom, bool show_axes)
{
    float scale_factor = 1.0;
#if ENABLE_RETINA_GL
    scale_factor = m_retina_helper->get_scale_factor();
#endif // ENABLE_RETINA_GL

    /*
    bool show_texture = ! bottom ||
            (m_gizmos.get_current_type() != GLGizmosManager::FdmSupports
          && m_gizmos.get_current_type() != GLGizmosManager::SlaSupports
          && m_gizmos.get_current_type() != GLGizmosManager::Hollow
          && m_gizmos.get_current_type() != GLGizmosManager::Seam
          && m_gizmos.get_current_type() != GLGizmosManager::MmSegmentation
          && m_gizmos.get_current_type() != GLGizmosManager::FuzzySkin);
    */
    //bool show_texture = true;
    //BBS set axes mode
    m_bed.set_axes_mode(m_main_toolbar.is_enabled());
    m_bed.render(*this, view_matrix, projection_matrix, bottom, scale_factor, show_axes);
}

void GLCanvas3D::_render_platelist(const Transform3d& view_matrix, const Transform3d& projection_matrix, bool bottom, bool only_current, bool only_body, int hover_id, bool render_cali, bool show_grid)
{
    wxGetApp().plater()->get_partplate_list().render(view_matrix, projection_matrix, bottom, only_current, only_body, hover_id, render_cali, show_grid);
}

void GLCanvas3D::_render_plane() const
{
    ;//TODO render assemble plane
}

//BBS: add outline drawing logic
void GLCanvas3D::_render_objects(GLVolumeCollection::ERenderType type, bool with_outline)
{
    if (m_volumes.empty())
        return;

    glsafe(::glEnable(GL_DEPTH_TEST));

    m_camera_clipping_plane = m_gizmos.get_clipping_plane();

    if (m_picking_enabled)
        // Update the layer editing selection to the first object selected, update the current object maximum Z.
        m_layers_editing.select_object(*m_model, this->is_layers_editing_enabled() ? m_selection.get_object_idx() : -1);

    if (const BuildVolume &build_volume = m_bed.build_volume(); build_volume.valid()) {
        switch (build_volume.type()) {
        case BuildVolume_Type::Rectangle: {
            const BoundingBox3Base<Vec3d> bed_bb = build_volume.bounding_volume().inflated(BuildVolume::SceneEpsilon);
            m_volumes.set_print_volume({ 0, // Rectangle
                { float(bed_bb.min.x()), float(bed_bb.min.y()), float(bed_bb.max.x()), float(bed_bb.max.y()) },
                { 0.0f, float(build_volume.printable_height()) } });
            break;
        }
        case BuildVolume_Type::Circle: {
            m_volumes.set_print_volume({ 1, // Circle
                { unscaled<float>(build_volume.circle().center.x()), unscaled<float>(build_volume.circle().center.y()), unscaled<float>(build_volume.circle().radius + BuildVolume::SceneEpsilon), 0.0f },
                { 0.0f, float(build_volume.printable_height() + BuildVolume::SceneEpsilon) } });
            break;
        }
        default:
        case BuildVolume_Type::Convex:
        case BuildVolume_Type::Custom: {
            m_volumes.set_print_volume({ static_cast<int>(type),
                { -FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX },
                { -FLT_MAX, FLT_MAX } }
            );
        }
        }
        if (m_requires_check_outside_state) {
            m_volumes.check_outside_state(build_volume, nullptr, nullptr);
            m_requires_check_outside_state = false;
        }
    }

    if (m_use_clipping_planes)
        m_volumes.set_z_range(-m_clipping_planes[0].get_data()[3], m_clipping_planes[1].get_data()[3]);
    else
        m_volumes.set_z_range(-FLT_MAX, FLT_MAX);

    GLGizmosManager& gm = get_gizmos_manager();
    GLGizmoBase* current_gizmo = gm.get_current();
    if (m_canvas_type == CanvasAssembleView) {
        m_volumes.set_clipping_plane(m_gizmos.get_assemble_view_clipping_plane().get_data());
    }
    else if (current_gizmo && !current_gizmo->apply_clipping_plane()) {
        m_volumes.set_clipping_plane(ClippingPlane::ClipsNothing().get_data());
    }
    else {
        m_volumes.set_clipping_plane(m_camera_clipping_plane.get_data());
    }
    if (m_canvas_type == CanvasAssembleView)
        m_volumes.set_show_sinking_contours(false);
    else
        m_volumes.set_show_sinking_contours(!m_gizmos.is_hiding_instances());

    GLShaderProgram* shader = wxGetApp().get_shader("gouraud");
    ECanvasType canvas_type = this->m_canvas_type;
    bool                 partly_inside_enable = canvas_type == ECanvasType::CanvasAssembleView ? false : true;
    if (shader != nullptr) {
        shader->start_using();

        const Size&   cvn_size = get_canvas_size();
        {
            const Camera& camera = wxGetApp().plater()->get_camera();
            shader->set_uniform("z_far", camera.get_far_z());
            shader->set_uniform("z_near", camera.get_near_z());
        }
        switch (type)
        {
        default:
        case GLVolumeCollection::ERenderType::Opaque:
        {
            GLGizmosManager& gm = get_gizmos_manager();
            if (dynamic_cast<GLGizmoPainterBase*>(gm.get_current()) == nullptr)
            {
                if (m_picking_enabled && m_layers_editing.is_enabled() && (m_layers_editing.last_object_id != -1) && (m_layers_editing.object_max_z() > 0.0f)) {
                    int object_id = m_layers_editing.last_object_id;
                const Camera& camera = wxGetApp().plater()->get_camera();
                m_volumes.render(type, false, camera.get_view_matrix(), camera.get_projection_matrix(), cvn_size, [object_id](const GLVolume& volume) {
                    // Which volume to paint without the layer height profile shader?
                    return volume.is_active && (volume.is_modifier || volume.composite_id.object_id != object_id);
                    });
                    m_layers_editing.render_volumes(*this, m_volumes);
                }
                else {
                    /*if (wxGetApp().plater()->is_wireframe_enabled()) {
                        if (wxGetApp().plater()->is_show_wireframe())
                            shader->set_uniform("show_wireframe", true);
                        else
                            shader->set_uniform("show_wireframe", false);
                    }*/
                    //BBS:add assemble view related logic
                    // do not cull backfaces to show broken geometry, if any
                const Camera& camera = wxGetApp().plater()->get_camera();
                    m_volumes.render(type, m_picking_enabled, camera.get_view_matrix(), camera.get_projection_matrix(), cvn_size, [this, canvas_type](const GLVolume& volume) {
                        if (canvas_type == ECanvasType::CanvasAssembleView) {
                            return !volume.is_modifier && !volume.is_wipe_tower;
                        }
                        else {
                            return (m_render_sla_auxiliaries || volume.composite_id.volume_id >= 0);
                        }
                        },
                        partly_inside_enable);
                }
            }
            else {
                // In case a painting gizmo is open, it should render the painted triangles
                // before transparent objects are rendered. Otherwise they would not be
                // visible when inside modifier meshes etc.
//                GLGizmosManager::EType type = gm.get_current_type();
                if (dynamic_cast<GLGizmoPainterBase*>(gm.get_current())) {
                    shader->stop_using();
                    gm.render_painter_gizmo();
                    shader->start_using();
                }
            }

            break;
        }
        case GLVolumeCollection::ERenderType::Transparent:
        {
            /*if (wxGetApp().plater()->is_wireframe_enabled()) {
                if (wxGetApp().plater()->is_show_wireframe())
                    shader->set_uniform("show_wireframe", true);
                else
                    shader->set_uniform("show_wireframe", false);
            }*/
            const Camera& camera = wxGetApp().plater()->get_camera();
            //BBS:add assemble view related logic
            m_volumes.render(type, false, camera.get_view_matrix(), camera.get_projection_matrix(), cvn_size, [canvas_type](const GLVolume& volume) {
                if (canvas_type == ECanvasType::CanvasAssembleView) {
                    return !volume.is_modifier;
                }
                else {
                    return true;
                }
                },
                partly_inside_enable);
            if (m_canvas_type == CanvasAssembleView && m_gizmos.m_assemble_view_data->model_objects_clipper()->get_position() > 0) {
                const GLGizmosManager& gm = get_gizmos_manager();
                shader->stop_using();
                gm.render_painter_assemble_view();
                shader->start_using();
            }
            break;
        }
        }

        /*if (wxGetApp().plater()->is_wireframe_enabled()) {
            shader->set_uniform("show_wireframe", false);
        }*/

        shader->stop_using();
    }

    m_camera_clipping_plane = ClippingPlane::ClipsNothing();
}

//BBS: GUI refactor: add canvas size as parameters
void GLCanvas3D::_render_gcode(int canvas_width, int canvas_height)
{
    m_gcode_viewer.render(canvas_width, canvas_height, SLIDER_RIGHT_MARGIN * GCODE_VIEWER_SLIDER_SCALE);
    IMSlider *layers_slider = m_gcode_viewer.get_layers_slider();
    IMSlider *moves_slider  = m_gcode_viewer.get_moves_slider();

    if (layers_slider->is_need_post_tick_event()) {
        auto evt = new wxCommandEvent(EVT_CUSTOMEVT_TICKSCHANGED, m_canvas->GetId());
        evt->SetInt((int)layers_slider->get_post_tick_event_type());
        wxPostEvent(m_canvas, *evt);
        layers_slider->reset_post_tick_event();
    }

    if (layers_slider->is_dirty()) {
        set_volumes_z_range({layers_slider->GetLowerValueD(), layers_slider->GetHigherValueD()});
        if (m_gcode_viewer.has_data()) {
            m_gcode_viewer.set_layers_z_range({static_cast<unsigned int>(layers_slider->GetLowerValue()), static_cast<unsigned int>(layers_slider->GetHigherValue())});
        }
        layers_slider->set_as_dirty(false);
        post_event(SimpleEvent(EVT_GLCANVAS_UPDATE));
        m_gcode_viewer.update_marker_curr_move();
    }

    if (moves_slider->is_dirty()) {
        moves_slider->set_as_dirty(false);
        m_gcode_viewer.update_sequential_view_current((moves_slider->GetLowerValueD() - 1.0), static_cast<unsigned int>(moves_slider->GetHigherValueD() - 1.0));
        post_event(SimpleEvent(EVT_GLCANVAS_UPDATE));
        m_gcode_viewer.update_marker_curr_move();
    }
}

void GLCanvas3D::_render_selection()
{
    float scale_factor = 1.0;
#if ENABLE_RETINA_GL
    scale_factor = m_retina_helper->get_scale_factor();
#endif // ENABLE_RETINA_GL

    if (!m_gizmos.is_running())
        m_selection.render(scale_factor);
}

void GLCanvas3D::_render_sequential_clearance()
{
    if (m_gizmos.is_dragging())
        return;
    auto type = m_gizmos.get_current_type();
    if (type == GLGizmosManager::EType::Undefined
        || can_sequential_clearance_show_in_gizmo()) {
        m_sequential_print_clearance.render();
    }
}

#if ENABLE_RENDER_SELECTION_CENTER
void GLCanvas3D::_render_selection_center()
{
    m_selection.render_center(m_gizmos.is_dragging());
}
#endif // ENABLE_RENDER_SELECTION_CENTER

void GLCanvas3D::_check_and_update_toolbar_icon_scale()
{
    // Update collapse toolbar
    GLToolbar& collapse_toolbar = wxGetApp().plater()->get_collapse_toolbar();
    collapse_toolbar.set_enabled(wxGetApp().plater()->get_sidebar_docking_state() != Sidebar::None);

    // Don't update a toolbar scale, when we are on a Preview
    if (wxGetApp().plater()->is_preview_shown()) {
        IMSlider   *m_layers_slider = get_gcode_viewer().get_layers_slider();
        IMSlider   *m_moves_slider  = get_gcode_viewer().get_moves_slider();
        float sc              = get_scale();
#ifdef WIN32
        int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
        sc *= (float) dpi / (float) DPI_DEFAULT;
#endif // WIN32

        m_layers_slider->set_scale(sc * GCODE_VIEWER_SLIDER_SCALE);
        m_moves_slider->set_scale(sc * GCODE_VIEWER_SLIDER_SCALE);
        m_gcode_viewer.set_scale(sc);

        auto *m_notification = wxGetApp().plater()->get_notification_manager();
        m_notification->set_scale(sc);
        return;
    }

    float scale = wxGetApp().toolbar_icon_scale() * get_scale();
    Size cnv_size = get_canvas_size();

    //BBS: GUI refactor: GLToolbar
    int size_i = int(GLToolbar::Default_Icons_Size * scale);
    // force even size
    if (size_i % 2 != 0)
        size_i -= 1;
    float size   = size_i;

    // Set current size for all top toolbars. It will be used for next calculations
    //BBS: GUI refactor: GLToolbar
    m_main_toolbar.set_icons_size(size);
    m_assemble_view_toolbar.set_icons_size(size);
    m_separator_toolbar.set_icons_size(size);
    collapse_toolbar.set_icons_size(size / 2.0);
    m_gizmos.set_overlay_icon_size(size);

    //BBS: GUI refactor: GLToolbar
#if BBS_TOOLBAR_ON_TOP
    float collapse_toolbar_width = collapse_toolbar.is_enabled() ? collapse_toolbar.get_width() : 0;

    float top_tb_width = m_main_toolbar.get_width() + m_gizmos.get_scaled_total_width() + m_assemble_view_toolbar.get_width() + m_separator_toolbar.get_width() + collapse_toolbar_width * 2;
    int   items_cnt = m_main_toolbar.get_visible_items_cnt() + m_gizmos.get_selectable_icons_cnt() + m_assemble_view_toolbar.get_visible_items_cnt() + m_separator_toolbar.get_visible_items_cnt() + collapse_toolbar.get_visible_items_cnt();
    float noitems_width = top_tb_width - size * items_cnt; // width of separators and borders in top toolbars

    // calculate scale needed for items in all top toolbars
    float new_h_scale = (cnv_size.get_width() - noitems_width) / (items_cnt * GLToolbar::Default_Icons_Size);

    //for protect
    if (new_h_scale <= 0) {
        new_h_scale = 1;
    }

    //use the same value as horizon
    float new_v_scale = new_h_scale;
#else
    float top_tb_width = = collapse_toolbar.get_width();
    int   items_cnt = collapse_toolbar.get_visible_items_cnt();
    float noitems_width = top_tb_width - size * items_cnt; // width of separators and borders in top toolbars

    // calculate scale needed for items in all top toolbars
    float new_h_scale = (cnv_size.get_width() - noitems_width) / (items_cnt * GLToolbar::Default_Icons_Size);

    //items_cnt = m_main_toolbar.get_visible_items_cnt() + m_gizmos.get_selectable_icons_cnt() + 3; // +3 means a place for top and view toolbars and separators in gizmos toolbar

    // calculate scale needed for items in the gizmos toolbar
    items_cnt = m_main_toolbar.get_visible_items_cnt() + m_gizmos.get_selectable_icons_cnt() + m_assemble_view_toolbar.get_visible_items_cnt();
    float new_v_scale = cnv_size.get_height() / (items_cnt * GLGizmosManager::Default_Icons_Size);
#endif

    // set minimum scale as a auto scale for the toolbars
    float new_scale = std::min(new_h_scale, new_v_scale);
    new_scale /= get_scale();
    if (fabs(new_scale - scale) > 0.05) // scale is changed by 5% and more
        wxGetApp().set_auto_toolbar_icon_scale(new_scale);
}

void GLCanvas3D::_render_overlays()
{
    glsafe(::glDisable(GL_DEPTH_TEST));

    _check_and_update_toolbar_icon_scale();

    _render_assemble_control();
    _render_assemble_info();

    _render_separator_toolbar_right();
    _render_separator_toolbar_left();
    _render_main_toolbar();
    _render_collapse_toolbar();
    _render_assemble_view_toolbar();
    //BBS: GUI refactor: GLToolbar
    _render_imgui_select_plate_toolbar();
    _render_return_toolbar();
    // BBS
    //_render_view_toolbar();
    _render_paint_toolbar();

    //BBS: GUI refactor: GLToolbar
    //move gizmos behind of main
    _render_gizmos_overlay();

    if (m_layers_editing.last_object_id >= 0 && m_layers_editing.object_max_z() > 0.0f)
        m_layers_editing.render_overlay(*this);

	auto curr_plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    auto curr_print_seq = curr_plate->get_real_print_seq();
    const Print* print = fff_print();
    bool sequential_print = (curr_print_seq == PrintSequence::ByObject) || print->config().print_order == PrintOrder::AsObjectList;
    std::vector<const ModelInstance*> sorted_instances;
    if (sequential_print) {
        if (print) {
            for (const PrintObject *print_object : print->objects())
            {
                for (const PrintInstance &instance : print_object->instances())
                {
                    sorted_instances.emplace_back(instance.model_instance);
                }
            }
        }
        /*for (ModelObject* model_object : m_model->objects)
            for (ModelInstance* model_instance : model_object->instances) {
                sorted_instances.emplace_back(model_instance);
            }*/
    }
    m_labels.render(sorted_instances);

    _render_3d_navigator();

    _render_canvas_toolbar();
}

void GLCanvas3D::_render_style_editor()
{
    bool show_style_editor = true;
    ImGui::Begin("ImGui Style Editor", &show_style_editor);
    // You can pass in a reference ImGuiStyle structure to compare to, revert to and save to
    // (without a reference style pointer, we will use one compared locally as a reference)

    ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.50f);
    ImGui::ShowFontSelector("Fonts##Selector");
    ImGui::Separator();

    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_None))
    {
        if (ImGui::BeginTabItem("Colors"))
        {
            static int output_dest = 0;
            static bool output_only_modified = false;
            if (ImGui::Button("Export"))
            {
                if (output_dest == 0)
                    ImGui::LogToClipboard();
                else
                    ImGui::LogToTTY();

                ImGui::LogText("RenderColors:" IM_NEWLINE);
                for (int i = 0; i < RenderCol_Count; i++)
                {
                    const ImVec4& col = RenderColor::colors[i];
                    const char* name = GetRenderColName(i);
                    if (!output_only_modified || memcmp(&col, &RenderColor::colors[i], sizeof(ImVec4)) != 0)
                        ImGui::LogText("RenderColor::colors[%s]%*s= ImVec4(%.2ff, %.2ff, %.2ff, %.2ff);" IM_NEWLINE,
                            name, 23 - (int)strlen(name), "", col.x, col.y, col.z, col.w);
                }
                ImGui::LogFinish();
            }
            ImGui::SameLine(); ImGui::SetNextItemWidth(120); ImGui::Combo("##output_type", &output_dest, "To Clipboard\0To TTY\0");
            ImGui::SameLine(); ImGui::Checkbox("Only Modified Colors", &output_only_modified);

            static ImGuiTextFilter filter;
            filter.Draw("Filter colors", ImGui::GetFontSize() * 16);

            static ImGuiColorEditFlags alpha_flags = 0;
            if (ImGui::RadioButton("Opaque", alpha_flags == ImGuiColorEditFlags_None)) { alpha_flags = ImGuiColorEditFlags_None; } ImGui::SameLine();
            if (ImGui::RadioButton("Alpha", alpha_flags == ImGuiColorEditFlags_AlphaPreview)) { alpha_flags = ImGuiColorEditFlags_AlphaPreview; } ImGui::SameLine();
            if (ImGui::RadioButton("Both", alpha_flags == ImGuiColorEditFlags_AlphaPreviewHalf)) { alpha_flags = ImGuiColorEditFlags_AlphaPreviewHalf; } ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
                ImGui::TextUnformatted("In the color list:\n"
                "Left-click on color square to open color picker,\n"
                    "Right-click to open edit options menu.");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImGui::BeginChild("##colors", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar | ImGuiWindowFlags_NavFlattened);
            ImGui::PushItemWidth(-160);
            for (int i = 0; i < RenderCol_Count; i++)
            {
                const char* name = GetRenderColName(i);
                if (!filter.PassFilter(name))
                    continue;
                ImGui::PushID(i);
                ImGui::ColorEdit4("##color", (float*)&RenderColor::colors[i], ImGuiColorEditFlags_AlphaBar | alpha_flags);
                // Tips: in a real user application, you may want to merge and use an icon font into the main font,
                // so instead of "Save"/"Revert" you'd use icons!
                // Read the FAQ and docs/FONTS.md about using icon fonts. It's really easy and super convenient!
                ImGui::SameLine(0.0f, 3.0f);
                if (ImGui::Button("Set")) {
                    GLVolume::update_render_colors();
                    PartPlate::update_render_colors();
                    GLGizmoBase::update_render_colors();
                    GLCanvas3D::update_render_colors();
                    Bed3D::update_render_colors();
                }
                ImGui::SameLine(0.0f, 3.0f);
                ImGui::TextUnformatted(name);
                ImGui::PopID();
            }
            ImGui::PopItemWidth();
            ImGui::EndChild();

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::PopItemWidth();
    ImGui::End();
}

void GLCanvas3D::_render_volumes_for_picking(const Camera& camera) const
{
    GLShaderProgram* shader = wxGetApp().get_shader("flat_clip");
    if (shader == nullptr)
        return;

    // do not cull backfaces to show broken geometry, if any
    glsafe(::glDisable(GL_CULL_FACE));

    const Transform3d& view_matrix = camera.get_view_matrix();
    for (size_t type = 0; type < 2; ++ type) {
        GLVolumeWithIdAndZList to_render = volumes_to_render(m_volumes.volumes, (type == 0) ? GLVolumeCollection::ERenderType::Opaque : GLVolumeCollection::ERenderType::Transparent, view_matrix);
        for (const GLVolumeWithIdAndZ& volume : to_render)
	        if (!volume.first->disabled && (volume.first->composite_id.volume_id >= 0 || m_render_sla_auxiliaries)) {
		        // Object picking mode. Render the object with a color encoding the object index.
                // we reserve color = (0,0,0) for occluders (as the printbed)
                // so we shift volumes' id by 1 to get the proper color
                //BBS: remove the bed picking logic
                const unsigned int id = volume.second.first;
                //const unsigned int id = 1 + volume.second.first;
                volume.first->model.set_color(picking_decode(id));
                shader->start_using();
                shader->set_uniform("view_model_matrix", view_matrix * volume.first->world_matrix());
                shader->set_uniform("projection_matrix", camera.get_projection_matrix());
                shader->set_uniform("volume_world_matrix", volume.first->world_matrix());
                shader->set_uniform("z_range", m_volumes.get_z_range());
                shader->set_uniform("clipping_plane", m_volumes.get_clipping_plane());
                volume.first->picking = true;
                volume.first->render();
                volume.first->picking = false;
                shader->stop_using();
	        }
	}

    glsafe(::glEnable(GL_CULL_FACE));
}

void GLCanvas3D::_render_current_gizmo() const
{
    //BBS update inv_zoom
    GLGizmoBase::INV_ZOOM = (float)wxGetApp().plater()->get_camera().get_inv_zoom();
    m_gizmos.render_current_gizmo();
}

//BBS: GUI refactor: GLToolbar adjust
//move the size calc to GLCanvas
void GLCanvas3D::_render_gizmos_overlay()
{
/*#if ENABLE_RETINA_GL
//     m_gizmos.set_overlay_scale(m_retina_helper->get_scale_factor());
    const float scale = m_retina_helper->get_scale_factor()*wxGetApp().toolbar_icon_scale();
    m_gizmos.set_overlay_scale(scale); //! #ys_FIXME_experiment
#else
//     m_gizmos.set_overlay_scale(m_canvas->GetContentScaleFactor());
//     m_gizmos.set_overlay_scale(wxGetApp().em_unit()*0.1f);
    const float size = int(GLGizmosManager::Default_Icons_Size * wxGetApp().toolbar_icon_scale());
    m_gizmos.set_overlay_icon_size(size); //! #ys_FIXME_experiment
#endif */ /* __WXMSW__ */
    m_gizmos.render_overlay();

    if (m_gizmo_highlighter.m_render_arrow)
    {
        m_gizmos.render_arrow(*this, m_gizmo_highlighter.m_gizmo_type);
    }
}

int GLCanvas3D::get_main_toolbar_offset() const
{
    const float cnv_width              = get_canvas_size().get_width();
    const float collapse_toolbar_width = get_collapse_toolbar_width() * 2;
    const float gizmo_width            = m_gizmos.get_scaled_total_width();
    const float assemble_width         = m_assemble_view_toolbar.get_width();
    const float separator_width        = m_separator_toolbar.get_width();
    const float toolbar_total_width    = m_main_toolbar.get_width() + separator_width + gizmo_width + assemble_width + collapse_toolbar_width;

    if (cnv_width < toolbar_total_width) {
        return is_collapse_toolbar_on_left() ? collapse_toolbar_width : 0;
    } else {
        const float offset = (cnv_width - toolbar_total_width) / 2;
        return is_collapse_toolbar_on_left() ? offset + collapse_toolbar_width : offset;
    }
}

//BBS: GUI refactor: GLToolbar adjust
//when rendering, {0, 0} is at the center, left-up is -0.5, 0.5, right-up is 0.5, -0.5
void GLCanvas3D::_render_main_toolbar()
{
    if (!m_main_toolbar.is_enabled())
        return;

    const Size cnv_size = get_canvas_size();
    const float top = 0.5f * (float)cnv_size.get_height();

    const float left = -0.5f * cnv_size.get_width() + get_main_toolbar_offset();
    m_main_toolbar.set_position(top, left);
    m_main_toolbar.render(*this);
    if (m_toolbar_highlighter.m_render_arrow)
        m_main_toolbar.render_arrow(*this, m_toolbar_highlighter.m_toolbar_item);
}

//BBS: GUI refactor: GLToolbar adjust
//when rendering, {0, 0} is at the center, {-0.5, 0.5} at the left-up
void GLCanvas3D::_render_imgui_select_plate_toolbar()
{
    if (!m_sel_plate_toolbar.is_enabled()) {
        if (!m_render_preview)
            m_render_preview = true;
        return;
    }

    IMToolbarItem* all_plates_stats_item = m_sel_plate_toolbar.m_all_plates_stats_item;

    PartPlateList& plate_list = wxGetApp().plater()->get_partplate_list();
    size_t         sliced_plates_cnt = 0; // ORCA make it accesable for other conditions

    for (int i = 0; i < plate_list.get_plate_count(); i++) {
        if (i < m_sel_plate_toolbar.m_items.size()) {
            if (i == plate_list.get_curr_plate_index() && !all_plates_stats_item->selected)
                m_sel_plate_toolbar.m_items[i]->selected = true;
            else
                m_sel_plate_toolbar.m_items[i]->selected = false;

            m_sel_plate_toolbar.m_items[i]->percent = plate_list.get_plate(i)->get_slicing_percent();

            if (plate_list.get_plate(i)->is_slice_result_valid()) {
                if (plate_list.get_plate(i)->is_slice_result_ready_for_print())
                    m_sel_plate_toolbar.m_items[i]->slice_state = IMToolbarItem::SliceState::SLICED;
                else
                    m_sel_plate_toolbar.m_items[i]->slice_state = IMToolbarItem::SliceState::SLICE_FAILED;
            }
            else {
                if (plate_list.get_plate(i)->has_printable_instances() && !plate_list.get_plate(i)->can_slice())
                    m_sel_plate_toolbar.m_items[i]->slice_state = IMToolbarItem::SliceState::SLICE_FAILED;
                else {
                    if (plate_list.get_plate(i)->get_slicing_percent() < 0.0f)
                        m_sel_plate_toolbar.m_items[i]->slice_state = IMToolbarItem::SliceState::UNSLICED;
                    else
                        m_sel_plate_toolbar.m_items[i]->slice_state = IMToolbarItem::SliceState::SLICING;
                }
            }
        }
    }
    if (m_sel_plate_toolbar.show_stats_item) {
        all_plates_stats_item->percent = 0.0f;

        for (auto plate : plate_list.get_nonempty_plate_list()) {
            if (plate->is_slice_result_valid() && plate->is_slice_result_ready_for_print())
                sliced_plates_cnt++;
        }
        all_plates_stats_item->percent = (float)(sliced_plates_cnt) / (float)(plate_list.get_nonempty_plate_list().size()) * 100.0f;

        if (all_plates_stats_item->percent == 0.0f)
            all_plates_stats_item->slice_state = IMToolbarItem::SliceState::UNSLICED;
        else if (sliced_plates_cnt == plate_list.get_nonempty_plate_list().size())
            all_plates_stats_item->slice_state = IMToolbarItem::SliceState::SLICED;
        else if (all_plates_stats_item->percent < 100.0f)
            all_plates_stats_item->slice_state = IMToolbarItem::SliceState::SLICING;

        for (auto toolbar_item : m_sel_plate_toolbar.m_items) {
            if(toolbar_item->slice_state == IMToolbarItem::SliceState::SLICE_FAILED) {
                all_plates_stats_item->slice_state = IMToolbarItem::SliceState::SLICE_FAILED;
                all_plates_stats_item->selected = false;
                break;
            }
        }

        // Changing parameters does not invalid all plates, need extra logic to validate
        bool gcode_result_valid = true;
        for (auto gcode_result : plate_list.get_nonempty_plates_slice_results()) {
            if (gcode_result->moves.size() == 0) {
                gcode_result_valid = false;
            }
        }
        if (all_plates_stats_item->selected && all_plates_stats_item->slice_state == IMToolbarItem::SliceState::SLICED && gcode_result_valid) {
            m_gcode_viewer.render_all_plates_stats(plate_list.get_nonempty_plates_slice_results());
            m_render_preview = false;
        }
        else{
            m_gcode_viewer.render_all_plates_stats(plate_list.get_nonempty_plates_slice_results(), false);
            m_render_preview = true;
        }
    }else
        m_render_preview = true;

    // places the toolbar on the top_left corner of the 3d scene
    float f_scale = get_scale();
    #ifdef WIN32
        const int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
        f_scale *= (float) dpi / (float) DPI_DEFAULT;
    #endif // WIN32
    Size cnv_size = get_canvas_size();
    auto canvas_w = float(cnv_size.get_width());
    auto canvas_h = float(cnv_size.get_height());

    bool is_hovered = false;

    m_sel_plate_toolbar.set_icon_size(100.0f * f_scale, 100.0f * f_scale);

    float button_width = m_sel_plate_toolbar.icon_width;
    float button_height = m_sel_plate_toolbar.icon_height;

    float frame_padding = 1.0f * f_scale;
    float margin_size = 4.0f * f_scale;
    float button_margin = frame_padding;

    const float y_offset = is_collapse_toolbar_on_left() ? (get_collapse_toolbar_height() + 5) : 0;
    // Make sure the window does not overlap the 3d navigator
    auto window_height_max = canvas_h - y_offset;
    if (wxGetApp().show_3d_navigator()) {
        window_height_max -= (128 * f_scale + 5);
    }

    // ORCA simplify and correct window size and margin calculations and get values from style
    ImGuiWrapper& imgui = *wxGetApp().imgui();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f) * f_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(4.0f, 4.0f) * f_scale);

    int item_count           = m_sel_plate_toolbar.m_items.size() + (m_sel_plate_toolbar.show_stats_item ? 1 : 0);
    float window_height_calc = (item_count * (button_height + (margin_size + button_margin) * 2.0f) + (item_count - 1) * ImGui::GetStyle().ItemSpacing.y + ImGui::GetStyle().WindowPadding.y * 2.0f);
    bool  show_scroll        = m_sel_plate_toolbar.is_display_scrollbar && (window_height_calc > window_height_max);
    float scrollbar_size     = 10.0f * f_scale ;
    float window_height      = std::min(window_height_calc, window_height_max);
    float window_width       = button_width + (margin_size + button_margin + ImGui::GetStyle().WindowPadding.x) * 2 + (show_scroll ? scrollbar_size : 0);

    ImVec4 window_bg     = m_is_dark ? ImVec4(.13f, .13f, .15f, .5f) : ImVec4(1.f, 1.f, 1.f, .7f);
    ImVec4 button_active = ImGuiWrapper::COL_ORCA; // ORCA: Use orca color for selected sliced plate border
    ImVec4 button_hover  = ImVec4(0.67f, 0.67f, 0.67, m_is_dark ? .6f : 1.0f);
    ImVec4 scroll_col    = ImVec4(0.77f, 0.77f, 0.77f, m_is_dark ? .6f : 1.0f);
    ImU32  plate_bg      = m_is_dark ? IM_COL32(255, 255, 255, 10) : IM_COL32(0, 0, 0, 10);
    ImU32  plate_dim     = m_is_dark ? IM_COL32(30, 30, 30, 100) : IM_COL32(0, 0, 0, 50);
    float  button_radius = 1.0f * f_scale;
    //ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.f, 0.f, 0.f, 1.0f));
    //use white text as the background switch to black
    ImGui::PushStyleColor(ImGuiCol_Text, m_is_dark ? ImVec4(.9f, .9f, .9f, 1) : ImVec4(.3f, .3f, .3f, 1)); // ORCA Plate number text > Add support for dark mode
    ImGui::PushStyleColor(ImGuiCol_WindowBg, window_bg);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0.f, 0.f, 0.f, 0.f)); // ORCA using background color with opacity creates a second color. This prevents secondary color
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, scroll_col);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, scroll_col);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, scroll_col);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, button_active);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, button_hover);

    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, scrollbar_size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, show_scroll ? (4.0f * f_scale) : (button_radius + margin_size + frame_padding + ImGui::GetStyle().WindowPadding.x));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, button_radius + margin_size);

    imgui.set_next_window_pos(canvas_w * 0 + 5.0f * f_scale, canvas_h * 0 + y_offset, ImGuiCond_Always, 0, 0); // ORCA Add slight gap on left edge so toolbar looks like floating and it creates separation with sidebar
    imgui.set_next_window_size(window_width, window_height, ImGuiCond_Always);

    if (show_scroll)
        imgui.begin(_L("Select Plate"), ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);
    else
        imgui.begin(_L("Select Plate"), ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);
    ImGui::SetWindowFontScale(1.2f);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f * f_scale);

    ImVec2 size = ImVec2(button_width, button_height); // Size of the image we want to make visible
    ImVec4 bg_col = ImVec4(128.0f, 128.0f, 128.0f, 0.0f);
    ImVec4 tint_col = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);               // No tint
    ImVec2 margin = ImVec2(margin_size, margin_size);

    if(m_sel_plate_toolbar.show_stats_item)
    {
        // draw image
        ImVec2 button_start_pos = ImGui::GetCursorScreenPos();

        if (all_plates_stats_item->selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, button_active);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, button_active);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, button_active);
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(128.0f, 128.0f, 128.0f, 0.0f));
            if (all_plates_stats_item->slice_state == IMToolbarItem::SliceState::SLICE_FAILED) {
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Button));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_Button));
            }
            else {
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, button_hover);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, button_hover);
            }
        }

        ImVec4 text_clr = m_is_dark ? ImVec4(.9f, .9f, .9f, 1) : ImVec4(.3f, .3f, .3f, 1);;
        ImTextureID btn_texture_id;
        if (all_plates_stats_item->slice_state == IMToolbarItem::SliceState::UNSLICED || all_plates_stats_item->slice_state == IMToolbarItem::SliceState::SLICING){
            btn_texture_id = (ImTextureID)(intptr_t)(m_process->running() ? all_plates_stats_item->image_slicing.get_id() : all_plates_stats_item->image_idle.get_id());
        } else if (all_plates_stats_item->slice_state == IMToolbarItem::SliceState::SLICE_FAILED) {
            btn_texture_id = (ImTextureID)(intptr_t)(all_plates_stats_item->image_failed.get_id());
        } else {
            btn_texture_id = (ImTextureID)(intptr_t)(all_plates_stats_item->image_stats.get_id());
        }

        if (ImGui::ImageButton2(btn_texture_id, size, {0,0}, {1,1}, frame_padding, bg_col, tint_col, margin)) {
            if (all_plates_stats_item->slice_state != IMToolbarItem::SliceState::SLICE_FAILED) {
                if (m_process && !m_process->running()) {
                    for (int i = 0; i < m_sel_plate_toolbar.m_items.size(); i++) {
                        m_sel_plate_toolbar.m_items[i]->selected = false;
                    }
                    all_plates_stats_item->selected = true;
                    wxGetApp().plater()->update(true, true);
                    wxCommandEvent evt = wxCommandEvent(EVT_GLTOOLBAR_SLICE_ALL);
                    wxPostEvent(wxGetApp().plater(), evt);
                }
            }
        }

        ImGui::PopStyleColor(3);

        ImVec2 start_pos = ImVec2(button_start_pos.x + frame_padding + margin.x, button_start_pos.y + frame_padding + margin.y);

        // ORCA add more useful information about slicing process and use progress bar on stats button
        std::string text_top;
        std::string text_bottom;
        ImVec2 bar_size         = ImVec2(size.x - margin.x * 2, 5.0f * f_scale);
        ImVec2 bar_bg_bgn       = start_pos + ImVec2(margin.x, size.y - bar_size.y - margin.y);
        ImVec2 bar_bg_end       = bar_bg_bgn + bar_size;
        int    total_plates_cnt = plate_list.get_nonempty_plate_list().size();
        ImGui::GetWindowDrawList()->AddRectFilled(start_pos, start_pos + size, plate_bg, button_radius); // Button background

        if (all_plates_stats_item->slice_state == IMToolbarItem::SliceState::UNSLICED || all_plates_stats_item->slice_state == IMToolbarItem::SliceState::SLICING) {
            float bar_current_perc_pos  = 0;
            float bar_total_perc_pos    = all_plates_stats_item->percent * (bar_size.x / 100.0f);
            int   current_slicing_plate = 0;
            if (m_process->running()) {
                for (int i = 0; i < total_plates_cnt; i++) {
                    if (m_sel_plate_toolbar.m_items[i]->slice_state == IMToolbarItem::SliceState::SLICING) {
                        current_slicing_plate = i;
                        bar_current_perc_pos  = m_sel_plate_toolbar.m_items[i]->percent * (bar_size.x / total_plates_cnt / 100.0f);
                    }
                }
                text_top = _u8L("Slicing") + ": " + std::to_string(current_slicing_plate + 1);
            } else {
                text_top = _u8L("Slice all");
            }
            text_bottom = std::to_string(sliced_plates_cnt) + " / " + std::to_string(total_plates_cnt);
            ImVec2 bar_fg_end = ImVec2(bar_bg_bgn.x + bar_total_perc_pos + bar_current_perc_pos, bar_bg_end.y);
            ImGui::GetWindowDrawList()->AddRectFilled(bar_bg_bgn, bar_bg_end, IM_COL32(128, 128, 128, 255), bar_size.y); // BAR BACKGROUND
            ImGui::GetWindowDrawList()->AddRectFilled(bar_bg_bgn, bar_fg_end, IM_COL32(0, 150, 136, 255),   bar_size.y); // BAR PROGRESS
        }
        else if (all_plates_stats_item->slice_state == IMToolbarItem::SliceState::SLICE_FAILED) {
            text_top    = _u8L("Failed");
            text_bottom = std::to_string(sliced_plates_cnt) + " / " + std::to_string(total_plates_cnt);
            ImGui::GetWindowDrawList()->AddRectFilled(bar_bg_bgn, bar_bg_end, IM_COL32(225, 74, 74, 255),   bar_size.y);
        }
        else if (all_plates_stats_item->slice_state == IMToolbarItem::SliceState::SLICED) {
            text_top    = _u8L("All Plates");
            text_bottom = _u8L("Stats");
        }

        // draw text
        GImGui->FontSize = 15.0f * f_scale;
        ImGui::PushStyleColor(ImGuiCol_Text, text_clr);
        ImVec2 text_size = ImGui::CalcTextSize(text_top.c_str());
        ImVec2 text_start_pos = ImVec2(start_pos.x + (button_width - text_size.x) / 2, start_pos.y + 3.0f * button_height / 5.0f);
        ImGui::RenderText(text_start_pos, text_top.c_str());
        text_size = ImGui::CalcTextSize(text_bottom.c_str());
        text_start_pos = ImVec2(start_pos.x + (button_width - text_size.x) / 2, text_start_pos.y + ImGui::GetTextLineHeight());
        ImGui::RenderText(text_start_pos, text_bottom.c_str());
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.2f);
    }

    for (int i = 0; i < m_sel_plate_toolbar.m_items.size(); i++) {
        IMToolbarItem* item = m_sel_plate_toolbar.m_items[i];

        // draw image
        ImVec2 button_start_pos = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        ImVec2 uv0 = ImVec2(0.0f, 1.0f);    // UV coordinates for lower-left
        ImVec2 uv1 = ImVec2(1.0f, 0.0f);    // UV coordinates in our texture

        auto button_pos = ImGui::GetCursorPos();
        ImGui::SetCursorPos(button_pos + margin);

        ImGui::Image(item->texture_id, size, uv0, uv1, tint_col);

        ImGui::SetCursorPos(button_pos);

        // invisible button
        auto button_size = size + margin + margin + ImVec2(2 * frame_padding, 2 * frame_padding);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f * f_scale);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.0f, .0f, .0f, .0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.0f, .0f, .0f, .0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.0f, .0f, .0f, .0f));
        if (item->selected) {
            ImGui::PushStyleColor(ImGuiCol_Border, button_active);
        }
        else {
            // Translate window pos to abs pos, also account for the window scrolling
            auto hover_rect = button_pos + ImGui::GetWindowPos() - ImGui::GetCurrentWindow()->Scroll;
            if (ImGui::IsMouseHoveringRect(hover_rect, hover_rect + button_size)) {
                ImGui::PushStyleColor(ImGuiCol_Border, button_hover);
            }
            else {
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(.0f, .0f, .0f, .0f));
            }
        }
        if(ImGui::Button("##invisible_button", button_size)){
            if (m_process && !m_process->running()) {
                all_plates_stats_item->selected = false;
                item->selected = true;
                // begin to slicing plate
                if (item->slice_state != IMToolbarItem::SliceState::SLICED)
                    wxGetApp().plater()->update(true, true);
                wxCommandEvent* evt = new wxCommandEvent(EVT_GLTOOLBAR_SELECT_SLICED_PLATE);
                evt->SetInt(i);
                wxQueueEvent(wxGetApp().plater(), evt);
            }
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();

        ImVec2 start_pos = ImVec2(button_start_pos.x + frame_padding + margin.x, button_start_pos.y + frame_padding + margin.y);
        if (item->slice_state == IMToolbarItem::SliceState::UNSLICED) {
            ImVec2 size = ImVec2(button_width, button_height);
            ImVec2 end_pos = ImVec2(start_pos.x + size.x, start_pos.y + size.y);
            ImGui::GetWindowDrawList()->AddRectFilled(start_pos, end_pos, plate_dim, button_radius);
        } else if (item->slice_state == IMToolbarItem::SliceState::SLICING) {
            ImVec2 size = ImVec2(button_width, button_height * item->percent / 100.0f);
            ImVec2 rect_start_pos = ImVec2(start_pos.x, start_pos.y + size.y);
            ImVec2 rect_end_pos = ImVec2(start_pos.x + button_width, start_pos.y + button_height);
            ImGui::GetWindowDrawList()->AddRectFilled(start_pos, rect_end_pos, plate_bg, button_radius);
            ImGui::GetWindowDrawList()->AddRectFilled(rect_start_pos, rect_end_pos, plate_dim, button_radius);
        } else if (item->slice_state == IMToolbarItem::SliceState::SLICE_FAILED) {
            // Draw exclamation mark that matches with icon
            ImVec2 end_pos = ImVec2(start_pos.x + button_width, start_pos.y + button_height);
            ImVec2 center  = ImVec2(start_pos.x + button_width/2, start_pos.y + button_height/2);
            auto draw_list =ImGui::GetWindowDrawList();
            auto clr = m_is_dark ? IM_COL32(60, 44, 48, 255) : IM_COL32(202, 186, 186, 255);
            draw_list->AddRectFilled(start_pos, end_pos, IM_COL32(64, 1, 1, 64), button_radius);
            draw_list->AddCircleFilled(center, 14.f * f_scale,IM_COL32(225, 74, 74, 255));
            draw_list->AddRectFilled(center - ImVec2(2.f, 10.f) * f_scale, center + ImVec2(2.f, 4.f) * f_scale, clr, 2.f * f_scale);
            draw_list->AddCircleFilled(center + ImVec2(0, 8.f * f_scale),2.f * f_scale, clr);
        } else if (item->slice_state == IMToolbarItem::SliceState::SLICED) {
            ImVec2 size = ImVec2(button_width, button_height);
            ImVec2 end_pos = ImVec2(start_pos.x + size.x, start_pos.y + size.y);
            ImGui::GetWindowDrawList()->AddRectFilled(start_pos, end_pos, plate_bg, button_radius);
        }

        // draw text
        GImGui->FontSize = 18.0f * f_scale; // ORCA fix font scaling
        ImVec2 text_start_pos = ImVec2(start_pos.x + 4.0f * f_scale, start_pos.y + 2.0f * f_scale); // ORCA move close to corner to prevent overlapping with preview
        ImGui::RenderText(text_start_pos, std::to_string(i + 1).c_str());
        ImGui::SetWindowFontScale(1.2f); // ORCA fix font scaling

        ImGui::PopID();
    }
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor(8);
    ImGui::PopStyleVar(7);

    if (ImGui::IsWindowHovered() || is_hovered) {
        m_sel_plate_toolbar.is_display_scrollbar = true;
    } else {
        m_sel_plate_toolbar.is_display_scrollbar = false;
    }

    imgui.end();
    m_sel_plate_toolbar.is_render_finish = true;
}

//BBS: GUI refactor: GLToolbar adjust
//when rendering, {0, 0} is at the center, {-0.5, 0.5} at the left-up
void GLCanvas3D::_render_assemble_view_toolbar() const
{
    if (!m_assemble_view_toolbar.is_enabled())
        return;

    const Size cnv_size = get_canvas_size();
    const float gizmo_width = m_gizmos.get_scaled_total_width();
    const float separator_width = m_separator_toolbar.get_width();
    const float top = 0.5f * (float)cnv_size.get_height();
    const float main_toolbar_left = -0.5f * cnv_size.get_width() + get_main_toolbar_offset();
    const float left = main_toolbar_left + (m_main_toolbar.get_width() + gizmo_width + separator_width);

    m_assemble_view_toolbar.set_position(top, left);
    m_assemble_view_toolbar.render(*this);
}

void GLCanvas3D::_render_return_toolbar() const
{
    if (!m_return_toolbar.is_enabled())
        return;

    float font_size = ImGui::GetFontSize();
    ImVec2 real_size = ImVec2(font_size * 4, font_size * 1.7);
    ImVec2 button_icon_size = ImVec2(font_size * .55, font_size * 1.1); // ORCA

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    Size cnv_size = get_canvas_size();
    auto canvas_w = float(cnv_size.get_width());
    auto canvas_h = float(cnv_size.get_height());
    float window_width = real_size.x + button_icon_size.x + imgui.scaled(2.0f);
    float window_height = button_icon_size.y + imgui.scaled(2.0f);
    float window_pos_x = 30.0f + (is_collapse_toolbar_on_left() ? (get_collapse_toolbar_width() + 5.f) : 0);
    float window_pos_y = 14.0f;

    imgui.set_next_window_pos(window_pos_x, window_pos_y, ImGuiCond_Always, 0, 0);
#ifdef __WINDOWS__
    imgui.set_next_window_size(window_width, window_height, ImGuiCond_Always);
#endif

    ImVec4 fg_color = m_is_dark ? ImVec4(1.f, 1.f, 1.f, 0.88f) : ImVec4(50 / 255.f, 58 / 255.f, 61 / 255.f, 1.f);
    ImVec4 bg_color = m_is_dark ? ImGuiWrapper::COL_TOOLBAR_BG_DARK : ImGuiWrapper::COL_TOOLBAR_BG;
    ImVec4 bg_color_hover = bg_color +  (m_is_dark ? ImVec4(.03f, .03f, .03f, 0) : ImVec4(-.02f, -.02f, -.02f, 0));

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 18.0f);
    ImGui::PushStyleColor(ImGuiCol_Button,        bg_color); // ORCA Toolbar color
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg_color_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  bg_color);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, fg_color);

    imgui.begin(_L("Assembly Return"), ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground
        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);

    float button_width = 20;
    float button_height = 20;
    ImVec2 size = ImVec2(button_width, button_height); // Size of the image we want to make visible
    ImVec2 uv0 = ImVec2(0.0f, 0.0f);
    ImVec2 uv1 = ImVec2(1.0f, 1.0f);

    ImVec4 bg_col = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    ImVec4 tint_col = fg_color; // icon color
    ImVec2 margin = ImVec2(10.0f, 5.0f);

    if (ImGui::ImageTextButton(real_size,_utf8(L("Return")).c_str(), m_return_toolbar.get_return_texture_id(), button_icon_size, uv0, uv1, -1, bg_col, tint_col, margin)) {
        if (m_canvas != nullptr)
            wxPostEvent(m_canvas, SimpleEvent(EVT_GLVIEWTOOLBAR_3D));
        const_cast<GLGizmosManager*>(&m_gizmos)->reset_all_states();
        wxGetApp().plater()->get_view3D_canvas3D()->get_gizmos_manager().reset_all_states();
        wxGetApp().plater()->get_view3D_canvas3D()->reload_scene(true);
    }
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(1);

    imgui.end();
}

void GLCanvas3D::_render_canvas_toolbar() 
{
    ImGuiWrapper &imgui    = *wxGetApp().imgui();
    float         sc       = get_scale();

    #ifdef WIN32
        const int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
        sc *= (float) dpi / (float) DPI_DEFAULT;
    #endif // WIN32

    ImVec2        btn_size = ImVec2(36.f, 36.f) * sc;
    ImVec2        margin   = ImVec2(m_canvas_toolbar_pos[0] > 0 ? 0.f : (10.f * sc), 10.f * sc);
    ImVec2        spacing  = ImVec2(6.f, 6.f)  * sc;
    ImVec2        padding  = ImVec2(2.f, 2.f)  * sc;
    Vec2i32       pos      = {
        m_canvas_toolbar_pos[0]        + margin.x,
        get_canvas_size().get_height() - margin.y
    };
    bool          zoom_btn = wxGetApp().show_canvas_zoom_button();

    imgui.set_next_window_pos(pos[0], pos[1], ImGuiCond_Always, 0, 1); // pivot bottom-left

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0    );
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding   , {0,0});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing    , {0,0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding  , padding); // without padding images clipping

    imgui.begin(_L("Canvas Toolbar"), ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);//

    ImTextureID m_normal_id = m_gizmos.get_icon_texture_id(m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_CANVAS_MENU_DARK       : GLGizmosManager::MENU_ICON_NAME::IC_CANVAS_MENU);
    ImTextureID m_hover_id  = m_gizmos.get_icon_texture_id(m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_CANVAS_MENU_DARK_HOVER : GLGizmosManager::MENU_ICON_NAME::IC_CANVAS_MENU_HOVER);

    if (ImGui::ImageButton3(m_normal_id, m_hover_id, btn_size)) {
        if(!ImGui::IsPopupOpen("CanvasToolbarMenu")){
            ImGui::SetNextWindowPos(ImVec2(pos[0] + padding.x, pos[1] - padding.y - (zoom_btn ? (btn_size.y + spacing.y) : 0.f)), ImGuiCond_Always, ImVec2(0, 1)); // pivot bottom-left
            ImGui::OpenPopup("CanvasToolbarMenu");
        }
    }
 
    if(zoom_btn){
        ImGui::Dummy({ 0, spacing.y});

        ImTextureID z_normal_id = m_gizmos.get_icon_texture_id(m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_CANVAS_ZOOM_DARK       : GLGizmosManager::MENU_ICON_NAME::IC_CANVAS_ZOOM);
        ImTextureID z_hover_id  = m_gizmos.get_icon_texture_id(m_is_dark ? GLGizmosManager::MENU_ICON_NAME::IC_CANVAS_ZOOM_DARK_HOVER : GLGizmosManager::MENU_ICON_NAME::IC_CANVAS_ZOOM_HOVER);

        if (ImGui::ImageButton3(z_normal_id, z_hover_id, btn_size)) {
            select_view("plate");
            if (m_selection.is_empty()) {
                if (m_canvas_type == ECanvasType::CanvasAssembleView)
                    zoom_to_volumes();
                else 
                    zoom_to_bed();
            } else {
                zoom_to_selection();
            }
        } else if (ImGui::IsItemHovered()) {
            auto tooltip = _L("Fit camera to scene or selected object.");
            auto width   = ImGui::CalcTextSize(tooltip.c_str()).x + imgui.scaled(2.0f);
            imgui.tooltip(tooltip, width);
        }
    }

    ImGui::PopStyleVar(4); // Window

    ImGui::PushStyleColor(ImGuiCol_PopupBg           , m_is_dark ? ImGuiWrapper::COL_TOOLBAR_BG_DARK : ImGuiWrapper::COL_TOOLBAR_BG);
    ImGui::PushStyleColor(ImGuiCol_Separator         , m_is_dark ? ImVec4(1, 1, 1, .20f) : ImVec4(0, 0, 0, .2f));
    ImGui::PushStyleColor(ImGuiCol_Text              , m_is_dark ? ImVec4(1, 1, 1, .88f) : ImVec4(50 / 255.f, 58 / 255.f, 61 / 255.f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled      , m_is_dark ? ImVec4(1, 1, 1, .44f) : ImVec4(50 / 255.f, 58 / 255.f, 61 / 255.f, .5f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered     , ImVec4(0, 0, 0, 0.f)); // bg color for menu item
    ImGui::PushStyleColor(ImGuiCol_BorderActive      , ImGuiWrapper::COL_ORCA);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.f     );
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding  , 8.f * sc);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f * sc);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding  , 2.f * sc);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding  , ImVec2(4.f, 10.f) * sc);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing    , ImVec2(0.f, 8.f ) * sc);

    if (ImGui::BeginPopup("CanvasToolbarMenu")) {
        ImGui::PushItemFlag(ImGuiItemFlags_SelectableDontClosePopup, true);

        Plater*    p   = wxGetApp().plater();
        AppConfig* cfg = wxGetApp().app_config;

        auto create_menu_item = [this, sc](
            const std::string& name,
            bool enable,
            bool condition,
            const std::function<void()>& action
        ) {
            ImGui::Dummy({2.f * sc,0});
            ImGui::SameLine();
            if (ImGui::BBLMenuItem(("        " + _u8L(name)).c_str(), nullptr, false, enable, ImGui::CalcTextSize(_u8L(name).c_str()).y))
                action();
            ImGui::SameLine(12.f * sc);
            ImGui::TextColored(enable ? ImVec4(1,1,1,1) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s", into_u8(condition ? ImGui::VisibleIcon : ImGui::HiddenIcon).c_str());
        };

        create_menu_item( "3D Navigator",
            m_canvas_type != ECanvasType::CanvasAssembleView, // not work on assembly
            wxGetApp().show_3d_navigator(),
            [this]{
                wxGetApp().toggle_show_3d_navigator();
                ImGui::CloseCurrentPopup(); // Close popup to show changes on UI
            }
        );

        create_menu_item( "Zoom button",
            true, // work on all
            wxGetApp().show_canvas_zoom_button(),
            [this]{
                wxGetApp().toggle_canvas_zoom_button();
                ImGui::CloseCurrentPopup(); // Close popup to show changes on UI
            }
        );

        ImGui::Separator();

        create_menu_item( "Overhangs",
            m_canvas_type == ECanvasType::CanvasView3D, // work only on prepare
            p->is_view3D_overhang_shown(),
            [this, p]{p->show_view3D_overhang(!p->is_view3D_overhang_shown());}
        );

        create_menu_item( "Outline",
            m_canvas_type != ECanvasType::CanvasPreview, // not work on preview
            wxGetApp().show_outline(),
            [this]{wxGetApp().toggle_show_outline();}
        );

        ImGui::Separator();

        create_menu_item( "Perspective",
            true, // work on all
            cfg->get_bool("use_perspective_camera"),
            [this, &cfg]{
                cfg->set_bool("use_perspective_camera", !(cfg->get_bool("use_perspective_camera")));
                wxGetApp().update_ui_from_settings();
            }
        );

        ImGui::Separator();

        create_menu_item( "Axes",
            m_canvas_type != ECanvasType::CanvasAssembleView, // not work on assembly
            m_show_world_axes,
            [this]{toggle_world_axes_visibility(false);}
        );

        // will add an option for gridlines in here

        create_menu_item( "Labels",
            m_canvas_type == ECanvasType::CanvasView3D, // work only on prepare
            p->are_view3D_labels_shown(),
            [this, p]{p->show_view3D_labels(!p->are_view3D_labels_shown());}
        );

        ImGui::PopItemFlag();
        ImGui::EndPopup();
    }

    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(6);

    imgui.end();
}

void GLCanvas3D::_render_separator_toolbar_right() const
{
    if (!m_separator_toolbar.is_enabled())
        return;

    const Size cnv_size = get_canvas_size();
    const float gizmo_width = m_gizmos.get_scaled_total_width();
    const float separator_width = m_separator_toolbar.get_width();
    const float top = 0.5f * (float)cnv_size.get_height();
    const float main_toolbar_left = -0.5f * cnv_size.get_width() + get_main_toolbar_offset();
    const float left = main_toolbar_left + (m_main_toolbar.get_width() + gizmo_width + separator_width / 2);

    m_separator_toolbar.set_position(top, left);
    m_separator_toolbar.render(*this,GLToolbarItem::SeparatorLine);
}

void GLCanvas3D::_render_separator_toolbar_left() const
{
    if (!m_separator_toolbar.is_enabled())
        return;

    const Size cnv_size = get_canvas_size();
    const float top = 0.5f * (float)cnv_size.get_height();
    const float main_toolbar_left = -0.5f * cnv_size.get_width() + get_main_toolbar_offset();
    const float left = main_toolbar_left + (m_main_toolbar.get_width());

    m_separator_toolbar.set_position(top, left);
    m_separator_toolbar.render(*this,GLToolbarItem::SeparatorLine);
}

void GLCanvas3D::_render_collapse_toolbar() const
{
    auto&      plater              = *wxGetApp().plater();
    const auto sidebar_docking_dir = plater.get_sidebar_docking_state();
    if (sidebar_docking_dir == Sidebar::None) {
        return;
    }

    GLToolbar& collapse_toolbar = plater.get_collapse_toolbar();

    const Size cnv_size = get_canvas_size();
    const float top  = 0.5f * (float)cnv_size.get_height();
    const float left = sidebar_docking_dir == Sidebar::Right ? 0.5f * (float) cnv_size.get_width() - (float) collapse_toolbar.get_width() :
                                                               -0.5f * (float) cnv_size.get_width();

    collapse_toolbar.set_position(top, left);
    collapse_toolbar.render(*this);
}

//BBS reander assemble toolbar
void GLCanvas3D::_render_paint_toolbar() const
{
    if (m_canvas_type != ECanvasType::CanvasAssembleView)
        return;
#if ENABLE_RETINA_GL
    float f_scale = m_retina_helper->get_scale_factor();
#else
    float f_scale = 1.0f;
#endif
    int em_unit = wxGetApp().em_unit() / 10;

    std::vector<std::string> colors = wxGetApp().plater()->get_extruder_colors_from_plater_config();
    int extruder_num = colors.size();
    std::vector<std::string> filament_text_first_line;
    std::vector<std::string> filament_text_second_line;
    {
        auto preset_bundle = wxGetApp().preset_bundle;
        for (auto filament_name : preset_bundle->filament_presets) {
            for (auto iter = preset_bundle->filaments.lbegin(); iter != preset_bundle->filaments.end(); iter++) {
                if (filament_name.compare(iter->name) == 0) {
                    std::string display_filament_type;
                    iter->config.get_filament_type(display_filament_type);
                    auto pos = display_filament_type.find(' ');
                    if (pos != std::string::npos) {
                        filament_text_first_line.push_back(display_filament_type.substr(0, pos));
                        filament_text_second_line.push_back(display_filament_type.substr(pos + 1));
                    }
                    else {
                        filament_text_first_line.push_back(display_filament_type);
                        filament_text_second_line.push_back("");
                    }
                }
            }
        }
    }

    ImGuiWrapper& imgui = *wxGetApp().imgui();
    const float canvas_w = float(get_canvas_size().get_width());
    const ImVec2 button_size = ImVec2(64.0f, 48.0f) * f_scale * em_unit;
    const float spacing = 4.0f * em_unit * f_scale;
    const float return_button_margin = 130.0f * em_unit * f_scale;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(spacing, spacing));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, m_is_dark ? ImGuiWrapper::COL_TOOLBAR_BG_DARK : ImGuiWrapper::COL_TOOLBAR_BG); // ORCA Toolbar color

    imgui.set_next_window_pos(0.5f * canvas_w, 0, ImGuiCond_Always, 0.5f, 0.0f);
    float constraint_window_width = canvas_w - 2 * return_button_margin;
    ImGui::SetNextWindowSizeConstraints({ 0, 0 }, { constraint_window_width, FLT_MAX });
    imgui.begin(_L("Paint Toolbar"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const float cursor_y = ImGui::GetCursorPosY();
    const ImVec2 arrow_button_size = ImVec2(0.375f * button_size.x, ImGui::GetWindowHeight());
    const ImRect left_arrow_button = ImRect(ImGui::GetCurrentWindow()->Pos, ImGui::GetCurrentWindow()->Pos + arrow_button_size);
    const ImRect right_arrow_button = ImRect(ImGui::GetCurrentWindow()->Pos + ImGui::GetWindowSize() - arrow_button_size, ImGui::GetCurrentWindow()->Pos + ImGui::GetWindowSize());
    ImU32 left_arrow_button_color = IM_COL32(0, 0, 0, 0.4f * 255);
    ImU32 right_arrow_button_color = IM_COL32(0, 0, 0, 0.4f * 255);
    ImU32 arrow_color = IM_COL32(255, 255, 255, 255);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImGuiContext& context = *GImGui;
    bool disabled = !wxGetApp().plater()->can_fillcolor();
    ColorRGBA rgba;

    for (int i = 0; i < extruder_num; i++) {
        if (i > 0)
            ImGui::SameLine();
        decode_color(colors[i], rgba);
        ImGui::PushStyleColor(ImGuiCol_Button, ImGuiWrapper::to_ImVec4(rgba));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGuiWrapper::to_ImVec4(rgba));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGuiWrapper::to_ImVec4(rgba));
        if (disabled)
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        if (ImGui::Button(("##filament_button" + std::to_string(i)).c_str(), button_size)) {
            if (!ImGui::IsMouseHoveringRect(left_arrow_button.Min, left_arrow_button.Max) && !ImGui::IsMouseHoveringRect(right_arrow_button.Min, right_arrow_button.Max))
                wxPostEvent(m_canvas, IntEvent(EVT_GLTOOLBAR_FILLCOLOR, i + 1));
        }
        if (ImGui::IsItemHovered() && i < 9) {
            if (!ImGui::IsMouseHoveringRect(left_arrow_button.Min, left_arrow_button.Max) && !ImGui::IsMouseHoveringRect(right_arrow_button.Min, right_arrow_button.Max)) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 20.0f * f_scale, 10.0f * f_scale });
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f * f_scale);
                imgui.tooltip(_L("Shortcut Key ") + std::to_string(i + 1), ImGui::GetFontSize() * 20.0f);
                ImGui::PopStyleVar(2);
            }
        }
        ImGui::PopStyleColor(3);
        if (disabled)
            ImGui::PopItemFlag();
    }

    const float text_offset_y = 4.0f * em_unit * f_scale;
    for (int i = 0; i < extruder_num; i++) {
        decode_color(colors[i], rgba);
        float  gray       = 0.299 * rgba.r_uchar() + 0.587 * rgba.g_uchar() + 0.114 * rgba.b_uchar();
        ImVec4 text_color = gray < 80 ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(0, 0, 0, 1.0f);

        imgui.push_bold_font();
        ImVec2 number_label_size = ImGui::CalcTextSize(std::to_string(i + 1).c_str());
        ImGui::SetCursorPosY(cursor_y + text_offset_y);
        ImGui::SetCursorPosX(spacing + i * (spacing + button_size.x) + (button_size.x - number_label_size.x) / 2);
        ImGui::TextColored(text_color, std::to_string(i + 1).c_str());
        imgui.pop_bold_font();

        ImVec2 filament_first_line_label_size = ImGui::CalcTextSize(filament_text_first_line[i].c_str());
        ImGui::SetCursorPosY(cursor_y + text_offset_y + number_label_size.y);
        ImGui::SetCursorPosX(spacing + i * (spacing + button_size.x) + (button_size.x - filament_first_line_label_size.x) / 2);
        ImGui::TextColored(text_color, filament_text_first_line[i].c_str());

        ImVec2 filament_second_line_label_size = ImGui::CalcTextSize(filament_text_second_line[i].c_str());
        ImGui::SetCursorPosY(cursor_y + text_offset_y + number_label_size.y + filament_first_line_label_size.y);
        ImGui::SetCursorPosX(spacing + i * (spacing + button_size.x) + (button_size.x - filament_second_line_label_size.x) / 2);
        ImGui::TextColored(text_color, filament_text_second_line[i].c_str());
    }

    if (ImGui::GetWindowWidth() == constraint_window_width) {
        if (ImGui::IsMouseHoveringRect(left_arrow_button.Min, left_arrow_button.Max)) {
            left_arrow_button_color = IM_COL32(0, 0, 0, 0.64f * 255);
            if (context.IO.MouseClicked[ImGuiMouseButton_Left]) {
                ImGui::SetScrollX(ImGui::GetScrollX() - button_size.x);
                imgui.set_requires_extra_frame();
            }
        }
        draw_list->AddRectFilled(left_arrow_button.Min, left_arrow_button.Max, left_arrow_button_color);
        ImGui::BBLRenderArrow(draw_list, left_arrow_button.GetCenter() - ImVec2(draw_list->_Data->FontSize, draw_list->_Data->FontSize) * 0.5f, arrow_color, ImGuiDir_Left, 2.0f);

        if (ImGui::IsMouseHoveringRect(right_arrow_button.Min, right_arrow_button.Max)) {
            right_arrow_button_color = IM_COL32(0, 0, 0, 0.64f * 255);
            if (context.IO.MouseClicked[ImGuiMouseButton_Left]) {
                ImGui::SetScrollX(ImGui::GetScrollX() + button_size.x);
                imgui.set_requires_extra_frame();
            }
        }
        draw_list->AddRectFilled(right_arrow_button.Min, right_arrow_button.Max, right_arrow_button_color);
        ImGui::BBLRenderArrow(draw_list, right_arrow_button.GetCenter() - ImVec2(draw_list->_Data->FontSize, draw_list->_Data->FontSize) * 0.5f, arrow_color, ImGuiDir_Right, 2.0f);
    }

    m_paint_toolbar_width = (ImGui::GetWindowWidth() + 50.0f * em_unit * f_scale);
    imgui.end();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

float GLCanvas3D::_show_assembly_tooltip_information(float caption_max, float x, float y) const
{
    ImGuiWrapper *imgui     = wxGetApp().imgui();
    ImTextureID normal_id = m_gizmos.get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_gizmos.get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += imgui->calc_text_size(": "sv).x + 35.f;

    float  scale       = get_scale();
    #ifdef WIN32
        int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
        scale *= (float) dpi / (float) DPI_DEFAULT;
    #endif // WIN32
    ImVec2 button_size = ImVec2(25 * scale, 25 * scale); // ORCA: Use exact resolution will prevent blur on icon

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, ImGui::GetStyle().FramePadding.y});
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [&imgui, & caption_max](const wxString &caption, const wxString &text) {
            imgui->text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };

        for (const auto &t : std::array<std::string, 3>{"object_selection", "part_selection", "number_key"}) {
            draw_text_with_caption(m_assembly_view_desc.at(t + "_caption") + ": ", m_assembly_view_desc.at(t));
        }
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
    auto same_line_size = button_size.x * 1.8;//with an space size
    ImGui::SameLine(same_line_size);
    same_line_size = imgui->calc_text_size("|"sv).x + same_line_size + imgui->calc_text_size("  "sv).x;
    imgui->text_colored(ImGuiWrapper::COL_ACTIVE, "|");
    ImGui::SameLine(same_line_size);
    return same_line_size;
}

//BBS
void GLCanvas3D::_render_assemble_control()
{
    if (m_canvas_type != ECanvasType::CanvasAssembleView) {
        GLVolume::explosion_ratio = m_explosion_ratio = 1.0;
        return;
    }
    if (m_gizmos.get_current_type() == GLGizmosManager::EType::MmSegmentation) {
        m_gizmos.m_assemble_view_data->model_objects_clipper()->set_position(0.0, true);
        return;
    }

    ImGuiWrapper* imgui = wxGetApp().imgui();

    ImGuiWrapper::push_toolbar_style(get_scale());

    auto canvas_w = float(get_canvas_size().get_width());
    auto canvas_h = float(get_canvas_size().get_height());

    const float text_padding = 7.0f;
    const float text_size_x = std::max(imgui->calc_text_size(_L("Reset direction")).x + 2 * ImGui::GetStyle().FramePadding.x,
        std::max(imgui->calc_text_size(_L("Explosion Ratio")).x, imgui->calc_text_size(_L("Section View")).x));
    const float slider_width = 60.0f;
    const float value_size = imgui->calc_text_size("3.00"sv).x + text_padding * 2;
    const float item_spacing = imgui->get_item_spacing().x;
    ImVec2 window_padding = ImGui::GetStyle().WindowPadding;

    imgui->set_next_window_pos(canvas_w / 2, canvas_h - 10.0f * get_scale(), ImGuiCond_Always, 0.5f, 1.0f);
    imgui->begin(_L("Assemble Control"), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    ImGui::AlignTextToFramePadding();
    float tip_icon_size;
    {
        float caption_max = 0.f;
        for (const auto &t : std::array<std::string, 3>{"object_selection", "part_selection", "number_key"}) {
            caption_max = std::max(caption_max, imgui->calc_text_size(m_assembly_view_desc.at(t + "_caption")).x);
        }
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float text_y =imgui->calc_text_size(_L("part selection")).y;
        float get_cur_x = pos.x;
        float get_cur_y = pos.y - ImGui::GetFrameHeight() - 4 * text_y;
        tip_icon_size =_show_assembly_tooltip_information(caption_max, get_cur_x, get_cur_y);
    }
    float same_line_width = tip_icon_size;
    {
        float clp_dist = m_gizmos.m_assemble_view_data->model_objects_clipper()->get_position();
        if (clp_dist == 0.f) {
            ImGui::AlignTextToFramePadding();
            imgui->text(_L("Section View"));
        }
        else {
            if (imgui->button(_L("Reset direction"))) {
                wxGetApp().CallAfter([this]() {
                    m_gizmos.m_assemble_view_data->model_objects_clipper()->set_position(-1., false);
                    });
            }
        }
        same_line_width += (text_size_x + item_spacing);
        ImGui::SameLine(same_line_width);
        ImGui::PushItemWidth(slider_width);
        bool view_slider_changed = imgui->bbl_slider_float_style("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true);

        same_line_width += (slider_width + item_spacing);
        ImGui::SameLine(same_line_width);
        ImGui::PushItemWidth(value_size);
        bool view_input_changed = ImGui::BBLDragFloat("##clp_dist_input", &clp_dist, 0.05f, 0.0f, 0.0f, "%.2f");

        if (view_slider_changed || view_input_changed)
            m_gizmos.m_assemble_view_data->model_objects_clipper()->set_position(clp_dist, true);

        same_line_width += (value_size + item_spacing * 2);
    }
    {
        auto temp_x = imgui->calc_text_size(_L("Explosion Ratio")).x;
        ImGui::SameLine(same_line_width);
        ImGui::PushItemWidth(temp_x);
        imgui->text(_L("Explosion Ratio"));

        same_line_width +=   (temp_x + item_spacing);
        ImGui::SameLine(same_line_width);
        ImGui::PushItemWidth(slider_width);
        bool explosion_slider_changed = imgui->bbl_slider_float_style("##ratio_slider", &m_explosion_ratio, 1.0f, 3.0f, "%1.2f");

        same_line_width +=  (slider_width + item_spacing);
        ImGui::SameLine(same_line_width);
        ImGui::PushItemWidth(value_size);
        bool explosion_input_changed = ImGui::BBLDragFloat("##ratio_input", &m_explosion_ratio, 0.1f, 1.0f, 3.0f, "%1.2f");
        same_line_width    +=   (value_size + item_spacing*2);
    }
    {
        ImGui::SameLine(same_line_width);
        // input
        std::vector<std::string> modes = {_u8L("Object"), _u8L("Part")};
        int selection_idx = m_selection.get_volume_selection_mode() == Selection::Instance ? 0 : 1;
        auto label         = _u8L("Selection Mode") + ":" ;
        auto label_width   = imgui->calc_text_size(label).x ;
        auto item_width    = imgui->calc_text_size(_u8L("Object")).x * 2.5 + imgui->calc_text_size("xx"sv).x+ item_spacing;
        //render imgui
        ImGui::AlignTextToFramePadding();
        ImGui::PushItemWidth(label_width);
        imgui->text(label);
        same_line_width += (label_width + item_spacing);
        ImGui::SameLine(same_line_width);
        ImGui::PushItemWidth(item_width);
        size_t selection_out = selection_idx;
        const char *selected_str = (selection_idx >= 0 && selection_idx < int(modes.size())) ? modes[selection_idx].c_str() : "";
        ImGuiWrapper::push_combo_style(get_scale());
        if (ImGui::BBLBeginCombo(("##" + label).c_str(), selected_str, 0)) {
            for (size_t line_idx = 0; line_idx < modes.size(); ++line_idx) {
                ImGui::PushID(int(line_idx));
                if (ImGui::Selectable("", line_idx == selection_idx))
                    selection_out = line_idx;

                ImGui::SameLine();
                ImGui::Text("%s", modes[line_idx].c_str());
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGuiWrapper::pop_combo_style();
        if (selection_idx != selection_out) {//do
            if (selection_out == 0) { m_selection.unlock_volume_selection_mode(); }
            m_selection.set_volume_selection_mode(selection_out == 1 ? Selection::Volume : Selection::Instance);
            if (selection_out == 1) { m_selection.lock_volume_selection_mode(); }
        }
        same_line_width += (label_width + item_width);
    }
    imgui->end();

    ImGuiWrapper::pop_toolbar_style();

    //BBS check ratio changed
    if (m_explosion_ratio != GLVolume::explosion_ratio) {
        for (GLVolume* volume : m_volumes.volumes) {
            volume->set_bounding_boxes_as_dirty();
        }
        GLVolume::explosion_ratio = m_explosion_ratio;
    }
}
void GLCanvas3D::_render_assemble_info() const
{
    if (m_canvas_type != ECanvasType::CanvasAssembleView) {
        return;
    }

    if (m_selection.is_empty()) {
        return;
    }

    ImGuiWrapper* imgui = wxGetApp().imgui();
    auto canvas_w = float(get_canvas_size().get_width());
    auto canvas_h = float(get_canvas_size().get_height());
    float space_size = imgui->get_style_scaling() * 8.0f;
    float caption_max = imgui->calc_text_size(_L("Total Volume:")).x + 3 * space_size;

    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = io.Fonts->Fonts[0];
    float origScale = font->Scale;
    font->Scale = 1.2;
    ImGui::PushFont(font);
    ImGui::PopFont();
    float margin = 10.0f * get_scale();
    imgui->set_next_window_pos(canvas_w - margin, canvas_h - margin, ImGuiCond_Always, 1.0f, 1.0f);
    ImGuiWrapper::push_common_window_style(get_scale()); // ORCA use window style for popups with title
    imgui->begin(_L("Assembly Info"), ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    font->Scale = origScale;
    ImGui::PushFont(font);
    ImGui::PopFont();

    double size0 = m_selection.get_bounding_box().size()(0);
    double size1 = m_selection.get_bounding_box().size()(1);
    double size2 = m_selection.get_bounding_box().size()(2);
    if (!m_selection.is_empty()) {
        ImGui::Text(_L("Volume:").ToUTF8()); ImGui::SameLine(caption_max);
        ImGui::Text("%.2f", size0 * size1 * size2);
        ImGui::Text(_L("Size:").ToUTF8()); ImGui::SameLine(caption_max);
        ImGui::Text("%.2f x %.2f x %.2f", size0, size1, size2);
    }
    imgui->end();
    ImGuiWrapper::pop_common_window_style();
}

#if ENABLE_SHOW_CAMERA_TARGET
void GLCanvas3D::_render_camera_target()
{
    static const float half_length = 5.0f;

    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glLineWidth(2.0f));
    const Vec3f& target = wxGetApp().plater()->get_camera().get_target().cast<float>();
    bool target_changed = !m_camera_target.target.isApprox(target.cast<double>());
    m_camera_target.target = target.cast<double>();

    for (int i = 0; i < 3; ++i) {
        if (!m_camera_target.axis[i].is_initialized() || target_changed) {
            m_camera_target.axis[i].reset();

            GLModel::Geometry init_data;
            init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3, GLModel::Geometry::EIndexType::USHORT };
            init_data.color = (i == X) ? ColorRGBA::X() : ((i == Y) ? ColorRGBA::Y() : ColorRGBA::Z());
            init_data.reserve_vertices(2);
            init_data.reserve_indices(2);

            // vertices
            if (i == X) {
                init_data.add_vertex(Vec3f(target.x() - half_length, target.y(), target.z()));
                init_data.add_vertex(Vec3f(target.x() + half_length, target.y(), target.z()));
            }
            else if (i == Y) {
                init_data.add_vertex(Vec3f(target.x(), target.y() - half_length, target.z()));
                init_data.add_vertex(Vec3f(target.x(), target.y() + half_length, target.z()));
            }
            else {
                init_data.add_vertex(Vec3f(target.x(), target.y(), target.z() - half_length));
                init_data.add_vertex(Vec3f(target.x(), target.y(), target.z() + half_length));
            }

            // indices
            init_data.add_line(0, 1);

            m_camera_target.axis[i].init_from(std::move(init_data));
        }
    }

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader != nullptr) {
        shader->start_using();
        const Camera& camera = wxGetApp().plater()->get_camera();
        shader->set_uniform("view_model_matrix", camera.get_view_matrix());
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        for (int i = 0; i < 3; ++i) {
            m_camera_target.axis[i].render();
        }
        shader->stop_using();
    }
}
#endif // ENABLE_SHOW_CAMERA_TARGET

void GLCanvas3D::_render_sla_slices()
{
    if (!m_use_clipping_planes || current_printer_technology() != ptSLA)
        return;

    const SLAPrint* print = this->sla_print();
    const PrintObjects& print_objects = print->objects();
    if (print_objects.empty())
        // nothing to render, return
        return;

    double clip_min_z = -m_clipping_planes[0].get_data()[3];
    double clip_max_z = m_clipping_planes[1].get_data()[3];
    for (unsigned int i = 0; i < (unsigned int)print_objects.size(); ++i) {
        const SLAPrintObject* obj = print_objects[i];

        if (!obj->is_step_done(slaposSliceSupports))
            continue;

        SlaCap::ObjectIdToModelsMap::iterator it_caps_bottom = m_sla_caps[0].triangles.find(i);
        SlaCap::ObjectIdToModelsMap::iterator it_caps_top = m_sla_caps[1].triangles.find(i);
        {
            if (it_caps_bottom == m_sla_caps[0].triangles.end())
                it_caps_bottom = m_sla_caps[0].triangles.emplace(i, SlaCap::Triangles()).first;
            if (!m_sla_caps[0].matches(clip_min_z)) {
                m_sla_caps[0].z = clip_min_z;
                it_caps_bottom->second.object.reset();
                it_caps_bottom->second.supports.reset();
            }
            if (it_caps_top == m_sla_caps[1].triangles.end())
                it_caps_top = m_sla_caps[1].triangles.emplace(i, SlaCap::Triangles()).first;
            if (!m_sla_caps[1].matches(clip_max_z)) {
                m_sla_caps[1].z = clip_max_z;
                it_caps_top->second.object.reset();
                it_caps_top->second.supports.reset();
            }
        }
        GLModel& bottom_obj_triangles = it_caps_bottom->second.object;
        GLModel& bottom_sup_triangles = it_caps_bottom->second.supports;
        GLModel& top_obj_triangles = it_caps_top->second.object;
        GLModel& top_sup_triangles = it_caps_top->second.supports;

        auto init_model = [](GLModel& model, const Pointf3s& triangles, const ColorRGBA& color) {
            GLModel::Geometry init_data;
            init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
            init_data.reserve_vertices(triangles.size());
            init_data.reserve_indices(triangles.size() / 3);
            init_data.color = color;

            unsigned int vertices_count = 0;
            for (const Vec3d& v : triangles) {
                init_data.add_vertex((Vec3f)v.cast<float>());
                ++vertices_count;
                if (vertices_count % 3 == 0) {
                    init_data.add_triangle(vertices_count - 3, vertices_count - 2, vertices_count - 1);
                }
            }

            if (!init_data.is_empty())
                model.init_from(std::move(init_data));
        };

        if ((!bottom_obj_triangles.is_initialized() || !bottom_sup_triangles.is_initialized() ||
            !top_obj_triangles.is_initialized() || !top_sup_triangles.is_initialized()) && !obj->get_slice_index().empty()) {
            double layer_height         = print->default_object_config().layer_height.value;
            double initial_layer_height = print->material_config().initial_layer_height.value;
            bool   left_handed          = obj->is_left_handed();

            coord_t key_zero = obj->get_slice_index().front().print_level();
            // Slice at the center of the slab starting at clip_min_z will be rendered for the lower plane.
            coord_t key_low  = coord_t((clip_min_z - initial_layer_height + layer_height) / SCALING_FACTOR) + key_zero;
            // Slice at the center of the slab ending at clip_max_z will be rendered for the upper plane.
            coord_t key_high = coord_t((clip_max_z - initial_layer_height) / SCALING_FACTOR) + key_zero;

            const SliceRecord& slice_low  = obj->closest_slice_to_print_level(key_low, coord_t(SCALED_EPSILON));
            const SliceRecord& slice_high = obj->closest_slice_to_print_level(key_high, coord_t(SCALED_EPSILON));

            // Offset to avoid OpenGL Z fighting between the object's horizontal surfaces and the triangluated surfaces of the cuts.
            double plane_shift_z = 0.002;

            if (slice_low.is_valid()) {
                const ExPolygons& obj_bottom = slice_low.get_slice(soModel);
                const ExPolygons& sup_bottom = slice_low.get_slice(soSupport);
                // calculate model bottom cap
                // calculate model bottom cap
                if (!bottom_obj_triangles.is_initialized() && !obj_bottom.empty())
                    init_model(bottom_obj_triangles, triangulate_expolygons_3d(obj_bottom, clip_min_z - plane_shift_z, !left_handed), { 1.0f, 0.37f, 0.0f, 1.0f });
                // calculate support bottom cap
                if (!bottom_sup_triangles.is_initialized() && !sup_bottom.empty())
                    init_model(bottom_sup_triangles, triangulate_expolygons_3d(sup_bottom, clip_min_z - plane_shift_z, !left_handed), { 1.0f, 0.0f, 0.37f, 1.0f });
            }

            if (slice_high.is_valid()) {
                const ExPolygons& obj_top = slice_high.get_slice(soModel);
                const ExPolygons& sup_top = slice_high.get_slice(soSupport);
                // calculate model top cap
                // calculate model top cap
                if (!top_obj_triangles.is_initialized() && !obj_top.empty())
                    init_model(top_obj_triangles, triangulate_expolygons_3d(obj_top, clip_max_z + plane_shift_z, left_handed), { 1.0f, 0.37f, 0.0f, 1.0f });
                // calculate support top cap
                if (!top_sup_triangles.is_initialized() && !sup_top.empty())
                    init_model(top_sup_triangles, triangulate_expolygons_3d(sup_top, clip_max_z + plane_shift_z, left_handed), { 1.0f, 0.0f, 0.37f, 1.0f });
            }
        }

        GLShaderProgram* shader = wxGetApp().get_shader("flat");
        if (shader != nullptr) {
            shader->start_using();

            for (const SLAPrintObject::Instance& inst : obj->instances()) {
                const Camera& camera = wxGetApp().plater()->get_camera();
                const Transform3d view_model_matrix = camera.get_view_matrix() *
                    Geometry::assemble_transform(Vec3d(unscale<double>(inst.shift.x()), unscale<double>(inst.shift.y()), 0.0),
                        inst.rotation * Vec3d::UnitZ(), Vec3d::Ones(),
                        obj->is_left_handed() ? Vec3d(-1.0f, 1.0f, 1.0f) : Vec3d::Ones());

                shader->set_uniform("view_model_matrix", view_model_matrix);
                shader->set_uniform("projection_matrix", camera.get_projection_matrix());

                bottom_obj_triangles.render();
                top_obj_triangles.render();
                bottom_sup_triangles.render();
                top_sup_triangles.render();

            }

            shader->stop_using();
        }
    }
}

void GLCanvas3D::_render_selection_sidebar_hints()
{
    m_selection.render_sidebar_hints(m_sidebar_field, m_gizmos.get_uniform_scaling());
}

void GLCanvas3D::_update_volumes_hover_state()
{
    for (GLVolume* v : m_volumes.volumes) {
        v->hover = GLVolume::HS_None;
    }

    if (m_hover_volume_idxs.empty())
        return;

    bool ctrl_pressed = wxGetKeyState(WXK_CONTROL); // additive select/deselect
    bool shift_pressed = wxGetKeyState(WXK_SHIFT);  // select by rectangle
    bool alt_pressed = wxGetKeyState(WXK_ALT);      // deselect by rectangle

    if (alt_pressed && (shift_pressed || ctrl_pressed)) {
        // illegal combinations of keys
        m_hover_volume_idxs.clear();
        return;
    }

    bool selection_modifiers_only = m_selection.is_empty() || m_selection.is_any_modifier();

    bool hover_modifiers_only = true;
    for (int i : m_hover_volume_idxs) {
        if (!m_volumes.volumes[i]->is_modifier) {
            hover_modifiers_only = false;
            break;
        }
    }

    std::set<std::pair<int, int>> hover_instances;
    for (int i : m_hover_volume_idxs) {
        const GLVolume& v = *m_volumes.volumes[i];
        hover_instances.insert(std::make_pair(v.object_idx(), v.instance_idx()));
    }

    bool hover_from_single_instance = hover_instances.size() == 1;

    if (hover_modifiers_only && !hover_from_single_instance) {
        // do not allow to select volumes from different instances
        m_hover_volume_idxs.clear();
        return;
    }

    for (int i : m_hover_volume_idxs) {
        GLVolume& volume = *m_volumes.volumes[i];
        if (volume.hover != GLVolume::HS_None)
            continue;

        bool deselect = volume.selected && ((ctrl_pressed && !shift_pressed) || alt_pressed);
        // (volume->is_modifier && !selection_modifiers_only && !is_ctrl_pressed) -> allows hovering on selected modifiers belonging to selection of type Instance
        bool select = (!volume.selected || (volume.is_modifier && !selection_modifiers_only && !ctrl_pressed)) && !alt_pressed;

        if (select || deselect) {
            bool as_volume =
                volume.is_modifier && hover_from_single_instance && !ctrl_pressed &&
                (
                (!deselect) ||
                (deselect && !m_selection.is_single_full_instance() && (volume.object_idx() == m_selection.get_object_idx()) && (volume.instance_idx() == m_selection.get_instance_idx()))
                );

            if (as_volume)
                volume.hover = deselect ? GLVolume::HS_Deselect : GLVolume::HS_Select;
            else {
                int object_idx = volume.object_idx();
                int instance_idx = volume.instance_idx();

                for (GLVolume* v : m_volumes.volumes) {
                    if (v->object_idx() == object_idx && v->instance_idx() == instance_idx)
                        v->hover = deselect ? GLVolume::HS_Deselect : GLVolume::HS_Select;
                }
            }
        }
        else if (volume.selected)
            volume.hover = GLVolume::HS_Hover;
    }
}

void GLCanvas3D::_perform_layer_editing_action(wxMouseEvent* evt)
{
    int object_idx_selected = m_layers_editing.last_object_id;
    if (object_idx_selected == -1)
        return;

    // A volume is selected. Test, whether hovering over a layer thickness bar.
    if (evt != nullptr) {
        const Rect& rect = LayersEditing::get_bar_rect_screen(*this);
        float b = rect.get_bottom();
        m_layers_editing.last_z = m_layers_editing.object_max_z() * (b - evt->GetY() - 1.0f) / (b - rect.get_top());
        m_layers_editing.last_action =
            evt->ShiftDown() ? (evt->RightIsDown() ? LAYER_HEIGHT_EDIT_ACTION_SMOOTH : LAYER_HEIGHT_EDIT_ACTION_REDUCE) :
            (evt->RightIsDown() ? LAYER_HEIGHT_EDIT_ACTION_INCREASE : LAYER_HEIGHT_EDIT_ACTION_DECREASE);
    }

    m_layers_editing.adjust_layer_height_profile();
    _refresh_if_shown_on_screen();

    // Automatic action on mouse down with the same coordinate.
    _start_timer();
}

Vec3d GLCanvas3D::_mouse_to_3d(const Point& mouse_pos, float* z)
{
    if (m_canvas == nullptr)
        return Vec3d(DBL_MAX, DBL_MAX, DBL_MAX);

    if (z == nullptr) {
        const SceneRaycaster::HitResult hit = m_scene_raycaster.hit(mouse_pos.cast<double>(), wxGetApp().plater()->get_camera(), nullptr);
        return hit.is_valid() ? hit.position.cast<double>() : _mouse_to_bed_3d(mouse_pos);
    }
    else {
        const Camera& camera = wxGetApp().plater()->get_camera();
        const Vec4i32 viewport(camera.get_viewport().data());
        Vec3d out;
        igl::unproject(Vec3d(mouse_pos.x(), viewport[3] - mouse_pos.y(), *z), camera.get_view_matrix().matrix(), camera.get_projection_matrix().matrix(), viewport, out);
        return out;
    }
}

Vec3d GLCanvas3D::_mouse_to_bed_3d(const Point& mouse_pos)
{
    return mouse_ray(mouse_pos).intersect_plane(0.0);
}

void GLCanvas3D::_start_timer()
{
    m_timer.Start(100, wxTIMER_CONTINUOUS);
}

void GLCanvas3D::_stop_timer()
{
    m_timer.Stop();
}

void GLCanvas3D::_load_print_toolpaths(const BuildVolume &build_volume)
{
    const Print *print = this->fff_print();
    if (print == nullptr)
        return;

    if (! print->is_step_done(psSkirtBrim))
        return;

    if (!print->has_skirt() && !print->has_brim())
        return;

    const ColorRGBA color = ColorRGBA::GREENISH();

    // number of skirt layers
    size_t total_layer_count = 0;
    for (const PrintObject* print_object : print->objects()) {
        total_layer_count = std::max(total_layer_count, print_object->total_layer_count());
    }
    size_t skirt_height = print->has_infinite_skirt() ? total_layer_count : std::min<size_t>(print->config().skirt_height.value, total_layer_count);
    if (skirt_height == 0 && print->has_brim())
        skirt_height = 1;

    // Get first skirt_height layers.
    //FIXME This code is fishy. It may not work for multiple objects with different layering due to variable layer height feature.
    // This is not critical as this is just an initial preview.
    const PrintObject* highest_object = *std::max_element(print->objects().begin(), print->objects().end(), [](auto l, auto r){ return l->layers().size() < r->layers().size(); });
    std::vector<float> print_zs;
    print_zs.reserve(skirt_height * 2);
    for (size_t i = 0; i < std::min(skirt_height, highest_object->layers().size()); ++ i)
        print_zs.emplace_back(float(highest_object->layers()[i]->print_z));
    // Only add skirt for the raft layers.
    for (size_t i = 0; i < std::min(skirt_height, std::min(highest_object->slicing_parameters().raft_layers(), highest_object->support_layers().size())); ++ i)
        print_zs.emplace_back(float(highest_object->support_layers()[i]->print_z));
    sort_remove_duplicates(print_zs);
    skirt_height = std::min(skirt_height, print_zs.size());
    print_zs.erase(print_zs.begin() + skirt_height, print_zs.end());

    GLVolume* volume = m_volumes.new_toolpath_volume(color);
    GLModel::Geometry init_data;
    init_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    for (size_t i = 0; i < skirt_height; ++ i) {
        volume->print_zs.emplace_back(print_zs[i]);
        volume->offsets.emplace_back(init_data.indices_count());
        //BBS: usage of m_brim are deleted
        _3DScene::extrusionentity_to_verts(print->skirt(), print_zs[i], Point(0, 0), init_data);
        // Ensure that no volume grows over the limits. If the volume is too large, allocate a new one.
        if (init_data.vertices_size_bytes() > MAX_VERTEX_BUFFER_SIZE) {
            volume->model.init_from(std::move(init_data));
            GLVolume &vol = *volume;
            volume = m_volumes.new_toolpath_volume(vol.color);
        }
    }
    volume->model.init_from(std::move(init_data));
    volume->is_outside = !contains(build_volume, volume->model);
}

void GLCanvas3D::_load_print_object_toolpaths(const PrintObject& print_object, const BuildVolume& build_volume, const std::vector<std::string>& str_tool_colors, const std::vector<CustomGCode::Item>& color_print_values)
{
    std::vector<ColorRGBA> tool_colors;
    decode_colors(str_tool_colors, tool_colors);

    struct Ctxt
    {
        const PrintInstances        *shifted_copies;
        std::vector<const Layer*>    layers;
        bool                         has_perimeters;
        bool                         has_infill;
        bool                         has_support;
        const std::vector<ColorRGBA>* tool_colors;
        bool                         is_single_material_print;
        int                          filaments_cnt;
        const std::vector<CustomGCode::Item>*   color_print_values;

        static ColorRGBA color_perimeters()           { return ColorRGBA::YELLOW(); }
        static ColorRGBA color_infill()               { return ColorRGBA::REDISH(); }
        static ColorRGBA color_support()              { return ColorRGBA::GREENISH(); }
        static ColorRGBA color_pause_or_custom_code() { return ColorRGBA::GRAY(); }

        // For cloring by a tool, return a parsed color.
        bool                         color_by_tool() const { return tool_colors != nullptr; }
        size_t                       number_tools() const { return color_by_tool() ? tool_colors->size() : 0; }
        const ColorRGBA&             color_tool(size_t tool) const { return (*tool_colors)[tool]; }

        // For coloring by a color_print(M600), return a parsed color.
        bool                         color_by_color_print() const { return color_print_values!=nullptr; }
        const size_t                 color_print_color_idx_by_layer_idx(const size_t layer_idx) const {
            const CustomGCode::Item value{layers[layer_idx]->print_z + EPSILON, CustomGCode::Custom, 0, ""};
            auto it = std::lower_bound(color_print_values->begin(), color_print_values->end(), value);
            return (it - color_print_values->begin()) % number_tools();
        }

        const size_t                 color_print_color_idx_by_layer_idx_and_extruder(const size_t layer_idx, const int extruder) const
        {
            const coordf_t print_z = layers[layer_idx]->print_z;

            auto it = std::find_if(color_print_values->begin(), color_print_values->end(),
                [print_z](const CustomGCode::Item& code)
                { return fabs(code.print_z - print_z) < EPSILON; });
            if (it != color_print_values->end()) {
                CustomGCode::Type type = it->type;
                // pause print or custom Gcode
                if (type == CustomGCode::PausePrint ||
                    (type != CustomGCode::ColorChange && type != CustomGCode::ToolChange))
                    return number_tools()-1; // last color item is a gray color for pause print or custom G-code

                // change tool (extruder)
                if (type == CustomGCode::ToolChange)
                    return get_color_idx_for_tool_change(it, extruder);
                // change color for current extruder
                if (type == CustomGCode::ColorChange) {
                    int color_idx = get_color_idx_for_color_change(it, extruder);
                    if (color_idx >= 0)
                        return color_idx;
                }
            }

            const CustomGCode::Item value{print_z + EPSILON, CustomGCode::Custom, 0, ""};
            it = std::lower_bound(color_print_values->begin(), color_print_values->end(), value);
            while (it != color_print_values->begin()) {
                --it;
                // change color for current extruder
                if (it->type == CustomGCode::ColorChange) {
                    int color_idx = get_color_idx_for_color_change(it, extruder);
                    if (color_idx >= 0)
                        return color_idx;
                }
                // change tool (extruder)
                if (it->type == CustomGCode::ToolChange)
                    return get_color_idx_for_tool_change(it, extruder);
            }

            return std::min<int>(filaments_cnt - 1, std::max<int>(extruder - 1, 0));;
        }

    private:
        int get_m600_color_idx(std::vector<CustomGCode::Item>::const_iterator it) const
        {
            int shift = 0;
            while (it != color_print_values->begin()) {
                --it;
                if (it->type == CustomGCode::ColorChange)
                    shift++;
            }
            return filaments_cnt + shift;
        }

        int get_color_idx_for_tool_change(std::vector<CustomGCode::Item>::const_iterator it, const int extruder) const
        {
            const int current_extruder = it->extruder == 0 ? extruder : it->extruder;
            if (number_tools() == size_t(filaments_cnt + 1)) // there is no one "M600"
                return std::min<int>(filaments_cnt - 1, std::max<int>(current_extruder - 1, 0));

            auto it_n = it;
            while (it_n != color_print_values->begin()) {
                --it_n;
                if (it_n->type == CustomGCode::ColorChange && it_n->extruder == current_extruder)
                    return get_m600_color_idx(it_n);
            }

            return std::min<int>(filaments_cnt - 1, std::max<int>(current_extruder - 1, 0));
        }

        int get_color_idx_for_color_change(std::vector<CustomGCode::Item>::const_iterator it, const int extruder) const
        {
            if (filaments_cnt == 1)
                return get_m600_color_idx(it);

            auto it_n = it;
            bool is_tool_change = false;
            while (it_n != color_print_values->begin()) {
                --it_n;
                if (it_n->type == CustomGCode::ToolChange) {
                    is_tool_change = true;
                    if (it_n->extruder == it->extruder || (it_n->extruder == 0 && it->extruder == extruder))
                        return get_m600_color_idx(it);
                    break;
                }
            }
            if (!is_tool_change && it->extruder == extruder)
                return get_m600_color_idx(it);

            return -1;
        }

    } ctxt;

    ctxt.has_perimeters = print_object.is_step_done(posPerimeters);
    ctxt.has_infill = print_object.is_step_done(posInfill);
    ctxt.has_support = print_object.is_step_done(posSupportMaterial);
    ctxt.tool_colors = tool_colors.empty() ? nullptr : &tool_colors;
    ctxt.color_print_values = color_print_values.empty() ? nullptr : &color_print_values;
    ctxt.is_single_material_print = this->fff_print()->extruders().size()==1;
    ctxt.filaments_cnt = wxGetApp().filaments_cnt();

    ctxt.shifted_copies = &print_object.instances();

    // order layers by print_z
    {
        size_t nlayers = 0;
        if (ctxt.has_perimeters || ctxt.has_infill)
            nlayers = print_object.layers().size();
        if (ctxt.has_support)
            nlayers += print_object.support_layers().size();
        ctxt.layers.reserve(nlayers);
    }
    if (ctxt.has_perimeters || ctxt.has_infill)
        for (const Layer *layer : print_object.layers())
            ctxt.layers.emplace_back(layer);
    if (ctxt.has_support)
        for (const Layer *layer : print_object.support_layers())
            ctxt.layers.emplace_back(layer);
    std::sort(ctxt.layers.begin(), ctxt.layers.end(), [](const Layer *l1, const Layer *l2) { return l1->print_z < l2->print_z; });

    // Maximum size of an allocation block: 32MB / sizeof(float)
    BOOST_LOG_TRIVIAL(debug) << "Loading print object toolpaths in parallel - start" << m_volumes.log_memory_info() << log_memory_info();

    const bool is_selected_separate_extruder = m_selected_extruder > 0 && ctxt.color_by_color_print();

    //FIXME Improve the heuristics for a grain size.
    size_t          grain_size = std::max(ctxt.layers.size() / 16, size_t(1));
    tbb::spin_mutex new_volume_mutex;
    auto            new_volume = [this, &new_volume_mutex](const ColorRGBA& color) {
        // Allocate the volume before locking.
		GLVolume *volume = new GLVolume(color);
		volume->is_extrusion_path = true;
        // to prevent sending data to gpu (in the main thread) while
        // editing the model geometry
        volume->model.disable_render();
        tbb::spin_mutex::scoped_lock lock;
    	// Lock by ROII, so if the emplace_back() fails, the lock will be released.
        lock.acquire(new_volume_mutex);
        m_volumes.volumes.emplace_back(volume);
        lock.release();
        return volume;
    };
    const size_t    volumes_cnt_initial = m_volumes.volumes.size();
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, ctxt.layers.size(), grain_size),
        [&ctxt, &new_volume, is_selected_separate_extruder, this](const tbb::blocked_range<size_t>& range) {
        GLVolumePtrs 		vols;
        std::vector<GLModel::Geometry> geometries;
        auto select_geometry = [&ctxt, &geometries](size_t layer_idx, int extruder, int feature) -> GLModel::Geometry& {
            return geometries[ctxt.color_by_color_print() ?
                ctxt.color_print_color_idx_by_layer_idx_and_extruder(layer_idx, extruder) :
                ctxt.color_by_tool() ?
                std::min<int>(ctxt.number_tools() - 1, std::max<int>(extruder - 1, 0)) :
                feature
            ];
        };
        if (ctxt.color_by_color_print() || ctxt.color_by_tool()) {
            for (size_t i = 0; i < ctxt.number_tools(); ++i) {
                vols.emplace_back(new_volume(ctxt.color_tool(i)));
                geometries.emplace_back(GLModel::Geometry());
            }
        }
        else {
            vols = { new_volume(ctxt.color_perimeters()), new_volume(ctxt.color_infill()), new_volume(ctxt.color_support()) };
            geometries = { GLModel::Geometry(), GLModel::Geometry(), GLModel::Geometry() };
        }

        assert(vols.size() == geometries.size());
        for (GLModel::Geometry& g : geometries) {
            g.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
        }
        for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++ idx_layer) {
            const Layer *layer = ctxt.layers[idx_layer];

            if (is_selected_separate_extruder) {
                bool at_least_one_has_correct_extruder = false;
                for (const LayerRegion* layerm : layer->regions()) {
                    if (layerm->slices.surfaces.empty())
                        continue;
                    const PrintRegionConfig& cfg = layerm->region().config();
                    if (cfg.wall_filament.value    == m_selected_extruder ||
                        cfg.sparse_infill_filament.value       == m_selected_extruder ||
                        cfg.solid_infill_filament.value == m_selected_extruder ) {
                        at_least_one_has_correct_extruder = true;
                        break;
                    }
                }
                if (!at_least_one_has_correct_extruder)
                    continue;
            }

            for (size_t i = 0; i < vols.size(); ++i) {
                GLVolume* vol = vols[i];
                if (vol->print_zs.empty() || vol->print_zs.back() != layer->print_z) {
                    vol->print_zs.emplace_back(layer->print_z);
                    vol->offsets.emplace_back(geometries[i].indices_count());
                }
            }

            for (const PrintInstance &instance : *ctxt.shifted_copies) {
                const Point &copy = instance.shift;
                for (const LayerRegion *layerm : layer->regions()) {
                    if (is_selected_separate_extruder)
                    {
                        const PrintRegionConfig& cfg = layerm->region().config();
                        if (cfg.wall_filament.value    != m_selected_extruder ||
                            cfg.sparse_infill_filament.value       != m_selected_extruder ||
                            cfg.solid_infill_filament.value != m_selected_extruder)
                            continue;
                    }
                    if (ctxt.has_perimeters)
                        _3DScene::extrusionentity_to_verts(layerm->perimeters, float(layer->print_z), copy,
                        	select_geometry(idx_layer, layerm->region().config().wall_filament.value, 0));
                    if (ctxt.has_infill) {
                        for (const ExtrusionEntity *ee : layerm->fills.entities) {
                            // fill represents infill extrusions of a single island.
                            const auto *fill = dynamic_cast<const ExtrusionEntityCollection*>(ee);
                            if (! fill->entities.empty())
                                _3DScene::extrusionentity_to_verts(*fill, float(layer->print_z), copy,
                                    select_geometry(idx_layer, is_solid_infill(fill->entities.front()->role()) ?
                                                    layerm->region().config().solid_infill_filament :
                                                    layerm->region().config().sparse_infill_filament, 1));
                        }
                    }
                }
                if (ctxt.has_support) {
                    const SupportLayer *support_layer = dynamic_cast<const SupportLayer*>(layer);
                    if (support_layer) {
                        for (const ExtrusionEntity *extrusion_entity : support_layer->support_fills.entities)
                            _3DScene::extrusionentity_to_verts(extrusion_entity, float(layer->print_z), copy,
	                            select_geometry(idx_layer, (extrusion_entity->role() == erSupportMaterial || extrusion_entity->role() == erSupportTransition) ?
                                                support_layer->object()->config().support_filament :
                                                support_layer->object()->config().support_interface_filament, 2));
                    }
                }
            }
            // Ensure that no volume grows over the limits. If the volume is too large, allocate a new one.
	        for (size_t i = 0; i < vols.size(); ++i) {
	            GLVolume &vol = *vols[i];
                if (geometries[i].vertices_size_bytes() > MAX_VERTEX_BUFFER_SIZE) {
                    vol.model.init_from(std::move(geometries[i]));
                    vols[i] = new_volume(vol.color);
                }
	        }
        }
        for (size_t i = 0; i < vols.size(); ++i) {
            if (!geometries[i].is_empty())
                vols[i]->model.init_from(std::move(geometries[i]));
        }
    });

    BOOST_LOG_TRIVIAL(debug) << "Loading print object toolpaths in parallel - finalizing results" << m_volumes.log_memory_info() << log_memory_info();
    // Remove empty volumes from the newly added volumes.
    {
        for (auto ptr_it = m_volumes.volumes.begin() + volumes_cnt_initial; ptr_it != m_volumes.volumes.end(); ++ptr_it)
            if ((*ptr_it)->empty()) {
                delete *ptr_it;
                *ptr_it = nullptr;
            }
        m_volumes.volumes.erase(std::remove(m_volumes.volumes.begin() + volumes_cnt_initial, m_volumes.volumes.end(), nullptr), m_volumes.volumes.end());
    }
    for (size_t i = volumes_cnt_initial; i < m_volumes.volumes.size(); ++i) {
        GLVolume* v = m_volumes.volumes[i];
        v->is_outside = !contains(build_volume, v->model);
        // We are done editinig the model, now it can be sent to gpu
        v->model.enable_render();
    }

    BOOST_LOG_TRIVIAL(debug) << "Loading print object toolpaths in parallel - end" << m_volumes.log_memory_info() << log_memory_info();
}

void GLCanvas3D::_load_wipe_tower_toolpaths(const BuildVolume& build_volume, const std::vector<std::string>& str_tool_colors)
{
    const Print *print = this->fff_print();
    if (print == nullptr || print->wipe_tower_data().tool_changes.empty())
        return;

    if (!print->is_step_done(psWipeTower))
        return;

    std::vector<ColorRGBA> tool_colors;
    decode_colors(str_tool_colors, tool_colors);

    struct Ctxt
    {
        const Print                  *print;
        const std::vector<ColorRGBA> *tool_colors;
        Vec2f                         wipe_tower_pos;
        float                         wipe_tower_angle;

        static ColorRGBA color_support() { return ColorRGBA::GREENISH(); }

        // For cloring by a tool, return a parsed color.
        bool                         color_by_tool() const { return tool_colors != nullptr; }
        size_t                       number_tools() const { return this->color_by_tool() ? tool_colors->size() : 0; }
        const ColorRGBA&             color_tool(size_t tool) const { return (*tool_colors)[tool]; }
        int                          volume_idx(int tool, int feature) const {
            return this->color_by_tool() ? std::min<int>(this->number_tools() - 1, std::max<int>(tool, 0)) : feature;
        }

        const std::vector<WipeTower::ToolChangeResult>& tool_change(size_t idx) {
            const auto &tool_changes = print->wipe_tower_data().tool_changes;
            return priming.empty() ?
                ((idx == tool_changes.size()) ? final : tool_changes[idx]) :
                ((idx == 0) ? priming : (idx == tool_changes.size() + 1) ? final : tool_changes[idx - 1]);
        }
        std::vector<WipeTower::ToolChangeResult> priming;
        std::vector<WipeTower::ToolChangeResult> final;
    } ctxt;

    ctxt.print = print;
    ctxt.tool_colors = tool_colors.empty() ? nullptr : &tool_colors;
    if (print->wipe_tower_data().priming)
        for (int i=0; i<(int)print->wipe_tower_data().priming.get()->size(); ++i)
            ctxt.priming.emplace_back(print->wipe_tower_data().priming.get()->at(i));
    if (print->wipe_tower_data().final_purge)
        ctxt.final.emplace_back(*print->wipe_tower_data().final_purge.get());

    ctxt.wipe_tower_angle = ctxt.print->config().wipe_tower_rotation_angle.value/180.f * PI;

    // BBS: add partplate logic
    int plate_idx = print->get_plate_index();
    Vec3d plate_origin = print->get_plate_origin();
    double wipe_tower_x = ctxt.print->config().wipe_tower_x.get_at(plate_idx) + plate_origin(0);
    double wipe_tower_y = ctxt.print->config().wipe_tower_y.get_at(plate_idx) + plate_origin(1);
    ctxt.wipe_tower_pos = Vec2f(wipe_tower_x, wipe_tower_y);

    BOOST_LOG_TRIVIAL(debug) << "Loading wipe tower toolpaths in parallel - start" << m_volumes.log_memory_info() << log_memory_info();

    //FIXME Improve the heuristics for a grain size.
    size_t          n_items = print->wipe_tower_data().tool_changes.size() + (ctxt.priming.empty() ? 0 : 1);
    size_t          grain_size = std::max(n_items / 128, size_t(1));
    tbb::spin_mutex new_volume_mutex;
    auto            new_volume = [this, &new_volume_mutex](const ColorRGBA& color) {
        auto *volume = new GLVolume(color);
		volume->is_extrusion_path = true;
        // to prevent sending data to gpu (in the main thread) while
        // editing the model geometry
        volume->model.disable_render();
        tbb::spin_mutex::scoped_lock lock;
        lock.acquire(new_volume_mutex);
        m_volumes.volumes.emplace_back(volume);
        lock.release();
        return volume;
    };
    const size_t   volumes_cnt_initial = m_volumes.volumes.size();
    std::vector<GLVolumeCollection> volumes_per_thread(n_items);
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, n_items, grain_size),
        [&ctxt, &new_volume](const tbb::blocked_range<size_t>& range) {
        // Bounding box of this slab of a wipe tower.
        GLVolumePtrs vols;
        std::vector<GLModel::Geometry> geometries;
        if (ctxt.color_by_tool()) {
            for (size_t i = 0; i < ctxt.number_tools(); ++i) {
                vols.emplace_back(new_volume(ctxt.color_tool(i)));
                geometries.emplace_back(GLModel::Geometry());
            }
        }
        else {
            vols = { new_volume(ctxt.color_support()) };
            geometries = { GLModel::Geometry() };
        }

        assert(vols.size() == geometries.size());
        for (GLModel::Geometry& g : geometries) {
            g.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
        }
        for (size_t idx_layer = range.begin(); idx_layer < range.end(); ++idx_layer) {
            const std::vector<WipeTower::ToolChangeResult> &layer = ctxt.tool_change(idx_layer);
            for (size_t i = 0; i < vols.size(); ++i) {
                GLVolume &vol = *vols[i];
                if (vol.print_zs.empty() || vol.print_zs.back() != layer.front().print_z) {
                    vol.print_zs.emplace_back(layer.front().print_z);
                    vol.offsets.emplace_back(geometries[i].indices_count());
                }
            }
            for (const WipeTower::ToolChangeResult &extrusions : layer) {
                for (size_t i = 1; i < extrusions.extrusions.size();) {
                    const WipeTower::Extrusion &e = extrusions.extrusions[i];
                    if (e.width == 0.) {
                        ++i;
                        continue;
                    }
                    size_t j = i + 1;
                    if (ctxt.color_by_tool())
                        for (; j < extrusions.extrusions.size() && extrusions.extrusions[j].tool == e.tool && extrusions.extrusions[j].width > 0.f; ++j);
                    else
                        for (; j < extrusions.extrusions.size() && extrusions.extrusions[j].width > 0.f; ++j);
                    size_t              n_lines = j - i;
                    Lines               lines;
                    std::vector<double> widths;
                    std::vector<double> heights;
                    lines.reserve(n_lines);
                    widths.reserve(n_lines);
                    heights.assign(n_lines, extrusions.layer_height);
                    WipeTower::Extrusion e_prev = extrusions.extrusions[i-1];

                    if (!extrusions.priming) { // wipe tower extrusions describe the wipe tower at the origin with no rotation
                        e_prev.pos = Eigen::Rotation2Df(ctxt.wipe_tower_angle) * e_prev.pos;
                        e_prev.pos += ctxt.wipe_tower_pos;
                    }

                    for (; i < j; ++i) {
                        WipeTower::Extrusion e = extrusions.extrusions[i];
                        assert(e.width > 0.f);
                        if (!extrusions.priming) {
                            e.pos = Eigen::Rotation2Df(ctxt.wipe_tower_angle) * e.pos;
                            e.pos += ctxt.wipe_tower_pos;
                        }

                        lines.emplace_back(Point::new_scale(e_prev.pos.x(), e_prev.pos.y()), Point::new_scale(e.pos.x(), e.pos.y()));
                        widths.emplace_back(e.width);

                        e_prev = e;
                    }
                    _3DScene::thick_lines_to_verts(lines, widths, heights, lines.front().a == lines.back().b, extrusions.print_z,
                        geometries[ctxt.volume_idx(e.tool, 0)]);
                }
            }
        }
        for (size_t i = 0; i < vols.size(); ++i) {
            GLVolume &vol = *vols[i];
            if (geometries[i].vertices_size_bytes() > MAX_VERTEX_BUFFER_SIZE) {
                vol.model.init_from(std::move(geometries[i]));
                vols[i] = new_volume(vol.color);
            }
        }
        for (size_t i = 0; i < vols.size(); ++i) {
            if (!geometries[i].is_empty())
                vols[i]->model.init_from(std::move(geometries[i]));
        }
        });

    BOOST_LOG_TRIVIAL(debug) << "Loading wipe tower toolpaths in parallel - finalizing results" << m_volumes.log_memory_info() << log_memory_info();
    // Remove empty volumes from the newly added volumes.
    {
        for (auto ptr_it = m_volumes.volumes.begin() + volumes_cnt_initial; ptr_it != m_volumes.volumes.end(); ++ptr_it)
            if ((*ptr_it)->empty()) {
                delete *ptr_it;
                *ptr_it = nullptr;
            }
        m_volumes.volumes.erase(std::remove(m_volumes.volumes.begin() + volumes_cnt_initial, m_volumes.volumes.end(), nullptr), m_volumes.volumes.end());
    }
    for (size_t i = volumes_cnt_initial; i < m_volumes.volumes.size(); ++i) {
        GLVolume* v = m_volumes.volumes[i];
        v->is_outside = !contains(build_volume, v->model);
        // We are done editinig the model, now it can be sent to gpu
        v->model.enable_render();
    }

    BOOST_LOG_TRIVIAL(debug) << "Loading wipe tower toolpaths in parallel - end" << m_volumes.log_memory_info() << log_memory_info();
}

// While it looks like we can call
// this->reload_scene(true, true)
// the two functions are quite different:
// 1) This function only loads objects, for which the step slaposSliceSupports already finished. Therefore objects outside of the print bed never load.
// 2) This function loads object mesh with the relative scaling correction (the "relative_correction" parameter) was applied,
// 	  therefore the mesh may be slightly larger or smaller than the mesh shown in the 3D scene.
void GLCanvas3D::_load_sla_shells()
{
    const SLAPrint* print = this->sla_print();
    if (print->objects().empty())
        // nothing to render, return
        return;

    auto add_volume = [this](const SLAPrintObject &object, int volume_id, const SLAPrintObject::Instance& instance,
        const TriangleMesh& mesh, const ColorRGBA& color, bool outside_printer_detection_enabled) {
        m_volumes.volumes.emplace_back(new GLVolume(color));
        GLVolume& v = *m_volumes.volumes.back();
#if ENABLE_SMOOTH_NORMALS
        v.model.init_from(mesh, true);
#else
        v.model.init_from(mesh);
#endif // ENABLE_SMOOTH_NORMALS
        v.shader_outside_printer_detection_enabled = outside_printer_detection_enabled;
        v.composite_id.volume_id = volume_id;
        v.set_instance_offset(unscale(instance.shift.x(), instance.shift.y(), 0.0));
        v.set_instance_rotation({ 0.0, 0.0, (double)instance.rotation });
        v.set_instance_mirror(X, object.is_left_handed() ? -1. : 1.);
        v.set_convex_hull(mesh.convex_hull_3d());
    };

    // adds objects' volumes
    for (const SLAPrintObject* obj : print->objects())
        if (obj->is_step_done(slaposSliceSupports)) {
            unsigned int initial_volumes_count = (unsigned int)m_volumes.volumes.size();
            for (const SLAPrintObject::Instance& instance : obj->instances()) {
                add_volume(*obj, 0, instance, obj->get_mesh_to_print(), GLVolume::MODEL_COLOR[0], true);
                // Set the extruder_id and volume_id to achieve the same color as in the 3D scene when
                // through the update_volumes_colors_by_extruder() call.
                m_volumes.volumes.back()->extruder_id = obj->model_object()->volumes.front()->extruder_id();
                if (obj->is_step_done(slaposSupportTree) && obj->has_mesh(slaposSupportTree))
                    add_volume(*obj, -int(slaposSupportTree), instance, obj->support_mesh(), GLVolume::SLA_SUPPORT_COLOR, true);
                if (obj->is_step_done(slaposPad) && obj->has_mesh(slaposPad))
                    add_volume(*obj, -int(slaposPad), instance, obj->pad_mesh(), GLVolume::SLA_PAD_COLOR, false);
            }
            double shift_z = obj->get_current_elevation();
            for (unsigned int i = initial_volumes_count; i < m_volumes.volumes.size(); ++ i) {
                // apply shift z
                m_volumes.volumes[i]->set_sla_shift_z(shift_z);
            }
        }

    update_volumes_colors_by_extruder();
}

void GLCanvas3D::_update_sla_shells_outside_state()
{
    check_volumes_outside_state();
}

void GLCanvas3D::_set_warning_notification_if_needed(EWarning warning)
{
    _set_current();
    bool show = false;
    if (!m_volumes.empty()) {
        show = _is_any_volume_outside();
        show &= m_gcode_viewer.has_data() && m_gcode_viewer.is_contained_in_bed() && m_gcode_viewer.m_conflict_result.has_value();
    } else {
        if (wxGetApp().is_editor()) {
            if (current_printer_technology() != ptSLA) {
                unsigned int max_z_layer = m_gcode_viewer.get_layers_z_range().back();
                if (warning == EWarning::ToolHeightOutside) // check if max z_layer height exceed max print height
                    show = m_gcode_viewer.has_data() && (m_gcode_viewer.get_layers_zs()[max_z_layer] - m_gcode_viewer.get_max_print_height() >= 1e-6);
                else if (warning == EWarning::ToolpathOutside) { // check if max x,y coords exceed bed area
                    show = m_gcode_viewer.has_data() && !m_gcode_viewer.is_contained_in_bed() &&
                           (m_gcode_viewer.get_max_print_height() -m_gcode_viewer.get_layers_zs()[max_z_layer] >= 1e-6);
                }
                else if (warning == EWarning::GCodeConflict)
                    show = m_gcode_viewer.has_data() && m_gcode_viewer.is_contained_in_bed() && m_gcode_viewer.m_conflict_result.has_value();
                else if (warning == EWarning::MultiExtruderPrintableError)
                    show = m_gcode_viewer.has_data() && (m_gcode_viewer.m_gcode_check_result.error_code & 1);
                else if (warning == EWarning::MultiExtruderHeightOutside)
                    show = m_gcode_viewer.has_data() && (m_gcode_viewer.m_gcode_check_result.error_code & (1 << 1));
                else if (warning == EWarning::FilamentUnPrintableOnFirstLayer)
                    show = m_gcode_viewer.has_data() && m_gcode_viewer.filament_printable_reuslt.has_value();
            }
        }
    }

    _set_warning_notification(warning, show);
}

void GLCanvas3D::_set_warning_notification(EWarning warning, bool state)
{
    using NotificationLevel = NotificationManager::NotificationLevel;
    enum ErrorType{
        PLATER_WARNING,
        PLATER_ERROR,
        SLICING_SERIOUS_WARNING,
        SLICING_ERROR,
        SLICING_LIMIT_ERROR,
        SLICING_HEIGHT_OUTSIDE
    };
    const static std::vector<std::string> extruder_name_list= {_u8L("left nozzle"), _u8L("right nozzle")};  // in ui, we treat extruder as nozzle
    std::string text;
    ErrorType error = ErrorType::PLATER_WARNING;
    const ModelObject* conflictObj=nullptr;
    switch (warning) {
    case EWarning::GCodeConflict: {
        static std::string prevConflictText;
        text  = prevConflictText;
        error = ErrorType::SLICING_SERIOUS_WARNING;
        if (!m_gcode_viewer.m_conflict_result) { break; }
        std::string objName1 = m_gcode_viewer.m_conflict_result.value()._objName1;
        std::string objName2 = m_gcode_viewer.m_conflict_result.value()._objName2;
        double      height   = m_gcode_viewer.m_conflict_result.value()._height;
        int         layer    = m_gcode_viewer.m_conflict_result.value().layer;
        text = (boost::format(_u8L("Conflicts of G-code paths have been found at layer %d, Z = %.2lfmm. Please separate the conflicted objects farther (%s <-> %s).")) % layer %
                height % objName1 % objName2)
                   .str();
        prevConflictText        = text;
        const PrintObject *obj2 = reinterpret_cast<const PrintObject *>(m_gcode_viewer.m_conflict_result.value()._obj2);
        conflictObj             = obj2->model_object();
        break;
    }
    case EWarning::ObjectOutside:      text = _u8L("An object is laid over the plate boundaries."); break;
    case EWarning::ToolHeightOutside:  text = _u8L("A G-code path goes beyond the max print height."); error = ErrorType::SLICING_ERROR; break;
    case EWarning::ToolpathOutside:    text = _u8L("A G-code path goes beyond the plate boundaries."); error = ErrorType::SLICING_ERROR; break;
    case EWarning::TPUPrintableError: {
        text = _u8L("Not support printing 2 or more TPU filaments.");
        error = ErrorType::SLICING_ERROR;
        break;
    }
    case EWarning::FilamentPrintableError: {
        text  = filament_printable_error_msg.ToUTF8();
        error = ErrorType::SLICING_ERROR;
        break;
    }
    case EWarning::LeftExtruderPrintableError:
    case EWarning::RightExtruderPrintableError: {
        error = ErrorType::PLATER_ERROR;
        break;
    }
    case EWarning::MultiExtruderPrintableError: {
        for (auto error_iter = m_gcode_viewer.m_gcode_check_result.print_area_error_infos.begin(); error_iter != m_gcode_viewer.m_gcode_check_result.print_area_error_infos.end(); ++error_iter) {
            if (error_iter != m_gcode_viewer.m_gcode_check_result.print_area_error_infos.begin()) {
                text += "\n";
            }
            int extruder_id = error_iter->first + 1; // change extruder id to 1 based
            std::string filaments;
            std::vector<int> slice_error_object_idxs;
            for (size_t i = 0; i < error_iter->second.size(); ++i) {
                if (i > 0) {
                    filaments += ", ";
                }
                int filament_id = error_iter->second[i].first + 1; // change filament id to 1 based
                int object_label_id = error_iter->second[i].second;

                filaments += std::to_string(filament_id);
                for (int object_idx = 0; object_idx < (int) m_model->objects.size(); ++object_idx) {
                    const ModelObject *model_object = m_model->objects[object_idx];
                    for (int instance_idx = 0; instance_idx < (int) model_object->instances.size(); ++instance_idx) {
                        const ModelInstance *model_instance = model_object->instances[instance_idx];
                        auto                 expect_id      = model_instance->get_labeled_id();
                        if (object_label_id == expect_id) {
                            slice_error_object_idxs.emplace_back(object_idx);
                        }
                    }
                }
            }
            for (GLVolume *volume : m_gcode_viewer.m_shells.volumes.volumes) {
                for (auto obj_idx : slice_error_object_idxs) {
                    if (volume->object_idx() == obj_idx) {
                        volume->slice_error = true;
                        volume->selected    = true;
                    }
                }
            }
            std::string extruder_name = extruder_name_list[extruder_id-1];
            if (error_iter->second.size() == 1) {
                text += (boost::format(_u8L("Filament %s is placed in the %s, but the generated G-code path exceeds the printable range of the %s.")) %filaments %extruder_name %extruder_name).str();
            }
            else {
                text += (boost::format(_u8L("Filaments %s are placed in the %s, but the generated G-code path exceeds the printable range of the %s.")) %filaments %extruder_name %extruder_name).str();
            }
        }
        error = ErrorType::SLICING_LIMIT_ERROR;
        break;
    }
    case EWarning::MultiExtruderHeightOutside: {
        for (auto error_iter = m_gcode_viewer.m_gcode_check_result.print_height_error_infos.begin(); error_iter != m_gcode_viewer.m_gcode_check_result.print_height_error_infos.end(); ++error_iter) {
            if (error_iter != m_gcode_viewer.m_gcode_check_result.print_height_error_infos.begin()) {
                text += "\n";
            }
            int              extruder_id = error_iter->first + 1; // change extruder id to 1 based
            std::set<int>    filament_ids;
            std::vector<int> slice_error_object_idxs;
            for (size_t i = 0; i < error_iter->second.size(); ++i) {
                int filament_id     = error_iter->second[i].first + 1; // change filament id to 1 based
                int object_label_id = error_iter->second[i].second;
                filament_ids.insert(filament_id);
                for (int object_idx = 0; object_idx < (int) m_model->objects.size(); ++object_idx) {
                    const ModelObject *model_object = m_model->objects[object_idx];
                    for (int instance_idx = 0; instance_idx < (int) model_object->instances.size(); ++instance_idx) {
                        const ModelInstance *model_instance = model_object->instances[instance_idx];
                        auto                 expect_id      = model_instance->get_labeled_id();
                        if (object_label_id == expect_id) {
                            slice_error_object_idxs.emplace_back(object_idx);
                        }
                    }
                }
            }

            std::string filaments;
            int         index = 0;
            for (auto filament_id : filament_ids) {
                if (index > 0) {
                    filaments += ", ";
                }
                filaments += std::to_string(filament_id);
                ++index;
            }

            for (GLVolume *volume : m_gcode_viewer.m_shells.volumes.volumes) {
                for (auto obj_idx : slice_error_object_idxs) {
                    if (volume->object_idx() == obj_idx) {
                        volume->slice_error = true;
                        volume->selected    = true;
                    }
                }
            }
            std::string extruder_name = extruder_name_list[extruder_id-1];
            if (error_iter->second.size() == 1) {
                text += (boost::format(_u8L("Filament %s is placed in the %s, but the generated G-code path exceeds the printable height of the %s.")) % filaments % extruder_name % extruder_name).str();
            } else {
                text += (boost::format(_u8L("Filaments %s are placed in the %s, but the generated G-code path exceeds the printable height of the %s.")) % filaments % extruder_name % extruder_name).str();
            }
        }
        if (!text.empty()) {
            text += "\n";
            text += _u8L("Open wiki for more information.");
        }
        error = ErrorType::SLICING_HEIGHT_OUTSIDE;
        break;
    }

    // BBS: remove _u8L() for SLA
    case EWarning::SlaSupportsOutside: text = ("SLA supports outside the print area were detected."); error = ErrorType::PLATER_ERROR; break;
    case EWarning::SomethingNotShown:  text = _u8L("Only the object being edited is visible."); break;
    case EWarning::ObjectClashed:
        error = ErrorType::PLATER_ERROR;
        break;
    case EWarning::ObjectLimited:
        text = get_object_limited_text();
        break;
    case EWarning::FilamentUnPrintableOnFirstLayer: {
        std::string             warning;
        const std::vector<int> &conflict_filament = m_gcode_viewer.filament_printable_reuslt.conflict_filament;
        auto                    iter              = conflict_filament.begin();
        for (int filament : conflict_filament) {
            warning += std::to_string(filament + 1);
            warning += " ";
        }
        text  = (boost::format(_u8L("filaments %s cannot be printed directly on the surface of this plate.")) % warning).str();
        error = ErrorType::SLICING_ERROR;
        break;
    }
    case EWarning::MixUsePLAAndPETG:
        text = _u8L("PLA and PETG filaments detected in the mixture. Adjust parameters according to the Wiki to ensure print quality.");
        break;
    case EWarning::PrimeTowerOutside:
        text  = _u8L("The prime tower extends beyond the plate boundary.");
        break;
    case EWarning::NozzleFilamentIncompatible: {
        text = _u8L(get_nozzle_filament_incompatible_text());
        break;
    }
    case EWarning::MixtureFilamentIncompatible: {
        text = _u8L(get_filament_mixture_warning_text());
        break;
    }
    case EWarning::FlushingVolumeZero:
        text = _u8L("Partial flushing volume set to 0. Multi-color printing may cause color mixing in models. Please redjust flushing settings.");
        error = ErrorType::SLICING_ERROR;
        break;
    }
    //BBS: this may happened when exit the app, plater is null
    if (!wxGetApp().plater())
        return;
    auto& notification_manager = *wxGetApp().plater()->get_notification_manager();

    switch (error)
    {
    case PLATER_WARNING:
        if (warning == EWarning::MixUsePLAAndPETG) {
            if (state) {
                notification_manager.push_slicing_customize_error_notification(NotificationType::BBLMixUsePLAAndPETG, NotificationLevel::WarningNotificationLevel, text, _u8L("Click Wiki for help."),
                    [](wxEvtHandler*) {
                        std::string language = wxGetApp().app_config->get("language");
                        wxString    region = L"en";
                        if (language.find("zh") == 0)
                        	region = L"zh";
                        wxGetApp().open_browser_with_warning_dialog(wxString::Format(L"https://wiki.bambulab.com/%s/filament-acc/filament/h2d-pla-and-petg-mutual-support", region));
                        return false;
                    });
            }
            else
                notification_manager.close_slicing_customize_error_notification(NotificationType::BBLMixUsePLAAndPETG, NotificationLevel::WarningNotificationLevel);
        }
        else if (warning == EWarning::NozzleFilamentIncompatible){
            if(state){
                notification_manager.push_slicing_customize_error_notification(NotificationType::BBLNozzleFilamentIncompatible, NotificationLevel::WarningNotificationLevel, text);
            }
            else{
                notification_manager.close_slicing_customize_error_notification(NotificationType::BBLNozzleFilamentIncompatible, NotificationLevel::WarningNotificationLevel);
            }
        }
        else {
            if (state)
                notification_manager.push_plater_warning_notification(text);
            else
                notification_manager.close_plater_warning_notification(text);
        }
        break;
    case PLATER_ERROR:
        if (warning == EWarning::LeftExtruderPrintableError) {
            if (state) {
                if (auto left_str = get_left_extruder_unprintable_text(); !left_str.empty())
                    notification_manager.bbl_show_filament_map_invalid_notification_before_slice(NotificationType::LeftExtruderUnprintableError, left_str);
            }
            else {
                notification_manager.bbl_close_filament_map_invalid_notification_before_slice(NotificationType::LeftExtruderUnprintableError);
            }
        }
        else if (warning == EWarning::RightExtruderPrintableError) {
            if (state) {
                if (auto right_str = get_right_extruder_unprintable_text(); !right_str.empty())
                    notification_manager.bbl_show_filament_map_invalid_notification_before_slice(NotificationType::RightExtruderUnprintableError, right_str);
            }
            else {
                notification_manager.bbl_close_filament_map_invalid_notification_before_slice(NotificationType::RightExtruderUnprintableError);
            }
        }
        else if (warning == EWarning::ObjectClashed) {
            auto str = get_object_clashed_text();
            if(state){
                if (!str.empty())
                    notification_manager.push_plater_error_notification(str);
            }
            else{
                notification_manager.close_plater_error_notification(str);
            }
        }
        else {
            if (state)
                notification_manager.push_plater_error_notification(text);
            else
                notification_manager.close_plater_error_notification(text);
        }
        break;
    case SLICING_SERIOUS_WARNING:
        if (state)
            notification_manager.push_slicing_serious_warning_notification(text, conflictObj ? std::vector<ModelObject const*>{conflictObj} : std::vector<ModelObject const*>{});
        else
            notification_manager.close_slicing_serious_warning_notification(text);
        break;
    case SLICING_ERROR:
        if (warning == EWarning::FilamentUnPrintableOnFirstLayer) {
            if (state) {
                notification_manager.bbl_show_bed_filament_incompatible_notification(text);
            }
            else {
                notification_manager.bbl_close_bed_filament_incompatible_notification();
            }
        }
        if (warning == EWarning::FilamentPrintableError) {
            if (state){
                auto callback = [](wxEvtHandler*) {
                    auto plater = wxGetApp().plater();
                    auto partplate = plater->get_partplate_list().get_curr_plate();
                    try_pop_up_before_slice(false, plater, partplate, true); // ignore the return value
                    return false;
                };
                notification_manager.push_slicing_customize_error_notification(NotificationType::BBLFilamentPrintableError, NotificationLevel::ErrorNotificationLevel, text,  _u8L("Click here to regroup"), callback);
            }
            else
                notification_manager.close_slicing_customize_error_notification(NotificationType::BBLFilamentPrintableError, NotificationLevel::ErrorNotificationLevel);
        }
        else if (warning == EWarning::FlushingVolumeZero) {
            if (state) {
                auto callback = [](wxEvtHandler *) {
                    auto                              plater              = wxGetApp().plater();
                    const wxEventTypeTag<SimpleEvent> EVT_SCHEDULE_BACKGROUND_PROCESS(wxNewEventType());
                    open_flushing_dialog(plater, SimpleEvent(EVT_SCHEDULE_BACKGROUND_PROCESS, plater));
                    plater->get_view3D_canvas3D()->reload_scene(true);
                    return false;
                };
                notification_manager.push_flushing_volume_error_notification(NotificationType::BBLFlushingVolumeZero, NotificationLevel::WarningNotificationLevel, text, _u8L("Flushing Volume"), callback);
            } else
                notification_manager.close_flushing_volume_error_notification(NotificationType::BBLFlushingVolumeZero, NotificationLevel::WarningNotificationLevel);
        }
        else {
            if (state)
                notification_manager.push_slicing_error_notification(text, conflictObj ? std::vector<ModelObject const*>{conflictObj} : std::vector<ModelObject const*>{});
            else
                notification_manager.close_slicing_error_notification(text);
        }
        break;
    case SLICING_LIMIT_ERROR:
        if (state)
            notification_manager.bbl_show_filament_map_invalid_notification_after_slice(NotificationType::BBLSliceLimitError, text);
        else
            notification_manager.bbl_close_filament_map_invalid_notification_after_slice(NotificationType::BBLSliceLimitError);
        break;
    case SLICING_HEIGHT_OUTSIDE:
        if (state)
            notification_manager.push_slicing_customize_error_notification(NotificationType::BBLSliceMultiExtruderHeightOutside, NotificationLevel::ErrorNotificationLevel, text);
        else
            notification_manager.close_slicing_customize_error_notification(NotificationType::BBLSliceMultiExtruderHeightOutside, NotificationLevel::ErrorNotificationLevel);
        break;
    default:
        break;
    }
}

bool GLCanvas3D::is_flushing_matrix_error() {

    const auto                &project_config = wxGetApp().preset_bundle->project_config;
    const std::vector<double> &config_matrix  = (project_config.option<ConfigOptionFloats>("flush_volumes_matrix"))->values;
    const std::vector<double> &config_multiplier = (project_config.option<ConfigOptionFloats>("flush_multiplier"))->values;

    for (auto multiplier : config_multiplier) {
        if (multiplier == 0) return true;
    }

    int  matrix_len = config_matrix.size() / config_multiplier.size();
    int  row_len    = std::sqrt(matrix_len);
    for (int i = 0; i < config_matrix.size(); i++)
    {
        int relative_id = i % matrix_len;
        int row_id      = relative_id / row_len;
        int col_id      = relative_id % row_len;
        if (row_id != col_id && config_matrix[i] == 0) return true;
    }
    return false;
}

bool GLCanvas3D::_is_any_volume_outside() const
{
    for (const GLVolume* volume : m_volumes.volumes) {
        if (volume != nullptr && volume->is_outside)
            return true;
    }

    return false;
}

void GLCanvas3D::_update_selection_from_hover()
{
    bool ctrl_pressed = wxGetKeyState(WXK_CONTROL);

    if (m_hover_volume_idxs.empty()) {
        if (!ctrl_pressed && (m_rectangle_selection.get_state() == GLSelectionRectangle::Select))
            m_selection.remove_all();

        return;
    }

    GLSelectionRectangle::EState state = m_rectangle_selection.get_state();

    bool hover_modifiers_only = true;
    for (int i : m_hover_volume_idxs) {
        if (!m_volumes.volumes[i]->is_modifier) {
            hover_modifiers_only = false;
            break;
        }
    }

    bool selection_changed = false;
    if (state == GLSelectionRectangle::Select) {
        bool contains_all = true;
        for (int i : m_hover_volume_idxs) {
            if (!m_selection.contains_volume((unsigned int)i)) {
                contains_all = false;
                break;
            }
        }

        // the selection is going to be modified (Add)
        if (!contains_all) {
            wxGetApp().plater()->take_snapshot(std::string("Select by rectangle"), UndoRedo::SnapshotType::Selection);
            selection_changed = true;
        }
    }
    else {
        bool contains_any = false;
        for (int i : m_hover_volume_idxs) {
            if (m_selection.contains_volume((unsigned int)i)) {
                contains_any = true;
                break;
            }
        }

        // the selection is going to be modified (Remove)
        if (contains_any) {
            wxGetApp().plater()->take_snapshot(std::string("Unselect by rectangle"), UndoRedo::SnapshotType::Selection);
            selection_changed = true;
        }
    }

    if (!selection_changed)
        return;

    Plater::SuppressSnapshots suppress(wxGetApp().plater());

    if ((state == GLSelectionRectangle::Select) && !ctrl_pressed)
        m_selection.clear();

    for (int i : m_hover_volume_idxs) {
        if (state == GLSelectionRectangle::Select) {
            if (hover_modifiers_only) {
                const GLVolume& v = *m_volumes.volumes[i];
                m_selection.add_volume(v.object_idx(), v.volume_idx(), v.instance_idx(), false);
            }
            else
                m_selection.add(i, false);
        }
        else
            m_selection.remove(i);
    }

    if (m_selection.is_empty())
        m_gizmos.reset_all_states();
    else
        m_gizmos.refresh_on_off_state();

    m_gizmos.update_data();
    post_event(SimpleEvent(EVT_GLCANVAS_OBJECT_SELECT));
    m_dirty = true;
}

bool GLCanvas3D::_deactivate_arrange_menu()
{
    if (m_main_toolbar.is_item_pressed("arrange")) {
        m_main_toolbar.force_right_action(m_main_toolbar.get_item_id("arrange"), *this);
        return true;
    }

    return false;
}

//BBS: add deactivate orient menu
bool GLCanvas3D::_deactivate_orient_menu()
{
    if (m_main_toolbar.is_item_pressed("orient")) {
        m_main_toolbar.force_right_action(m_main_toolbar.get_item_id("orient"), *this);
        return true;
    }

    return false;
}

//BBS: add deactivate layersediting menu
bool GLCanvas3D::_deactivate_layersediting_menu()
{
    if (m_main_toolbar.is_item_pressed("layersediting")) {
        m_main_toolbar.force_left_action(m_main_toolbar.get_item_id("layersediting"), *this);
        return true;
    }

    return false;
}

bool GLCanvas3D::_deactivate_collapse_toolbar_items()
{
    GLToolbar& collapse_toolbar = wxGetApp().plater()->get_collapse_toolbar();
    if (collapse_toolbar.is_item_pressed("print")) {
        collapse_toolbar.force_left_action(collapse_toolbar.get_item_id("print"), *this);
        return true;
    }

    return false;
}

void GLCanvas3D::highlight_toolbar_item(const std::string& item_name)
{
    GLToolbarItem* item = m_main_toolbar.get_item(item_name);
    if (!item || !item->is_visible())
        return;
    m_toolbar_highlighter.init(item, this);
}

void GLCanvas3D::highlight_gizmo(const std::string& gizmo_name)
{
    GLGizmosManager::EType gizmo = m_gizmos.get_gizmo_from_name(gizmo_name);
    if(gizmo == GLGizmosManager::EType::Undefined)
        return;
    m_gizmo_highlighter.init(&m_gizmos, gizmo, this);
}

const Print* GLCanvas3D::fff_print() const
{
    return (m_process == nullptr) ? nullptr : m_process->fff_print();
}

const SLAPrint* GLCanvas3D::sla_print() const
{
    return (m_process == nullptr) ? nullptr : m_process->sla_print();
}

void GLCanvas3D::WipeTowerInfo::apply_wipe_tower() const
{
    // BBS: add partplate logic
    DynamicConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
    Vec3d plate_origin = wxGetApp().plater()->get_partplate_list().get_plate(m_plate_idx)->get_origin();
    ConfigOptionFloat wipe_tower_x(m_pos(X) - plate_origin(0));
    ConfigOptionFloat wipe_tower_y(m_pos(Y) - plate_origin(1));

    ConfigOptionFloats* wipe_tower_x_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_x", true);
    ConfigOptionFloats* wipe_tower_y_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_y", true);
    wipe_tower_x_opt->set_at(&wipe_tower_x, m_plate_idx, 0);
    wipe_tower_y_opt->set_at(&wipe_tower_y, m_plate_idx, 0);

    //q->update();
}

void GLCanvas3D::RenderTimer::Notify()
{
    wxPostEvent((wxEvtHandler*)GetOwner(), RenderTimerEvent( EVT_GLCANVAS_RENDER_TIMER, *this));
}

void GLCanvas3D::ToolbarHighlighterTimer::Notify()
{
    wxPostEvent((wxEvtHandler*)GetOwner(), ToolbarHighlighterTimerEvent(EVT_GLCANVAS_TOOLBAR_HIGHLIGHTER_TIMER, *this));
}

void GLCanvas3D::GizmoHighlighterTimer::Notify()
{
    wxPostEvent((wxEvtHandler*)GetOwner(), GizmoHighlighterTimerEvent(EVT_GLCANVAS_GIZMO_HIGHLIGHTER_TIMER, *this));
}

void GLCanvas3D::ToolbarHighlighter::set_timer_owner(wxEvtHandler* owner, int timerid/* = wxID_ANY*/)
{
    m_timer.SetOwner(owner, timerid);
}

void GLCanvas3D::ToolbarHighlighter::init(GLToolbarItem* toolbar_item, GLCanvas3D* canvas)
{
    if (m_timer.IsRunning())
        invalidate();
    if (!toolbar_item || !canvas)
        return;

    m_timer.Start(300, false);

    m_toolbar_item = toolbar_item;
    m_canvas       = canvas;
}

void GLCanvas3D::ToolbarHighlighter::invalidate()
{
    m_timer.Stop();

    if (m_toolbar_item) {
        m_toolbar_item->set_highlight(GLToolbarItem::EHighlightState::NotHighlighted);
    }
    m_toolbar_item = nullptr;
    m_blink_counter = 0;
    m_render_arrow = false;
}

void GLCanvas3D::ToolbarHighlighter::blink()
{
    if (m_toolbar_item) {
        char state = m_toolbar_item->get_highlight();
        if (state != (char)GLToolbarItem::EHighlightState::HighlightedShown)
            m_toolbar_item->set_highlight(GLToolbarItem::EHighlightState::HighlightedShown);
        else
            m_toolbar_item->set_highlight(GLToolbarItem::EHighlightState::HighlightedHidden);

        m_render_arrow = !m_render_arrow;
        m_canvas->set_as_dirty();
    }
    else
        invalidate();

    if ((++m_blink_counter) >= 11)
        invalidate();
}

void GLCanvas3D::GizmoHighlighter::set_timer_owner(wxEvtHandler* owner, int timerid/* = wxID_ANY*/)
{
    m_timer.SetOwner(owner, timerid);
}

void GLCanvas3D::GizmoHighlighter::init(GLGizmosManager* manager, GLGizmosManager::EType gizmo, GLCanvas3D* canvas)
{
    if (m_timer.IsRunning())
        invalidate();
    if (!gizmo || !canvas)
        return;

    m_timer.Start(300, false);

    m_gizmo_manager = manager;
    m_gizmo_type    = gizmo;
    m_canvas        = canvas;
}

void GLCanvas3D::GizmoHighlighter::invalidate()
{
    m_timer.Stop();

    if (m_gizmo_manager) {
        m_gizmo_manager->set_highlight(GLGizmosManager::EType::Undefined, false);
    }
    m_gizmo_manager = nullptr;
    m_gizmo_type = GLGizmosManager::EType::Undefined;
    m_blink_counter = 0;
    m_render_arrow = false;
}

void GLCanvas3D::GizmoHighlighter::blink()
{
    if (m_gizmo_manager) {
        if (m_blink_counter % 2 == 0)
            m_gizmo_manager->set_highlight(m_gizmo_type, true);
        else
            m_gizmo_manager->set_highlight(m_gizmo_type, false);

        m_render_arrow = !m_render_arrow;
        m_canvas->set_as_dirty();
    }
    else
        invalidate();

    if ((++m_blink_counter) >= 11)
        invalidate();
}

const ModelVolume *get_model_volume(const GLVolume &v, const Model &model)
{
    const ModelVolume * ret = nullptr;

    if (v.object_idx() < (int)model.objects.size()) {
        const ModelObject *obj = model.objects[v.object_idx()];
        if (v.volume_idx() < (int)obj->volumes.size())
            ret = obj->volumes[v.volume_idx()];
    }

    return ret;
}

ModelVolume *get_model_volume(const ObjectID &volume_id, const ModelObjectPtrs &objects)
{
    for (const ModelObject *obj : objects)
        for (ModelVolume *vol : obj->volumes)
            if (vol->id() == volume_id)
                return vol;
    return nullptr;
}

ModelVolume *get_model_volume(const GLVolume &v, const ModelObject& object) {
    if (v.volume_idx() < 0)
        return nullptr;

    size_t volume_idx = static_cast<size_t>(v.volume_idx());
    if (volume_idx >= object.volumes.size())
        return nullptr;

    return object.volumes[volume_idx];
}

ModelVolume *get_model_volume(const GLVolume &v, const ModelObjectPtrs &objects)
{
    if (v.object_idx() < 0)
        return nullptr;
    size_t objext_idx = static_cast<size_t>(v.object_idx());
    if (objext_idx >= objects.size())
        return nullptr;
    if (objects[objext_idx] == nullptr)
        return nullptr;
    return get_model_volume(v, *objects[objext_idx]);
}

GLVolume *get_first_hovered_gl_volume(const GLCanvas3D &canvas) {
    int hovered_id_signed = canvas.get_first_hover_volume_idx();
    if (hovered_id_signed < 0)
        return nullptr;

    size_t hovered_id = static_cast<size_t>(hovered_id_signed);
    const GLVolumePtrs &volumes = canvas.get_volumes().volumes;
    if (hovered_id >= volumes.size())
        return nullptr;

    return volumes[hovered_id];
}

GLVolume *get_selected_gl_volume(const GLCanvas3D &canvas) {
    const GLVolume *gl_volume = get_selected_gl_volume(canvas.get_selection());
    if (gl_volume == nullptr)
        return nullptr;

    const GLVolumePtrs &gl_volumes = canvas.get_volumes().volumes;
    for (GLVolume *v : gl_volumes)
        if (v->composite_id == gl_volume->composite_id)
            return v;
    return nullptr;
}

ModelObject *get_model_object(const GLVolume &gl_volume, const Model &model) {
    return get_model_object(gl_volume, model.objects);
}

ModelObject *get_model_object(const GLVolume &gl_volume, const ModelObjectPtrs &objects) {
    if (gl_volume.object_idx() < 0)
        return nullptr;
    size_t objext_idx = static_cast<size_t>(gl_volume.object_idx());
    if (objext_idx >= objects.size())
        return nullptr;
    return objects[objext_idx];
}

ModelInstance *get_model_instance(const GLVolume &gl_volume, const Model& model) {
    return get_model_instance(gl_volume, model.objects);
}

ModelInstance *get_model_instance(const GLVolume &gl_volume, const ModelObjectPtrs &objects) {
    if (gl_volume.instance_idx() < 0)
        return nullptr;
    ModelObject *object = get_model_object(gl_volume, objects);
    return get_model_instance(gl_volume, *object);
}

ModelInstance *get_model_instance(const GLVolume &gl_volume, const ModelObject &object) {
    if (gl_volume.instance_idx() < 0)
        return nullptr;
    size_t instance_idx = static_cast<size_t>(gl_volume.instance_idx());
    if (instance_idx >= object.instances.size())
        return nullptr;
    return object.instances[instance_idx];
}

} // namespace GUI
} // namespace Slic3r
