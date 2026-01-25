#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"

#include <GL/glew.h>

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_Colors.hpp"

// TODO: Display tooltips quicker on Linux

namespace Slic3r {
namespace GUI {

float GLGizmoBase::INV_ZOOM = 1.0f;


const float GLGizmoBase::Grabber::SizeFactor = 0.05f;
const float GLGizmoBase::Grabber::MinHalfSize = 4.0f;
const float GLGizmoBase::Grabber::DraggingScaleFactor = 1.25f;
const float GLGizmoBase::Grabber::FixedGrabberSize = 16.0f;
const float GLGizmoBase::Grabber::FixedRadiusSize = 80.0f;


ColorRGBA GLGizmoBase::DEFAULT_BASE_COLOR = { 0.625f, 0.625f, 0.625f, 1.0f };
ColorRGBA GLGizmoBase::DEFAULT_DRAG_COLOR = { 1.0f, 1.0f, 1.0f, 1.0f };
ColorRGBA GLGizmoBase::DEFAULT_HIGHLIGHT_COLOR = {1.0f, 0.38f, 0.0f, 1.0f};
std::array<ColorRGBA, 3> GLGizmoBase::AXES_HOVER_COLOR = {{
    {ColorRGBA::X().r() * 1.2f, ColorRGBA::X().g() * 1.4f, ColorRGBA::X().b() * 1.4f, 1.0f},
    {ColorRGBA::Y().r() * 1.2f, ColorRGBA::Y().g() * 1.2f, ColorRGBA::Y().b() * 1.2f, 1.0f},
    {ColorRGBA::Z().r() * 1.2f, ColorRGBA::Z().g() * 1.2f, ColorRGBA::Z().b() * 1.2f, 1.0f},
}};

std::array<ColorRGBA, 3> GLGizmoBase::AXES_COLOR = {{
    ColorRGBA::X(),
    ColorRGBA::Y(),
    ColorRGBA::Z()
}};

ColorRGBA            GLGizmoBase::CONSTRAINED_COLOR   = {0.5f, 0.5f, 0.5f, 1.0f};
ColorRGBA            GLGizmoBase::FLATTEN_COLOR       = {0.96f, 0.93f, 0.93f, 0.5f};
ColorRGBA            GLGizmoBase::FLATTEN_HOVER_COLOR = {1.0f, 1.0f, 1.0f, 0.75f};

// new style color
ColorRGBA            GLGizmoBase::GRABBER_NORMAL_COL        = {1.0f, 1.0f, 1.0f, 1.0f};
ColorRGBA            GLGizmoBase::GRABBER_HOVER_COL         = {0.863f, 0.125f, 0.063f, 1.0f};
ColorRGBA            GLGizmoBase::GRABBER_UNIFORM_COL       = {0, 1.0, 1.0, 1.0f};
ColorRGBA            GLGizmoBase::GRABBER_UNIFORM_HOVER_COL = {0, 0.7, 0.7, 1.0f};


void GLGizmoBase::update_render_colors()
{
    GLGizmoBase::AXES_COLOR = { {
                                ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Grabber_X]),
                                ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Grabber_Y]),
                                ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Grabber_Z])
                                } };

    GLGizmoBase::FLATTEN_COLOR       = ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Flatten_Plane]);
    GLGizmoBase::FLATTEN_HOVER_COLOR = ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Flatten_Plane_Hover]);
}

void GLGizmoBase::load_render_colors()
{
    RenderColor::colors[RenderCol_Grabber_X]           = ImGuiWrapper::to_ImVec4(GLGizmoBase::AXES_COLOR[0]);
    RenderColor::colors[RenderCol_Grabber_Y]           = ImGuiWrapper::to_ImVec4(GLGizmoBase::AXES_COLOR[1]);
    RenderColor::colors[RenderCol_Grabber_Z]           = ImGuiWrapper::to_ImVec4(GLGizmoBase::AXES_COLOR[2]);
    RenderColor::colors[RenderCol_Flatten_Plane]       = ImGuiWrapper::to_ImVec4(GLGizmoBase::FLATTEN_COLOR);
    RenderColor::colors[RenderCol_Flatten_Plane_Hover] = ImGuiWrapper::to_ImVec4(GLGizmoBase::FLATTEN_HOVER_COLOR);
}

PickingModel GLGizmoBase::Grabber::s_cube;
PickingModel GLGizmoBase::Grabber::s_cone;

GLGizmoBase::Grabber::~Grabber()
{
    //if (s_cube.model.is_initialized())
    //    s_cube.model.reset();

    //if (s_cone.model.is_initialized())
    //    s_cone.model.reset();
}

float GLGizmoBase::Grabber::get_half_size(float size) const
{
    return std::max(size * SizeFactor, MinHalfSize);
}

float GLGizmoBase::Grabber::get_dragging_half_size(float size) const
{
    return get_half_size(size) * DraggingScaleFactor;
}

PickingModel &GLGizmoBase::Grabber::get_cube()
{
    if (!s_cube.model.is_initialized()) {
        // This cannot be done in constructor, OpenGL is not yet
        // initialized at that point (on Linux at least).
        indexed_triangle_set its = its_make_cube(1.0, 1.0, 1.0);
        its_translate(its, -0.5f * Vec3f::Ones());
        s_cube.model.init_from(its);
        s_cube.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
    }
    return s_cube;
}

void GLGizmoBase::Grabber::register_raycasters_for_picking(int id)
{
    picking_id = id;
    // registration will happen on next call to render()
}

void GLGizmoBase::Grabber::unregister_raycasters_for_picking()
{
    wxGetApp().plater()->canvas3D()->remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, picking_id);
    picking_id = -1;
    raycasters = { nullptr };
}

void GLGizmoBase::Grabber::render(float size, const ColorRGBA& render_color)
{
    GLShaderProgram* shader = wxGetApp().get_current_shader();
    if (shader == nullptr)
        return;

    if (!s_cube.model.is_initialized()) {
        // This cannot be done in constructor, OpenGL is not yet
        // initialized at that point (on Linux at least).
        indexed_triangle_set its = its_make_cube(1.0, 1.0, 1.0);
        its_translate(its, -0.5f * Vec3f::Ones());
        s_cube.model.init_from(its);
        s_cube.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
    }

    if (!s_cone.model.is_initialized()) {
        indexed_triangle_set its = its_make_cone(1.0, 1.0, double(PI) / 18.0);
        s_cone.model.init_from(its);
        s_cone.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
    }

    //BBS set to fixed size grabber
    const float  grabber_size   = FixedGrabberSize * INV_ZOOM;
    const double extension_size = 0.75 * FixedGrabberSize * INV_ZOOM;

    s_cube.model.set_color(render_color);
    s_cone.model.set_color(render_color);

    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    const Transform3d& view_matrix = camera.get_view_matrix();
    const Matrix3d view_matrix_no_offset = view_matrix.matrix().block(0, 0, 3, 3);

    auto render_extension = [&view_matrix, &view_matrix_no_offset, shader, this](int idx, PickingModel &model, const Transform3d &model_matrix) {
        shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
        const Matrix3d view_normal_matrix = view_matrix_no_offset * model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);
        model.model.render();

        if (raycasters[idx] == nullptr) {
            GLCanvas3D &canvas = *wxGetApp().plater()->canvas3D();
            raycasters[idx] = canvas.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, picking_id, *model.mesh_raycaster, model_matrix);
        } else {
            raycasters[idx]->set_transform(model_matrix);
        }
    };

    if (extensions == EGrabberExtension::PosZ) {
        const Transform3d model_matrix = matrix * Geometry::assemble_transform(center, angles, Vec3d(0.75 * extension_size, 0.75 * extension_size, 2.0 * extension_size));
        render_extension(0, s_cone, model_matrix);
    } else {
        const Transform3d model_matrix = matrix * Geometry::assemble_transform(center, angles, grabber_size * Vec3d::Ones());
        render_extension(0, s_cube, model_matrix);
        
        const Transform3d extension_model_matrix_base = matrix * Geometry::assemble_transform(center, angles);
        const Vec3d extension_scale(0.75 * extension_size, 0.75 * extension_size, 3.0 * extension_size);
        if ((int(extensions) & int(GLGizmoBase::EGrabberExtension::PosX)) != 0) {
            render_extension(1, s_cone, extension_model_matrix_base * Geometry::assemble_transform(2.0 * extension_size * Vec3d::UnitX(), Vec3d(0.0, 0.5 * double(PI), 0.0), extension_scale));
        }
        if ((int(extensions) & int(GLGizmoBase::EGrabberExtension::NegX)) != 0) {
            render_extension(2, s_cone, extension_model_matrix_base * Geometry::assemble_transform(-2.0 * extension_size * Vec3d::UnitX(), Vec3d(0.0, -0.5 * double(PI), 0.0), extension_scale));
        }
        if ((int(extensions) & int(GLGizmoBase::EGrabberExtension::PosY)) != 0) {
            render_extension(3, s_cone, extension_model_matrix_base * Geometry::assemble_transform(2.0 * extension_size * Vec3d::UnitY(), Vec3d(-0.5 * double(PI), 0.0, 0.0), extension_scale));
        }
        if ((int(extensions) & int(GLGizmoBase::EGrabberExtension::NegY)) != 0) {
            render_extension(4, s_cone, extension_model_matrix_base * Geometry::assemble_transform(-2.0 * extension_size * Vec3d::UnitY(), Vec3d(0.5 * double(PI), 0.0, 0.0), extension_scale));
        }
        if ((int(extensions) & int(GLGizmoBase::EGrabberExtension::PosZ)) != 0) {
            render_extension(5, s_cone, extension_model_matrix_base * Geometry::assemble_transform(2.0 * extension_size * Vec3d::UnitZ(), Vec3d::Zero(), extension_scale));
        }
        if ((int(extensions) & int(GLGizmoBase::EGrabberExtension::NegZ)) != 0) {
            render_extension(6, s_cone, extension_model_matrix_base * Geometry::assemble_transform(-2.0 * extension_size * Vec3d::UnitZ(), Vec3d(double(PI), 0.0, 0.0), extension_scale));
        }
    }
}

bool GLGizmoBase::render_combo(const std::string &label, const std::vector<std::string> &lines, int &selection_idx, float label_width, float item_width)
{
    ImGuiWrapper::push_combo_style(m_parent.get_scale());
    ImGui::AlignTextToFramePadding();
    m_imgui->text(label);
    ImGui::SameLine(label_width);
    ImGui::PushItemWidth(item_width);

    size_t selection_out = selection_idx;

    const char *selected_str = (selection_idx >= 0 && selection_idx < int(lines.size())) ? lines[selection_idx].c_str() : "";
    if (ImGui::BBLBeginCombo(("##" + label).c_str(), selected_str, 0)) {
        for (size_t line_idx = 0; line_idx < lines.size(); ++line_idx) {
            ImGui::PushID(int(line_idx));
            if (ImGui::Selectable("", line_idx == selection_idx)) selection_out = line_idx;

            ImGui::SameLine();
            ImGui::Text("%s", lines[line_idx].c_str());
            ImGui::PopID();
        }

        ImGui::EndCombo();
    }

    bool is_changed = selection_idx != selection_out;
    selection_idx   = selection_out;

    //if (is_changed) update_connector_shape();
    ImGuiWrapper::pop_combo_style();

    return is_changed;
}

void GLGizmoBase::render_cross_mark(const Vec3f &target, bool is_single)
{
    const float half_length = 4.0f;

    // ORCA: OpenGL Core Profile
#if !SLIC3R_OPENGL_ES
    if (!OpenGLManager::get_gl_info().is_core_profile())
        glsafe(::glLineWidth(2.0f));
#endif // !SLIC3R_OPENGL_ES

    auto render_line = [](const Vec3f& p1, const Vec3f& p2, const ColorRGBA& color) {
        GLModel::Geometry init_data;
        init_data.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3};
        init_data.color  = color;
        init_data.reserve_vertices(2);
        init_data.reserve_indices(2);

        // vertices
        init_data.add_vertex(p1);
        init_data.add_vertex(p2);

        // indices
        init_data.add_line(0, 1);

        GLModel model;
        model.init_from(std::move(init_data));
        model.render();
    };

    // draw line for x axis
    if (!is_single) {
        render_line(
            {target(0) - half_length, target(1), target(2)}, 
            {target(0) + half_length, target(1), target(2)},
            ColorRGBA::X()); // ORCA match axis colors
    }
    else {
        render_line(
            {target(0), target(1), target(2)}, 
            {target(0) + half_length, target(1), target(2)},
            ColorRGBA::X()); // ORCA match axis colors
    }
    // draw line for y axis
    if (!is_single) {
        render_line(
            {target(0), target(1) - half_length, target(2)}, 
            {target(0), target(1) + half_length, target(2)},
            ColorRGBA::Y()); // ORCA match axis colors
    } else {
        render_line(
            {target(0), target(1), target(2)}, 
            {target(0), target(1) + half_length, target(2)},
            ColorRGBA::Y()); // ORCA match axis colors
    }
    // draw line for z axis
    if (!is_single) {
        render_line(
            {target(0), target(1), target(2) - half_length}, 
            {target(0), target(1), target(2) + half_length},
            ColorRGBA::Z()); // ORCA match axis colors
    } else {
        render_line(
            {target(0), target(1), target(2)}, 
            {target(0), target(1), target(2) + half_length},
            ColorRGBA::Z()); // ORCA match axis colors
    }
}

GLGizmoBase::GLGizmoBase(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : m_parent(parent)
    , m_group_id(-1)
    , m_state(Off)
    , m_shortcut_key(NO_SHORTCUT_KEY_VALUE)
    , m_icon_filename(icon_filename)
    , m_sprite_id(sprite_id)
    , m_imgui(wxGetApp().imgui())
{
}


std::string GLGizmoBase::get_action_snapshot_name() const
{
    return "Gizmo action";
}

void GLGizmoBase::set_icon_filename(const std::string &filename) {
    m_icon_filename = filename;
}

void GLGizmoBase::set_hover_id(int id)
{
    // do not change hover id during dragging
    assert(!m_dragging);

    // allow empty grabbers when not using grabbers but use hover_id - flatten, rotate
//    if (!m_grabbers.empty() && id >= (int) m_grabbers.size())
//        return;
    
    m_hover_id = id;
    on_set_hover_id();    
}

bool GLGizmoBase::update_items_state()
{
    bool res = m_dirty;
    m_dirty  = false;
    return res;
}

bool GLGizmoBase::GizmoImguiBegin(const std::string &name, int flags)
{
    return m_imgui->begin(name, flags);
}

void GLGizmoBase::GizmoImguiEnd()
{
    last_input_window_width = ImGui::GetWindowWidth();
    m_imgui->end();
}

void GLGizmoBase::GizmoImguiSetNextWIndowPos(float &x, float y, int flag, float pivot_x, float pivot_y)
{
    GizmoImguiSetNextWIndowPos(x, y, last_input_window_width, 0, flag, pivot_x, pivot_y);
}

void GLGizmoBase::GizmoImguiSetNextWIndowPos(float &x, float y, float w, float h, int flag, float pivot_x, float pivot_y)
{
    if (abs(w) > 0.01f) {
        if (x + w > m_parent.get_canvas_size().get_width()) {
            if (w > m_parent.get_canvas_size().get_width()) {
                x = 0;
            } else {
                x = m_parent.get_canvas_size().get_width() - w;
            }
        }
    }

    m_imgui->set_next_window_pos(x, y, flag, pivot_x, pivot_y);
}

void GLGizmoBase::register_grabbers_for_picking()
{
    for (size_t i = 0; i < m_grabbers.size(); ++i) {
        m_grabbers[i].register_raycasters_for_picking((m_group_id >= 0) ? m_group_id : i);
    }
}

void GLGizmoBase::unregister_grabbers_for_picking()
{
    for (size_t i = 0; i < m_grabbers.size(); ++i) {
        m_grabbers[i].unregister_raycasters_for_picking();
    }
}

void GLGizmoBase::render_grabbers(const BoundingBoxf3& box) const
{
#if ENABLE_FIXED_GRABBER
    render_grabbers((float)(GLGizmoBase::Grabber::FixedGrabberSize));
#else
    render_grabbers((float)((box.size().x() + box.size().y() + box.size().z()) / 3.0));
#endif
}

void GLGizmoBase::render_grabbers(float size) const
{
    render_grabbers(0, m_grabbers.size() - 1, size, false);
}

void GLGizmoBase::render_grabbers(size_t first, size_t last, float size, bool force_hover) const
{
    GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;
    shader->start_using();
    shader->set_uniform("emission_factor", 0.1f);
    glsafe(::glDisable(GL_CULL_FACE));
    for (size_t i = first; i <= last; ++i) {
        if (m_grabbers[i].enabled)
            m_grabbers[i].render(force_hover ? true : m_hover_id == (int)i, size);
    }
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

// help function to process grabbers
// call start_dragging, stop_dragging, on_dragging
bool GLGizmoBase::use_grabbers(const wxMouseEvent &mouse_event) {
    bool is_dragging_finished = false;
    if (mouse_event.Moving()) { 
        // it should not happen but for sure
        assert(!m_dragging);
        if (m_dragging) is_dragging_finished = true;
        else return false; 
    } 

    if (mouse_event.LeftDown()) {
        Selection &selection = m_parent.get_selection();
        if (!selection.is_empty() && m_hover_id != -1 /* &&
            (m_grabbers.empty() || m_hover_id < static_cast<int>(m_grabbers.size()))*/) {
            selection.setup_cache();

            m_dragging = true;
            for (auto &grabber : m_grabbers) grabber.dragging = false;
//            if (!m_grabbers.empty() && m_hover_id < int(m_grabbers.size()))
//                m_grabbers[m_hover_id].dragging = true;
            
            on_start_dragging();

            // Let the plater know that the dragging started
            m_parent.post_event(SimpleEvent(EVT_GLCANVAS_MOUSE_DRAGGING_STARTED));
            m_parent.set_as_dirty();
            return true;
        }
    } else if (m_dragging) {
        // when mouse cursor leave window than finish actual dragging operation
        bool is_leaving = mouse_event.Leaving();
        if (mouse_event.Dragging()) {
            Point      mouse_coord(mouse_event.GetX(), mouse_event.GetY());
            auto       ray = m_parent.mouse_ray(mouse_coord);
            UpdateData data(ray, mouse_coord);

            on_dragging(data);

            wxGetApp().obj_manipul()->set_dirty();
            m_parent.set_as_dirty();
            return true;
        }
        else if (mouse_event.LeftUp() || is_leaving || is_dragging_finished) {
            do_stop_dragging(is_leaving);
            return true;
        }
    }
    return false;
}

void GLGizmoBase::do_stop_dragging(bool perform_mouse_cleanup)
{
    for (auto& grabber : m_grabbers) grabber.dragging = false;
    m_dragging = false;

    // NOTE: This should be part of GLCanvas3D
    // Reset hover_id when leave window
    if (perform_mouse_cleanup) m_parent.mouse_up_cleanup();

    on_stop_dragging();

    // There is prediction that after draggign, data are changed
    // Data are updated twice also by canvas3D::reload_scene.
    // Should be fixed.
    m_parent.get_gizmos_manager().update_data();

    wxGetApp().obj_manipul()->set_dirty();

    // Let the plater know that the dragging finished, so a delayed
    // refresh of the scene with the background processing data should
    // be performed.
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED));
    // updates camera target constraints
    m_parent.refresh_camera_scene_box();
}

std::string GLGizmoBase::format(float value, unsigned int decimals) const
{
    return Slic3r::string_printf("%.*f", decimals, value);
}

void GLGizmoBase::set_dirty() {
    m_dirty = true;
}

void GLGizmoBase::render_input_window(float x, float y, float bottom_limit)
{
    on_render_input_window(x, y, bottom_limit);
    if (m_first_input_window_render) {
        // imgui windows that don't have an initial size needs to be processed once to get one
        // and are not rendered in the first frame
        // so, we forces to render another frame the first time the imgui window is shown
        // https://github.com/ocornut/imgui/issues/2949
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
        m_first_input_window_render = false;
    }
}



std::string GLGizmoBase::get_name(bool include_shortcut) const
{
    int key = get_shortcut_key();
    std::string out = on_get_name();
    if (include_shortcut && key >= WXK_CONTROL_A && key <= WXK_CONTROL_Z)
        out += std::string(" [") + char(int('A') + key - int(WXK_CONTROL_A)) + "]";
    return out;
}

} // namespace GUI
} // namespace Slic3r
