#include "GLGizmoRotate.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Jobs/RotoptimizeJob.hpp"

#include "libslic3r/PresetBundle.hpp"

#include <GL/glew.h>

namespace Slic3r {
namespace GUI {


const float GLGizmoRotate::Offset = 5.0f;
const unsigned int GLGizmoRotate::AngleResolution = 64;
const unsigned int GLGizmoRotate::ScaleStepsCount = 72;
const float GLGizmoRotate::ScaleStepRad = 2.0f * float(PI) / GLGizmoRotate::ScaleStepsCount;
const unsigned int GLGizmoRotate::ScaleLongEvery = 2;
const float GLGizmoRotate::ScaleLongTooth = 0.1f; // in percent of radius
const unsigned int GLGizmoRotate::SnapRegionsCount = 8;
const float GLGizmoRotate::GrabberOffset = 0.15f; // in percent of radius

GLGizmoRotate::GLGizmoRotate(GLCanvas3D& parent, GLGizmoRotate::Axis axis)
    : GLGizmoBase(parent, "", -1)
    , m_axis(axis)
    , m_angle(0.0)
    , m_center(0.0, 0.0, 0.0)
    , m_radius(0.0f)
    , m_snap_coarse_in_radius(0.0f)
    , m_snap_coarse_out_radius(0.0f)
    , m_snap_fine_in_radius(0.0f)
    , m_snap_fine_out_radius(0.0f)
    , m_drag_color(DEFAULT_DRAG_COLOR)
    , m_highlight_color(DEFAULT_HIGHLIGHT_COLOR)
{
    m_group_id = static_cast<int>(axis);
}

void GLGizmoRotate::set_highlight_color(const ColorRGBA &color)
{
    m_highlight_color = color;
}

void GLGizmoRotate::set_angle(double angle)
{
    if (std::abs(angle - 2.0 * double(PI)) < EPSILON)
        angle = 0.0;

    m_angle = angle;
}

std::string GLGizmoRotate::get_tooltip() const
{
    std::string axis;
    switch (m_axis)
    {
    case X: { axis = "X"; break; }
    case Y: { axis = "Y"; break; }
    case Z: { axis = "Z"; break; }
    }
    return (m_hover_id == 0 || m_grabbers.front().dragging) ? axis + ": " + format(float(Geometry::rad2deg(m_angle)), 2) : "";
}

bool GLGizmoRotate::on_mouse(const wxMouseEvent &mouse_event)
{
    return use_grabbers(mouse_event);
}

void GLGizmoRotate::dragging(const UpdateData &data) { on_dragging(data); }

void GLGizmoRotate::start_dragging()
{
    m_grabbers[0].dragging = true;
    on_start_dragging();
}

void GLGizmoRotate::stop_dragging()
{
    m_grabbers[0].dragging = false;
    on_stop_dragging();
}

void GLGizmoRotate::enable_grabber() { m_grabbers[0].enabled = true; }
void GLGizmoRotate::disable_grabber() { m_grabbers[0].enabled = false; }

bool GLGizmoRotate::on_init()
{
    m_grabbers.push_back(Grabber());
    m_grabbers.back().extensions = (GLGizmoBase::EGrabberExtension)(int(GLGizmoBase::EGrabberExtension::PosY) | int(GLGizmoBase::EGrabberExtension::NegY));
    return true;
}

void GLGizmoRotate::on_start_dragging()
{
    init_data_from_selection(m_parent.get_selection());
}

void GLGizmoRotate::on_dragging(const UpdateData &data)
{
    const Vec2d mouse_pos = to_2d(mouse_position_in_local_plane(data.mouse_ray));
    const Vec2d orig_dir = Vec2d::UnitX();
    const Vec2d new_dir = mouse_pos.normalized();

    double theta = ::acos(std::clamp(new_dir.dot(orig_dir), -1.0, 1.0));
    if (cross2(orig_dir, new_dir) < 0.0)
        theta = 2.0 * (double)PI - theta;

    const double len = mouse_pos.norm();

    // snap to coarse snap region
    if (m_snap_coarse_in_radius <= len && len <= m_snap_coarse_out_radius) {
        const double step = 2.0 * double(PI) / double(SnapRegionsCount);
        theta = step * std::round(theta / step);
    }
    else {
        // snap to fine snap region (scale)
        if (m_snap_fine_in_radius <= len && len <= m_snap_fine_out_radius) {
            const double step = 2.0 * double(PI) / double(ScaleStepsCount);
            theta = step * std::round(theta / step);
        }
    }

    if (theta == 2.0 * double(PI))
        theta = 0.0;

    m_angle = theta;
}

void GLGizmoRotate::on_render()
{
    if (!m_grabbers.front().enabled)
        return;

    const Selection& selection = m_parent.get_selection();
    if (m_hover_id != 0 && !m_grabbers.front().dragging)
        init_data_from_selection(selection);

    const double grabber_radius = (double)m_radius * (1.0 + (double)GrabberOffset);
    m_grabbers.front().center = Vec3d(::cos(m_angle) * grabber_radius, ::sin(m_angle) * grabber_radius, 0.0);
    m_grabbers.front().angles.z() = m_angle;
    m_grabbers.front().color = AXES_COLOR[m_axis];
    m_grabbers.front().hover_color = AXES_HOVER_COLOR[m_axis];

    glsafe(::glEnable(GL_DEPTH_TEST));

    m_grabbers.front().matrix = local_transform(selection);

#if !SLIC3R_OPENGL_ES
    if (!OpenGLManager::get_gl_info().is_core_profile())
        glsafe(::glLineWidth((m_hover_id != -1) ? 2.0f : 1.5f));
#endif // !SLIC3R_OPENGL_ES

#if SLIC3R_OPENGL_ES
    GLShaderProgram* shader = wxGetApp().get_shader("dashed_lines");
#else
    GLShaderProgram* shader = OpenGLManager::get_gl_info().is_core_profile() ? wxGetApp().get_shader("dashed_thick_lines") : wxGetApp().get_shader("flat");
#endif // SLIC3R_OPENGL_ES
    if (shader != nullptr) {
        shader->start_using();

        const Camera& camera = wxGetApp().plater()->get_camera();
        const Transform3d view_model_matrix = camera.get_view_matrix() * m_grabbers.front().matrix;
        shader->set_uniform("view_model_matrix", view_model_matrix);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
#if !SLIC3R_OPENGL_ES
        if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
            const std::array<int, 4>& viewport = camera.get_viewport();
            shader->set_uniform("viewport_size", Vec2d(double(viewport[2]), double(viewport[3])));
            shader->set_uniform("width", 0.25f);
            shader->set_uniform("gap_size", 0.0f);
#if !SLIC3R_OPENGL_ES
        }
#endif // !SLIC3R_OPENGL_ES

        const bool radius_changed = std::abs(m_old_radius - m_radius) > EPSILON;
        m_old_radius = m_radius;

        const ColorRGBA color = (m_hover_id != -1) ? m_drag_color : m_highlight_color;
        render_circle(color, radius_changed);
        if (m_hover_id != -1) {
            const bool hover_radius_changed = std::abs(m_old_hover_radius - m_radius) > EPSILON;
            m_old_hover_radius = m_radius;

            render_scale(color, hover_radius_changed);
            render_snap_radii(color, hover_radius_changed);
            render_reference_radius(color, hover_radius_changed);
            render_angle_arc(m_highlight_color, hover_radius_changed);
        }

        // ORCA dont use axis color on line because they are not on same direction with axis
        const ColorRGBA line_color = (m_hover_id != -1) ? m_drag_color : ColorRGBA(.6f, .6f ,.6f, 1.f);
        render_grabber_connection(line_color, radius_changed); 
        shader->stop_using();
    }

    render_grabber(m_bounding_box);
}

void GLGizmoRotate::init_data_from_selection(const Selection& selection)
{
    const auto [box, box_trafo] = m_force_local_coordinate ?
        selection.get_bounding_box_in_reference_system(ECoordinatesType::Local) : selection.get_bounding_box_in_current_reference_system();
    m_bounding_box = box;
    const std::pair<Vec3d, double> sphere = selection.get_bounding_sphere();
    m_center = sphere.first;
    m_radius = Offset + sphere.second;
    m_orient_matrix = box_trafo;
    m_orient_matrix.translation() = m_center;
    m_snap_coarse_in_radius = m_radius / 3.0f;
    m_snap_coarse_out_radius = 2.0f * m_snap_coarse_in_radius;
    m_snap_fine_in_radius = m_radius;
    m_snap_fine_out_radius = m_snap_fine_in_radius + m_radius * ScaleLongTooth;
}

//BBS: add input window for move
void GLGizmoRotate3D::on_render_input_window(float x, float y, float bottom_limit)
{
    //if (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptSLA)
    //    return;
    if (m_object_manipulation)
        m_object_manipulation->do_render_rotate_window(m_imgui, "Rotate", x, y, bottom_limit);
    //RotoptimzeWindow popup{m_imgui, m_rotoptimizewin_state, {x, y, bottom_limit}};
}

void GLGizmoRotate3D::load_rotoptimize_state()
{
    std::string accuracy_str =
        wxGetApp().app_config->get("sla_auto_rotate", "accuracy");

    std::string method_str =
        wxGetApp().app_config->get("sla_auto_rotate", "method_id");

    if (!accuracy_str.empty()) {
        float accuracy = std::stof(accuracy_str);
        accuracy = std::max(0.f, std::min(accuracy, 1.f));

        m_rotoptimizewin_state.accuracy = accuracy;
    }

    if (!method_str.empty()) {
        int method_id = std::stoi(method_str);
        if (method_id < int(RotoptimizeJob::get_methods_count()))
            m_rotoptimizewin_state.method_id = method_id;
    }
}

void GLGizmoRotate::render_circle(const ColorRGBA& color, bool radius_changed)
{
    if (!m_circle.is_initialized() || radius_changed) {
        m_circle.reset();

        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::LineLoop, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(ScaleStepsCount);
        init_data.reserve_indices(ScaleStepsCount);

        // vertices + indices
        for (unsigned int i = 0; i < ScaleStepsCount; ++i) {
            const float angle = float(i * ScaleStepRad);
            init_data.add_vertex(Vec3f(::cos(angle) * m_radius, ::sin(angle) * m_radius, 0.0f));
            init_data.add_index(i);
        }

        m_circle.init_from(std::move(init_data));
    }

    m_circle.set_color(color);
    m_circle.render();
}

void GLGizmoRotate::render_scale(const ColorRGBA& color, bool radius_changed)
{
    const float out_radius_long = m_snap_fine_out_radius;
    const float out_radius_short = m_radius * (1.0f + 0.5f * ScaleLongTooth);

    if (!m_scale.is_initialized() || radius_changed) {
        m_scale.reset();

        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(2 * ScaleStepsCount);
        init_data.reserve_indices(2 * ScaleStepsCount);

        // vertices + indices
        for (unsigned int i = 0; i < ScaleStepsCount; ++i) {
            const float angle = float(i * ScaleStepRad);
            const float cosa = ::cos(angle);
            const float sina = ::sin(angle);
            const float in_x = cosa * m_radius;
            const float in_y = sina * m_radius;
            const float out_x = (i % ScaleLongEvery == 0) ? cosa * out_radius_long : cosa * out_radius_short;
            const float out_y = (i % ScaleLongEvery == 0) ? sina * out_radius_long : sina * out_radius_short;

            // vertices
            init_data.add_vertex(Vec3f(in_x, in_y, 0.0f));
            init_data.add_vertex(Vec3f(out_x, out_y, 0.0f));

            // indices
            init_data.add_line(i * 2, i * 2 + 1);
        }

        m_scale.init_from(std::move(init_data));
    }

    m_scale.set_color(color);
    m_scale.render();
}

void GLGizmoRotate::render_snap_radii(const ColorRGBA& color, bool radius_changed)
{
    const float step = 2.0f * float(PI) / float(SnapRegionsCount);
    const float in_radius = m_radius / 3.0f;
    const float out_radius = 2.0f * in_radius;

    if (!m_snap_radii.is_initialized() || radius_changed) {
        m_snap_radii.reset();

        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(2 * ScaleStepsCount);
        init_data.reserve_indices(2 * ScaleStepsCount);

        // vertices + indices
        for (unsigned int i = 0; i < ScaleStepsCount; ++i) {
            const float angle = float(i * step);
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

        m_snap_radii.init_from(std::move(init_data));
    }

    m_snap_radii.set_color(color);
    m_snap_radii.render();
}

void GLGizmoRotate::render_reference_radius(const ColorRGBA& color, bool radius_changed)
{
    if (!m_reference_radius.is_initialized() || radius_changed) {
        m_reference_radius.reset();

        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(2);
        init_data.reserve_indices(2);

        // vertices
        init_data.add_vertex(Vec3f(0.0f, 0.0f, 0.0f));
        init_data.add_vertex(Vec3f(m_radius * (1.0f + GrabberOffset), 0.0f, 0.0f));

        // indices
        init_data.add_line(0, 1);

        m_reference_radius.init_from(std::move(init_data));
    }

    m_reference_radius.set_color(color);
    m_reference_radius.render();
}

void GLGizmoRotate::render_angle_arc(const ColorRGBA& color, bool radius_changed)
{
    const float step_angle = float(m_angle) / float(AngleResolution);
    const float ex_radius = m_radius * (1.0f + GrabberOffset);

    const bool angle_changed = std::abs(m_old_angle - m_angle) > EPSILON;
    m_old_angle = m_angle;

    if (!m_angle_arc.is_initialized() || radius_changed || angle_changed) {
        m_angle_arc.reset();
        if (m_angle > 0.0f) {
            GLModel::Geometry init_data;
            init_data.format = { GLModel::Geometry::EPrimitiveType::LineStrip, GLModel::Geometry::EVertexLayout::P3 };
            init_data.reserve_vertices(1 + AngleResolution);
            init_data.reserve_indices(1 + AngleResolution);

            // vertices + indices
            for (unsigned int i = 0; i <= AngleResolution; ++i) {
                const float angle = float(i) * step_angle;
                init_data.add_vertex(Vec3f(::cos(angle) * ex_radius, ::sin(angle) * ex_radius, 0.0f));
                init_data.add_index(i);
            }

            m_angle_arc.init_from(std::move(init_data));
        }
    }

    m_angle_arc.set_color(color);
    m_angle_arc.render();
}

void GLGizmoRotate::render_grabber_connection(const ColorRGBA& color, bool radius_changed)
{
    if (!m_grabber_connection.model.is_initialized() || radius_changed || !m_grabber_connection.old_center.isApprox(m_grabbers.front().center)) {
        m_grabber_connection.model.reset();
        m_grabber_connection.old_center = m_grabbers.front().center;

        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(2);
        init_data.reserve_indices(2);

        // vertices
        init_data.add_vertex(Vec3f(0.0f, 0.0f, 0.0f));
        init_data.add_vertex((Vec3f)m_grabbers.front().center.cast<float>());

        // indices
        init_data.add_line(0, 1);

        m_grabber_connection.model.init_from(std::move(init_data));
    }

    m_grabber_connection.model.set_color(color);
    m_grabber_connection.model.render();
}

void GLGizmoRotate::render_grabber(const BoundingBoxf3& box)
{
    m_grabbers.front().color = m_highlight_color;
    render_grabbers(box);
}

Transform3d GLGizmoRotate::local_transform(const Selection& selection) const
{
    Transform3d ret;

    switch (m_axis)
    {
    case X:
    {
        ret = Geometry::rotation_transform(0.5 * PI * Vec3d::UnitY()) * Geometry::rotation_transform(-0.5 * PI * Vec3d::UnitZ());
        break;
    }
    case Y:
    {
        ret = Geometry::rotation_transform(-0.5 * PI * Vec3d::UnitZ()) * Geometry::rotation_transform(-0.5 * PI * Vec3d::UnitY());
        break;
    }
    default:
    case Z:
    {
        ret = Transform3d::Identity();
        break;
    }
    }

    return m_orient_matrix * ret;
}

Vec3d GLGizmoRotate::mouse_position_in_local_plane(const Linef3& mouse_ray) const
{
    const double half_pi = 0.5 * double(PI);

    Transform3d m = Transform3d::Identity();

    switch (m_axis)
    {
    case X:
    {
        m.rotate(Eigen::AngleAxisd(half_pi, Vec3d::UnitZ()));
        m.rotate(Eigen::AngleAxisd(-half_pi, Vec3d::UnitY()));
        break;
    }
    case Y:
    {
        m.rotate(Eigen::AngleAxisd(half_pi, Vec3d::UnitY()));
        m.rotate(Eigen::AngleAxisd(half_pi, Vec3d::UnitZ()));
        break;
    }
    default:
    case Z:
    {
        // no rotation applied
        break;
    }
    }

    m = m * Geometry::Transformation(m_orient_matrix).get_matrix_no_offset().inverse();

    m.translate(-m_center);

    const Linef3 local_mouse_ray = transform(mouse_ray, m);
    if (std::abs(local_mouse_ray.vector().dot(Vec3d::UnitZ())) < EPSILON) {
        // if the ray is parallel to the plane containing the circle
        if (std::abs(local_mouse_ray.vector().dot(Vec3d::UnitY())) > 1.0 - EPSILON)
            // if the ray is parallel to grabber direction
            return Vec3d::UnitX();
        else {
            const Vec3d world_pos = (local_mouse_ray.a.x() >= 0.0) ? mouse_ray.a - m_center : mouse_ray.b - m_center;
            m.translate(m_center);
            return m * world_pos;
        }
    }
    else
        return local_mouse_ray.intersect_plane(0.0);
}

//BBS: GUI refactor: add obj manipulation
GLGizmoRotate3D::GLGizmoRotate3D(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id, GizmoObjectManipulation* obj_manipulation)
    : GLGizmoBase(parent, icon_filename, sprite_id)
    , m_gizmos({ 
        GLGizmoRotate(parent, GLGizmoRotate::X), 
        GLGizmoRotate(parent, GLGizmoRotate::Y),
        GLGizmoRotate(parent, GLGizmoRotate::Z) })
	//BBS: GUI refactor: add obj manipulation
    , m_object_manipulation(obj_manipulation)
{
    load_rotoptimize_state();
}

bool GLGizmoRotate3D::on_mouse(const wxMouseEvent &mouse_event)
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

bool GLGizmoRotate3D::on_init()
{
    for (GLGizmoRotate& g : m_gizmos) 
        if (!g.init()) return false;

    for (unsigned int i = 0; i < 3; ++i)
        m_gizmos[i].set_highlight_color(AXES_COLOR[i]);

    m_shortcut_key = WXK_CONTROL_R;

    return true;
}

std::string GLGizmoRotate3D::on_get_name() const
{
    if (!on_is_activable() && m_state == EState::Off) {
        return _u8L("Rotate") + ":\n" + _u8L("Please select at least one object.");
    } else {
        return _u8L("Rotate");
    }
}

void GLGizmoRotate3D::on_set_state()
{
    for (GLGizmoRotate &g : m_gizmos)
        g.set_state(m_state);
    if (get_state() == On) {
        m_object_manipulation->set_coordinates_type(ECoordinatesType::World);
    } else {
        m_last_volume = nullptr;
    }
}

void GLGizmoRotate3D::data_changed(bool is_serializing) {
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

    bool is_wipe_tower = selection.is_wipe_tower();
    if (is_wipe_tower) {
        DynamicPrintConfig& config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        float wipe_tower_rotation_angle =
            dynamic_cast<const ConfigOptionFloat *>(
                config.option("wipe_tower_rotation_angle"))
                ->value;
        set_rotation(Vec3d(0., 0., (M_PI / 180.) * wipe_tower_rotation_angle));
        m_gizmos[0].disable_grabber();
        m_gizmos[1].disable_grabber();
    } else {
        set_rotation(Vec3d::Zero());
        m_gizmos[0].enable_grabber();
        m_gizmos[1].enable_grabber();
    }
}

bool GLGizmoRotate3D::on_is_activable() const
{
    // BBS: don't support rotate wipe tower
    const Selection& selection = m_parent.get_selection();
    return !m_parent.get_selection().is_empty() && !selection.is_wipe_tower();
}

void GLGizmoRotate3D::on_start_dragging()
{
    assert(0 <= m_hover_id && m_hover_id < 3);
    m_gizmos[m_hover_id].start_dragging();
}

void GLGizmoRotate3D::on_stop_dragging()
{
    assert(0 <= m_hover_id && m_hover_id < 3);
    m_parent.do_rotate(L("Gizmo-Rotate"));
    m_gizmos[m_hover_id].stop_dragging();
}

void GLGizmoRotate3D::on_dragging(const UpdateData &data)
{
    assert(0 <= m_hover_id && m_hover_id < 3);
    m_gizmos[m_hover_id].dragging(data);
}

void GLGizmoRotate3D::on_render()
{
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));

    if (m_hover_id == -1 || m_hover_id == 0)
        m_gizmos[X].render();

    if (m_hover_id == -1 || m_hover_id == 1)
        m_gizmos[Y].render();

    if (m_hover_id == -1 || m_hover_id == 2)
        m_gizmos[Z].render();
}

void GLGizmoRotate3D::on_register_raycasters_for_picking()
{
    // the gizmo grabbers are rendered on top of the scene, so the raytraced picker should take it into account
    m_parent.set_raycaster_gizmos_on_top(true);
    for (GLGizmoRotate& g : m_gizmos) {
        g.register_raycasters_for_picking();
    }
}

void GLGizmoRotate3D::on_unregister_raycasters_for_picking()
{
    for (GLGizmoRotate& g : m_gizmos) {
        g.unregister_raycasters_for_picking();
    }
    m_parent.set_raycaster_gizmos_on_top(false);
}

GLGizmoRotate3D::RotoptimzeWindow::RotoptimzeWindow(ImGuiWrapper *   imgui,
                                                    State &          state,
                                                    const Alignment &alignment)
    : m_imgui{imgui}
{
    imgui->begin(_L("Optimize orientation"), ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoCollapse);

    // adjust window position to avoid overlap the view toolbar
    float win_h = ImGui::GetWindowHeight();
    float x = alignment.x, y = alignment.y;
    y = std::min(y, alignment.bottom_limit - win_h);
    ImGui::SetWindowPos(ImVec2(x, y), ImGuiCond_Always);

    float max_text_w = 0.;
    auto padding = ImGui::GetStyle().FramePadding;
    padding.x *= 2.f;
    padding.y *= 2.f;

    for (size_t i = 0; i < RotoptimizeJob::get_methods_count(); ++i) {
        float w =
            ImGui::CalcTextSize(RotoptimizeJob::get_method_name(i).c_str()).x +
            padding.x + ImGui::GetFrameHeight();
        max_text_w = std::max(w, max_text_w);
    }

    ImGui::PushItemWidth(max_text_w);

    if (ImGui::BeginCombo("", RotoptimizeJob::get_method_name(state.method_id).c_str())) {
        for (size_t i = 0; i < RotoptimizeJob::get_methods_count(); ++i) {
            if (ImGui::Selectable(RotoptimizeJob::get_method_name(i).c_str())) {
                state.method_id = i;
#ifdef SUPPORT_SLA_AUTO_ROTATE
                wxGetApp().app_config->set("sla_auto_rotate",
                                           "method_id",
                                           std::to_string(state.method_id));
#endif SUPPORT_SLA_AUTO_ROTATE
            }

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", RotoptimizeJob::get_method_description(i).c_str());
        }

        ImGui::EndCombo();
    }

    ImVec2 sz = ImGui::GetItemRectSize();

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", RotoptimizeJob::get_method_description(state.method_id).c_str());

    ImGui::Separator();

    auto btn_txt = _L("Apply");
    auto btn_txt_sz = ImGui::CalcTextSize(btn_txt.c_str());
    ImVec2 button_sz = {btn_txt_sz.x + padding.x, btn_txt_sz.y + padding.y};
    ImGui::SetCursorPosX(padding.x + sz.x - button_sz.x);

    if (!wxGetApp().plater()->get_ui_job_worker().is_idle())
        imgui->disabled_begin(true);

    if ( imgui->button(btn_txt) ) {
        replace_job(wxGetApp().plater()->get_ui_job_worker(),
                    std::make_unique<RotoptimizeJob>());
    }

    imgui->disabled_end();
}

GLGizmoRotate3D::RotoptimzeWindow::~RotoptimzeWindow()
{
    m_imgui->end();
}

} // namespace GUI
} // namespace Slic3r
