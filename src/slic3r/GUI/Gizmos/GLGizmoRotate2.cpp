#include "GLGizmoRotate2.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "GLGizmoMeasure.hpp"

#include <glad/gl.h>
#include <wx/glcanvas.h>
#include <imgui/imgui_internal.h>

// #define GIZMO_2_DEBUG

namespace Slic3r {
namespace GUI {
    
PickingModel GLGizmoRotate2::s_torus;
PickingModel GLGizmoRotate2::s_torus_half;

GLModel GLGizmoRotate2::s_snap_radii;
GLModel GLGizmoRotate2::s_snap_radii_fine;

constexpr double AXIS_RING_RADIUS = 225.0 / 2;
constexpr double AXIS_RING_THICKNESS = 4.0;
constexpr double AXIS_HOTSPOT_RADIUS = 12.0;
constexpr unsigned int AXIS_RING_STEP_COUNT = 120;
constexpr double AXIS_RING_STEP_RAD = 2.0f * PI / AXIS_RING_STEP_COUNT;
constexpr double AXIS_NUMBER_OFFSET = 20;

constexpr int SNAP_DEG = 5; // in degree
constexpr int SNAP_DEG_FINE = 1; // in degree
constexpr double SNAP_RADII_OFFSET = 16;
constexpr double SNAP_RADII_LEN    = 22;
constexpr double SNAP_RADII_WIDTH  = 1;

constexpr double DRAG_REF_SEGMENT_LENGTH = 12;

void render_dragging_mouse_ref(const Vec2d& mouse_pos, const Vec2d& center_pos)
{
    const ImU32 color = ImGuiWrapper::to_ImU32(GLGizmoBase::DEFAULT_DRAG_COLOR);
    constexpr double line_width = AXIS_RING_THICKNESS / 2;

    // Draw cursor reference line
    auto draw_list = ImGui::GetOverlayDrawList();
    draw_list->AddLineDashed({(float) mouse_pos.x(), (float) mouse_pos.y()}, {(float) center_pos.x(), (float) center_pos.y()},
                             color, line_width,
                             std::max(1, (int) ((mouse_pos - center_pos).norm() / DRAG_REF_SEGMENT_LENGTH)));

    // Draw arrow cursor
    const Vec2d normal = (mouse_pos - center_pos).normalized();
    const Transform2d trafo =
        Eigen::Translation2d(mouse_pos) *
        Eigen::Rotation2Dd(std::atan2(normal.y(), normal.x()));

    auto draw_line = [&trafo, &color, draw_list, line_width](const Vec2d& a, const Vec2d& b) {
        const Vec2d at = trafo * a;
        const Vec2d bt = trafo * b;
        draw_list->AddLine({(float) at.x(), (float) at.y()}, {(float) bt.x(), (float) bt.y()}, color, line_width);
    };
    draw_line({0, 6}, {0, 18});
    draw_line({0, 18}, {-6, 12});
    draw_line({0, 18}, {6, 12});
    draw_line({0, -6}, {0, -18});
    draw_line({0, -18}, {-6, -12});
    draw_line({0, -18}, {6, -12});
}

GLGizmoRotate2::GLGizmoRotate2(GLCanvas3D& parent, GLGizmoRotate::Axis axis)
    : GLGizmoBase(parent, "", -1)
    , m_axis(axis)
{
    
}

bool GLGizmoRotate2::on_init()
{
    if (!s_torus.model.is_initialized()) {
        indexed_triangle_set its = its_make_torus(AXIS_RING_RADIUS, AXIS_HOTSPOT_RADIUS, AXIS_RING_STEP_RAD);
        s_torus.model.init_from(its);
        s_torus.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
    }
    if (!s_torus_half.model.is_initialized()) {
        indexed_triangle_set its = its_make_torus(AXIS_RING_RADIUS, AXIS_HOTSPOT_RADIUS, AXIS_RING_STEP_RAD);
        its_rotate_x(its, -90);
        indexed_triangle_set upper;
        cut_mesh(its, 0.f, &upper, nullptr);
        its_rotate_x(upper, 90);
        s_torus_half.model.init_from(upper);
        s_torus_half.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(upper)));
    }

    m_axis_color = AXES_COLOR[m_axis];
    m_highlight_color = AXES_HOVER_COLOR[m_axis];

    return true;
}

void GLGizmoRotate2::on_start_dragging()
{
    init_data_from_selection(m_parent.get_selection());
    m_mouse_curr_pos = m_parent.get_local_mouse_position();

    // Find where the mouse clicked on the ring
    const Camera& camera = wxGetApp().plater()->get_camera();
    const auto raycaster = m_raycasters[1];
    Vec3f position, normal;
    const bool hit = raycaster->get_raycaster()->closest_hit(m_mouse_curr_pos, raycaster->get_transform(), camera, position, normal);
    assert(hit);
    m_drag_angle_start = 0;
    if (hit) {
        m_drag_angle_start = atan2f(position.y(), position.x());
    }

    update_center_ss();

    m_parent.set_as_dirty();
}

// Make sure angle is between [0, 2PI)
static double cap_angle(double angle)
{
    angle = std::fmod(angle, 2 * PI);
    if (angle < 0) {
        angle += 2 * PI;
    }
    return angle;
}

void GLGizmoRotate2::on_dragging(const UpdateData& data)
{
    update_center_ss();

    // Calculate the delta angle which cursor rotate around center in ccw
    const Vec2d orig_dir = (m_mouse_curr_pos - m_center_ss).normalized();
    const Vec2d new_pos  = data.mouse_pos.cast<double>();
    const Vec2d new_dir  = (new_pos - m_center_ss).normalized();
    double theta         = ::acos(std::clamp(new_dir.dot(orig_dir), -1.0, 1.0));
    if (cross2(orig_dir, new_dir) < 0.0)
        theta = -theta;

    // Revert the direction when the axis points towards the camera
    const Camera& camera = wxGetApp().plater()->get_camera();
    const Vec3d normal     = local_transform().matrix().block(0, 0, 3, 3) * Vec3d::UnitZ();
    const Vec3d camera_dir = -camera.get_dir_forward();
    if (normal.dot(camera_dir) > 0) {
        theta = -theta;
    }

    if (m_precise) {
        theta = theta / 30; // In precise mode, one rotate does 12 degrees
    }

    m_angle          = cap_angle(m_angle + theta);
    m_mouse_curr_pos = new_pos;
}

void GLGizmoRotate2::on_render()
{
    if (!m_enabled) {
        return;
    }

    const Selection& selection = m_parent.get_selection();
    if (m_hover_id != 0 && !m_dragging)
        init_data_from_selection(selection);

    const Camera& camera = wxGetApp().plater()->get_camera();
    const Transform3d& view_matrix = camera.get_view_matrix();
    const Vec3d camera_dir = -camera.get_dir_forward();

    glsafe(::glDisable(GL_DEPTH_TEST));

    // Render center indicator
    if (m_show_center || m_dragging) {
        GLShaderProgram* shader = wxGetApp().get_shader("flat");
        if (shader != nullptr) {
            shader->start_using();
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());

            Transform3d center_transform = Transform3d::Identity();
            center_transform.translate(m_center);
            // Ensure it faces the camera
            center_transform.rotate(Eigen::Quaternion<double>::FromTwoVectors(Vec3d::UnitZ(), camera_dir).toRotationMatrix());

            auto render_center = [this, shader, &center_transform, &view_matrix](const ColorRGBA& color, const float radius) {
                shader->set_uniform("view_model_matrix", view_matrix * center_transform * Geometry::scale_transform(INV_ZOOM * radius));
                render_circle(color);
            };
            
            render_center(ColorRGBA::DARK_GRAY(), 6);
            render_center(ColorRGBA::ORCA(), 4);

            shader->stop_using();
        }
    }


    Transform3d model_matrix = local_transform();

    // Make the gizmo size constant
    model_matrix.scale(INV_ZOOM * Vec3d::Ones());

    // Ring always towards camera
    const float angle_view = atan2f(camera_dir[(2 + m_axis) % 3], camera_dir[(1 + m_axis) % 3]) + PI * 0.5;
    model_matrix = model_matrix * Geometry::rotation_transform(angle_view * Vec3d::UnitZ());
    const Transform3d view_model_matrix = view_matrix * model_matrix;

    const auto full_circle = is_approx(abs(camera_dir[m_axis]), 1.);

    if (full_circle) {
        m_raycasters[0]->set_active(false);
        m_raycasters[1]->set_active(true);
    } else {
        m_raycasters[1]->set_active(false);
        m_raycasters[0]->set_active(true);
    }
    m_raycasters[0]->set_transform(model_matrix);
    m_raycasters[1]->set_transform(model_matrix);

#if SLIC3R_OPENGL_ES
    GLShaderProgram* shader = wxGetApp().get_shader("dashed_lines");
#else
    GLShaderProgram* shader = OpenGLManager::get_gl_info().is_core_profile() ? wxGetApp().get_shader("dashed_thick_lines") : wxGetApp().get_shader("flat");
#endif // SLIC3R_OPENGL_ES
    if (shader != nullptr) {
        shader->start_using();

        auto set_line_width = [shader](const float w) {
#if !SLIC3R_OPENGL_ES
            if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
                shader->set_uniform("width", w);
#if !SLIC3R_OPENGL_ES
            } else {
                glsafe(::glLineWidth(w));
            }
#endif // !SLIC3R_OPENGL_ES
        };

        shader->set_uniform("view_model_matrix", view_model_matrix);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
#if !SLIC3R_OPENGL_ES
        if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
            const std::array<int, 4>& viewport = camera.get_viewport();
            shader->set_uniform("viewport_size", Vec2d(double(viewport[2]), double(viewport[3])));
            shader->set_uniform("gap_size", 0.0f);
#if !SLIC3R_OPENGL_ES
        }
#endif // !SLIC3R_OPENGL_ES

        if (m_dragging) {
            if (!full_circle) {
                // Render the rotation axis
                set_line_width(AXIS_RING_THICKNESS / 2);

                const float far_ratio   = camera.get_far_z() / camera.get_near_z();
                const float axis_length = Vec3f(
                    camera.get_near_width() * far_ratio,
                    camera.get_near_height() * far_ratio,
                    camera.get_far_z() - camera.get_near_z()
                ).norm() * camera.get_zoom();
                if (!m_axis_line.is_initialized() || axis_length > m_old_axis_line_length) {
                    m_old_axis_line_length = axis_length;
                    m_axis_line.reset();

                    GLModel::Geometry init_data;
                    init_data.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3};
                    init_data.reserve_vertices(2);
                    init_data.reserve_indices(2);

                    // vertices
                    init_data.add_vertex(Vec3f(0.0f, 0.0f, axis_length));
                    init_data.add_vertex(Vec3f(0.0f, 0.0f, -axis_length));

                    // indices
                    init_data.add_line(0, 1);

                    m_axis_line.init_from(std::move(init_data));
                }
                m_axis_line.set_color(m_axis_color);
                m_axis_line.render();
            }

            // Render reference radius
            // The start angle radius
            shader->set_uniform("view_model_matrix", view_model_matrix * Geometry::rotation_transform(m_drag_angle_start * Vec3d::UnitZ()));
            set_line_width(AXIS_RING_THICKNESS / 4);
            render_radius(m_highlight_color);

            // The current angle radius
            shader->set_uniform("view_model_matrix", view_model_matrix * Geometry::rotation_transform((m_drag_angle_start + get_angle()) * Vec3d::UnitZ()));
            set_line_width(AXIS_RING_THICKNESS);
            render_radius(m_highlight_color);
            
            // Restore VMM
            shader->set_uniform("view_model_matrix", view_model_matrix);
        }

        set_line_width(AXIS_RING_THICKNESS);
        ColorRGBA ring_color;
        if (m_hover_id == -1) {
            glsafe(::glEnable(GL_BLEND));
            glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            ring_color = m_axis_color;
            ring_color.a(0.7);
        } else {
            ring_color = m_dragging ? DEFAULT_DRAG_COLOR : m_highlight_color;
        }
        render_ring(ring_color, full_circle || m_dragging);
        if (m_hover_id == -1) {
            glsafe(::glDisable(GL_BLEND));
        }

        if (m_dragging) {
            // Draw angle arc
            shader->set_uniform("view_model_matrix", view_model_matrix * Geometry::rotation_transform(m_drag_angle_start * Vec3d::UnitZ()));
            render_angle_arc(m_highlight_color);

            if (m_snap) {
                set_line_width(SNAP_RADII_WIDTH);
                render_snap_radii(DEFAULT_DRAG_COLOR);
            }
        }
        
        shader->stop_using();
    }

    if (m_dragging) {
        render_dragging_mouse_ref(m_mouse_curr_pos, m_center_ss);
        render_angle_number();
    }

#ifdef GIZMO_2_DEBUG
    {
        GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
        if (shader != nullptr) {
            shader->start_using();
            shader->set_uniform("emission_factor", 0.1f);
            //glsafe(::glDisable(GL_CULL_FACE));

            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            shader->set_uniform("view_model_matrix", view_model_matrix);
            const Matrix3d view_matrix_no_offset = view_matrix.matrix().block(0, 0, 3, 3);
            const Matrix3d view_normal_matrix = view_matrix_no_offset * model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
            shader->set_uniform("view_normal_matrix", view_normal_matrix);
            //if (full_circle) {
            //    s_torus.model.set_color(m_axis_color);
            //    s_torus.model.render();
            //} else {
            //    s_torus_half.model.set_color(m_axis_color);
            //    s_torus_half.model.render();
            //}
    
            //glsafe(::glEnable(GL_CULL_FACE));
            shader->stop_using();
        }

        ImGuiWrapper& imgui = *wxGetApp().imgui();
        imgui.begin(std::string("Rotate"), ImGuiWindowFlags_AlwaysAutoResize);
        char buf[1024];
        sprintf(buf, "camera_dir: %.3f, %.3f, %.3f", camera_dir.x(), camera_dir.y(), camera_dir.z());
        imgui.text(std::string(buf));
        sprintf(buf, "m_hover_id: %d", m_hover_id);
        imgui.text(std::string(buf));
        if (m_dragging) {
            sprintf(buf, "center: %f, %f, %f", m_center.x(), m_center.y(), m_center.z());
            imgui.text(std::string(buf));
            sprintf(buf, "drag_angle_start: %f", Geometry::rad2deg(m_drag_angle_start));
            imgui.text(std::string(buf));
            sprintf(buf, "mouse_curr_pos: %f, %f", m_mouse_curr_pos.x(), m_mouse_curr_pos.y());
            imgui.text(std::string(buf));

            imgui.draw_cross_hair({(float)m_center_ss.x(), (float)m_center_ss.y()});
        }
        imgui.end();
    }
#endif
}

void GLGizmoRotate2::render_ring(const ColorRGBA& color, const bool full)
{
    if (!m_ring.is_initialized()) {
        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::LineStrip, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(AXIS_RING_STEP_COUNT);
        init_data.reserve_indices(AXIS_RING_STEP_COUNT);

        // vertices + indices
        for (unsigned int i = 0; i <= AXIS_RING_STEP_COUNT; ++i) {
            const double angle = i * AXIS_RING_STEP_RAD;
            init_data.add_vertex(Vec3f(cos(angle) * AXIS_RING_RADIUS, -sin(angle) * AXIS_RING_RADIUS, 0.0f));
            init_data.add_index(i);
        }

        m_ring.init_from(std::move(init_data));
    }

    m_ring.set_color(color);
    if (full) {
        m_ring.render();
    } else {
        // Half circle
        m_ring.render(std::make_pair<size_t, size_t>(0, AXIS_RING_STEP_COUNT / 2));
    }
}

void GLGizmoRotate2::render_circle(const ColorRGBA& color)
{
    if (!m_circle.is_initialized()) {
        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::TriangleFan, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(AXIS_RING_STEP_COUNT);
        init_data.reserve_indices(AXIS_RING_STEP_COUNT);

        // vertices + indices
        for (unsigned int i = 0; i < AXIS_RING_STEP_COUNT; ++i) {
            const double angle = i * AXIS_RING_STEP_RAD;
            init_data.add_vertex(Vec3f(cos(angle), sin(angle), 0.0f));
            init_data.add_index(i);
        }

        m_circle.init_from(std::move(init_data));
    }

    m_circle.set_color(color);
    m_circle.render();
}

void GLGizmoRotate2::render_radius(const ColorRGBA& color)
{
    if (!m_reference_radius.is_initialized()) {
        GLModel::Geometry init_data;
        init_data.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3};
        init_data.reserve_vertices(2);
        init_data.reserve_indices(2);

        // vertices
        init_data.add_vertex(Vec3f(0.0f, 0.0f, 0.0f));
        init_data.add_vertex(Vec3f(AXIS_RING_RADIUS, 0.0f, 0.0f));

        // indices
        init_data.add_line(0, 1);

        m_reference_radius.init_from(std::move(init_data));
    }

    m_reference_radius.set_color(color);
    m_reference_radius.render();
}

void GLGizmoRotate2::render_angle_arc(const ColorRGBA& color)
{
    const float new_angle = get_angle();
    const float step_angle = new_angle / float(AXIS_RING_STEP_COUNT);

    const bool angle_changed = !is_approx(m_old_angle, new_angle);
    m_old_angle = new_angle;

    if (!m_angle_arc.is_initialized() || angle_changed) {
        m_angle_arc.reset();
        if (new_angle > 0.0f) {
            GLModel::Geometry init_data;
            init_data.format = { GLModel::Geometry::EPrimitiveType::LineStrip, GLModel::Geometry::EVertexLayout::P3 };
            init_data.reserve_vertices(1 + AXIS_RING_STEP_COUNT);
            init_data.reserve_indices(1 + AXIS_RING_STEP_COUNT);

            // vertices + indices
            for (unsigned int i = 0; i <= AXIS_RING_STEP_COUNT; ++i) {
                const float angle = float(i) * step_angle;
                init_data.add_vertex(Vec3f(::cos(angle) * AXIS_RING_RADIUS, ::sin(angle) * AXIS_RING_RADIUS, 0.0f));
                init_data.add_index(i);
            }

            m_angle_arc.init_from(std::move(init_data));
        }
    }

    m_angle_arc.set_color(color);
    m_angle_arc.render();
}

void GLGizmoRotate2::render_snap_radii(const ColorRGBA& color)
{
    auto generate_model = [](GLModel& m, const int step_deg) {
        constexpr float in_radius = AXIS_RING_RADIUS + SNAP_RADII_OFFSET;
        constexpr float out_radius = in_radius + SNAP_RADII_LEN;
        const int steps = 360 / step_deg;
        
        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(2 * steps);
        init_data.reserve_indices(2 * steps);
        
        // vertices + indices
        for (unsigned int i = 0; i < steps; ++i) {
            const float angle = Geometry::deg2rad(float(i * step_deg));
            const float cosa = ::cos(angle);
            const float sina = ::sin(angle);
            const float in_x = cosa * in_radius;
            const float in_y = sina * in_radius;
            const float out_x = cosa * out_radius;
            const float out_y = sina * out_radius;

            // vertices
            init_data.add_vertex(Vec3f(in_x, in_y, 0.0f));
            init_data.add_vertex(Vec3f(out_x, out_y, 0.0f));

            // indices
            init_data.add_line(i * 2, i * 2 + 1);
        }

        m.init_from(std::move(init_data));
    };

    if (m_precise) {
        if (!s_snap_radii_fine.is_initialized()) {
            generate_model(s_snap_radii_fine, SNAP_DEG_FINE);
        }

        s_snap_radii_fine.set_color(color);
        s_snap_radii_fine.render();
    } else {
        if (!s_snap_radii.is_initialized()) {
            generate_model(s_snap_radii, SNAP_DEG);
        }

        s_snap_radii.set_color(color);
        s_snap_radii.render();
    }
}


void GLGizmoRotate2::render_angle_number()
{
    static ImVec2 size(0.0f, 0.0f);
    Vec2f position((float) m_center_ss.x(), (float) (m_center_ss.y() - (AXIS_RING_RADIUS - AXIS_NUMBER_OFFSET) * (wxGetApp().em_unit() / 10.)));
    const Size cnv_size = m_parent.get_canvas_size();
    position.x() = std::clamp(position.x(), size.x / 2.f, cnv_size.get_width() - size.x / 2.f);
    position.y() = std::clamp(position.y(), size.y, (float)cnv_size.get_height());
    
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    imgui.set_next_window_pos(position.x(), position.y(), ImGuiCond_Always, 0.5f, 1.0f);

    imgui.begin(wxString("rotate_angle"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    
    std::string axis;
    switch (m_axis)
    {
    case GLGizmoRotate::Axis::X: { axis = "X"; break; }
    case GLGizmoRotate::Axis::Y: { axis = "Y"; break; }
    case GLGizmoRotate::Axis::Z: { axis = "Z"; break; }
    }
    ImGui::TextUnformatted((axis + ": " + format(float(Geometry::rad2deg(get_angle())), 2)).c_str());

    size = ImGui::GetWindowSize();
    imgui.end();
    ImGuiWrapper::pop_toolbar_style();

    if (position.x() < size.x / 2.f || position.x() > cnv_size.get_width() - size.x / 2.f ||
        position.y() < size.y || position.y() > cnv_size.get_height()) {
        imgui.set_requires_extra_frame();
    }
}

void GLGizmoRotate2::on_register_raycasters_for_picking()
{
    m_raycasters[0] = wxGetApp().plater()->canvas3D()->add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, m_axis, *s_torus_half.mesh_raycaster);
    m_raycasters[1] = wxGetApp().plater()->canvas3D()->add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, m_axis, *s_torus.mesh_raycaster);
}

void GLGizmoRotate2::on_unregister_raycasters_for_picking()
{
    wxGetApp().plater()->canvas3D()->remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo, m_axis);
    m_raycasters = {nullptr};
}

bool GLGizmoRotate2::on_mouse(const wxMouseEvent &mouse_event)
{
    return use_grabbers(mouse_event);
}

void GLGizmoRotate2::start_dragging()
{
    m_dragging = true;
    m_parent.set_cursor(GLCanvas3D::ECursorType::Blank);
    on_start_dragging();
}

void GLGizmoRotate2::stop_dragging()
{
    m_dragging = false;
    m_parent.set_cursor(GLCanvas3D::ECursorType::Standard);
    on_stop_dragging();
}

void GLGizmoRotate2::dragging(const UpdateData &data) { on_dragging(data); }

void GLGizmoRotate2::init_data_from_selection(const Selection& selection)
{
    const auto [box, box_trafo] = selection.get_bounding_box_in_current_reference_system();
    const std::pair<Vec3d, double> sphere = selection.get_bounding_sphere();
    m_center = sphere.first;
    m_orient_matrix = box_trafo;
    m_orient_matrix.translation() = m_center;
}

void GLGizmoRotate2::update_center_ss()
{
    const Camera& camera = wxGetApp().plater()->get_camera();
    const std::array<int, 4>& viewport = camera.get_viewport();
    m_center_ss = TransformHelper::world_to_ss(m_center, camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix(), viewport);
    m_center_ss.y() = viewport[3] - m_center_ss.y(); // y-axis is upside down in screen space
}

Transform3d GLGizmoRotate2::local_transform() const
{
    Transform3d ret;

    switch (m_axis)
    {
    case GLGizmoRotate::Axis::X:
    {
        ret = Geometry::rotation_transform(0.5 * PI * Vec3d::UnitY()) * Geometry::rotation_transform(0.5 * PI * Vec3d::UnitZ());
        break;
    }
    case GLGizmoRotate::Axis::Y:
    {
        ret = Geometry::rotation_transform(-0.5 * PI * Vec3d::UnitZ()) * Geometry::rotation_transform(-0.5 * PI * Vec3d::UnitY());
        break;
    }
    default:
    case GLGizmoRotate::Axis::Z:
    {
        ret = Transform3d::Identity();
        break;
    }
    }

    return m_orient_matrix * ret;
}

std::string GLGizmoRotate2::get_tooltip() const
{
    std::string axis;
    switch (m_axis)
    {
    case GLGizmoRotate::Axis::X: { axis = "X"; break; }
    case GLGizmoRotate::Axis::Y: { axis = "Y"; break; }
    case GLGizmoRotate::Axis::Z: { axis = "Z"; break; }
    }
    return (m_hover_id == 0 && !m_dragging) ? wxString::Format(_L("Drag to rotate around %s axis"),axis).utf8_string() : "";
}

void GLGizmoRotate2::set_precise(bool enabled)
{
    m_precise = enabled;
    if (m_dragging)
        m_parent.set_as_dirty();
}

void GLGizmoRotate2::set_snap(bool enabled)
{
    m_snap = enabled;
    if (m_dragging)
        m_parent.set_as_dirty();
}

void GLGizmoRotate2::set_angle(const double angle) { m_angle = cap_angle(angle); }

double GLGizmoRotate2::get_angle() const
{
    if (m_snap) {
        const int snap = m_precise ? SNAP_DEG_FINE : SNAP_DEG;

        return Geometry::deg2rad(snap * round(Geometry::rad2deg(m_angle) / snap));
    }

    return m_angle;
}


GLGizmoRotate3D2::GLGizmoRotate3D2(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id, GizmoObjectManipulation* obj_manipulation)
    : GLGizmoBase(parent, icon_filename, sprite_id)
    , m_gizmos({ 
            GLGizmoRotate2(parent, GLGizmoRotate::X), 
            GLGizmoRotate2(parent, GLGizmoRotate::Y),
            GLGizmoRotate2(parent, GLGizmoRotate::Z) })
    , m_object_manipulation(obj_manipulation)
{
    for (auto& axis : m_gizmos) {
        axis.m_show_center = axis.m_axis == GLGizmoRotate::X;
    }
}

bool GLGizmoRotate3D2::on_init()
{
    for (auto& axis : m_gizmos) {
        if (!axis.init()) {
            return false;
        }
    }

    m_shortcut_key = WXK_CONTROL_R;

    return true;
}

std::string GLGizmoRotate3D2::on_get_name() const
{
    if (!on_is_activable() && m_state == EState::Off) {
        return _u8L("Rotate") + ":\n" + _u8L("Please select at least one object.");
    } else {
        return _u8L("Rotate");
    }
}

void GLGizmoRotate3D2::on_set_state()
{
    for (auto &g : m_gizmos)
        g.set_state(m_state);
    if (get_state() == On) {
        m_object_manipulation->set_coordinates_type(ECoordinatesType::World);
    } else {
        m_last_volume = nullptr;
    }
}

bool GLGizmoRotate3D2::on_is_activable() const
{
    // BBS: don't support rotate wipe tower
    const Selection& selection = m_parent.get_selection();
    return !m_parent.get_selection().is_empty() && !selection.is_wipe_tower();
}

bool GLGizmoRotate3D2::on_mouse(const wxMouseEvent &mouse_event)
{
    if (mouse_event.Dragging() && m_dragging) {
        // Apply new temporary rotations
        TransformationType transformation_type;
        if (m_parent.get_selection().is_wipe_tower())
            transformation_type = TransformationType::World_Relative_Joint;
        else {
            switch (wxGetApp().obj_manipul()->get_coordinates_type())
            {
            default:
            case ECoordinatesType::World:    { transformation_type = TransformationType::World_Relative_Joint; break; }
            case ECoordinatesType::Instance: { transformation_type = TransformationType::Instance_Relative_Joint; break; }
            case ECoordinatesType::Local:    { transformation_type = TransformationType::Local_Relative_Joint; break; }
            }
        }
        if (mouse_event.AltDown())
            transformation_type.set_independent();
         m_parent.get_selection().rotate(get_rotation(), transformation_type);
    }
    return use_grabbers(mouse_event);
}

bool GLGizmoRotate3D2::gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    if (action == SLAGizmoEventType::ShiftDown) {
        for (auto& axis : m_gizmos)
            axis.set_precise(true);
    } else if (action == SLAGizmoEventType::ShiftUp) {
        for (auto& axis : m_gizmos)
            axis.set_precise(false);
    } else if (action == SLAGizmoEventType::CtrlDown) {
        for (auto& axis : m_gizmos)
            axis.set_snap(true);
    } else if (action == SLAGizmoEventType::CtrlUp) {
        for (auto& axis : m_gizmos)
            axis.set_snap(false);
    }

    return false;
}

void GLGizmoRotate3D2::on_start_dragging()
{
    assert(0 <= m_hover_id && m_hover_id < 3);
    m_gizmos[m_hover_id].start_dragging();
}

void GLGizmoRotate3D2::on_stop_dragging()
{
    assert(0 <= m_hover_id && m_hover_id < 3);
    m_parent.do_rotate(L("Gizmo-Rotate"));
    m_gizmos[m_hover_id].stop_dragging();
}

void GLGizmoRotate3D2::on_dragging(const UpdateData &data)
{
    assert(0 <= m_hover_id && m_hover_id < 3);
    m_gizmos[m_hover_id].dragging(data);
}

void GLGizmoRotate3D2::on_render()
{
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
    
    if (m_dragging) {
        if (0 <= m_hover_id && m_hover_id < 3) {
            m_gizmos[m_hover_id].render();
        }
    } else {
        for (auto& axis : m_gizmos) {
            axis.render();
        }
    }

#ifdef GIZMO_2_DEBUG
    {
        ImGuiWrapper& imgui = *wxGetApp().imgui();
        imgui.begin(std::string("Rotate"), ImGuiWindowFlags_AlwaysAutoResize);
        char buf[1024];
        sprintf(buf, "m_hover_id: %d", m_hover_id);
        imgui.text(std::string(buf));
        imgui.end();
    }
#endif
}

void GLGizmoRotate3D2::on_register_raycasters_for_picking()
{
    // the gizmo grabbers are rendered on top of the scene, so the raytraced picker should take it into account
    m_parent.set_raycaster_gizmos_on_top(true);
    for (GLGizmoRotate2& g : m_gizmos) {
        g.register_raycasters_for_picking();
    }
}

void GLGizmoRotate3D2::on_unregister_raycasters_for_picking()
{
    for (GLGizmoRotate2& g : m_gizmos) {
        g.unregister_raycasters_for_picking();
    }
    m_parent.set_raycaster_gizmos_on_top(false);
}

void GLGizmoRotate3D2::data_changed(bool is_serializing)
{
    const Selection &selection = m_parent.get_selection();
    const GLVolume * volume    = selection.get_first_volume();
    if (volume == nullptr) {
        m_last_volume = nullptr;
        return;
    }
    if (m_last_volume != volume) {
        m_last_volume = volume;
        Geometry::Transformation tran;
        if (selection.is_single_full_instance()) {
            tran = volume->get_instance_transformation();
        } else {
            tran = volume->get_volume_transformation();
        }
        m_object_manipulation->set_init_rotation(tran);
    }
    
    set_rotation(Vec3d::Zero());
}

std::string GLGizmoRotate3D2::get_tooltip() const {
    std::string tooltip = m_gizmos[X].get_tooltip();
    if (tooltip.empty())
        tooltip = m_gizmos[Y].get_tooltip();
    if (tooltip.empty())
        tooltip = m_gizmos[Z].get_tooltip();
    return tooltip;
}

void GLGizmoRotate3D2::on_render_input_window(float x, float y, float bottom_limit)
{
    if (m_object_manipulation)
        m_object_manipulation->do_render_rotate_window(m_imgui, "Rotate", x, y, bottom_limit);
}

} // namespace GUI
} // namespace Slic3r
