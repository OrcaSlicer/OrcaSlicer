#include "libslic3r/libslic3r.h"
#include "libslic3r/AppConfig.hpp"

#include "Camera.hpp"
#include "GUI_App.hpp"
#if ENABLE_CAMERA_STATISTICS
#include "Mouse3DController.hpp"
#include "Plater.hpp"
#endif // ENABLE_CAMERA_STATISTICS

#include <GL/glew.h>

namespace Slic3r {
namespace GUI {

const double Camera::DefaultDistance = 1000.0;
const double Camera::DefaultZoomToBoxMarginFactor = 1.025;
const double Camera::DefaultZoomToVolumesMarginFactor = 1.025;
double Camera::FrustrumMinZRange = 50.0;
double Camera::FrustrumMinNearZ = 100.0;
double Camera::FrustrumZMargin = 10.0;
double Camera::MaxFovDeg = 60.0;
double Camera::ZoomUnit = 0.1;

std::string Camera::get_type_as_string() const
{
    switch (m_type)
    {
    case EType::Unknown:     return "unknown";
    case EType::Perspective: return "perspective";
    default:
    case EType::Ortho:       return "orthographic";
    };
}

void Camera::set_type(EType type)
{
    if (m_type != type && (type == EType::Ortho || type == EType::Perspective)) {
        m_type = type;
        m_prevent_auto_type = true;
        if (m_update_config_on_type_change_enabled) {
            wxGetApp().app_config->set_bool("use_perspective_camera", m_type == EType::Perspective);
        }
    }
}

void Camera::select_next_type()
{
    unsigned char next = (unsigned char)m_type + 1;
    if (next == (unsigned char)EType::Num_types)
        next = 1;

    set_type((EType)next);
}

void Camera::auto_type(EType preferred_type)
{
    if (!wxGetApp().app_config->get_bool("auto_perspective")) return;
    if (preferred_type == EType::Perspective) {
        if (!m_prevent_auto_type) {
            set_type(preferred_type);
            m_prevent_auto_type = false;
        }
    } else {
        set_type(preferred_type);
        m_prevent_auto_type = false;
    }
}

void Camera::translate(const Vec3d& displacement) {
    if (!displacement.isApprox(Vec3d::Zero())) {
        m_view_matrix.translate(-displacement);
        update_target(); 
    }
}

void Camera::set_target(const Vec3d& target)
{
    //BBS do not check validation
    //const Vec3d new_target = validate_target(target);
    update_target();
    const Vec3d new_target = target;
    const Vec3d new_displacement = new_target - m_target;
    if (!new_displacement.isApprox(Vec3d::Zero())) {
        m_target = new_target;
        m_view_matrix.translate(-new_displacement);
    }
}

void Camera::set_zoom(double zoom)
{
    // Don't allow to zoom too far outside the scene.
    const double zoom_min = min_zoom();
    if (zoom_min > 0.0)
        zoom = std::max(zoom, zoom_min);

    // Don't allow to zoom too close to the scene.
    m_zoom = std::min(zoom, max_zoom());
}

void Camera::select_view(const std::string& direction)
{
    if (direction == "iso") {
        set_default_orientation();
        auto_type(EType::Perspective);
    }
    else if (direction == "left") {
        look_at(m_target - m_distance * Vec3d::UnitX(), m_target, Vec3d::UnitZ());
        auto_type(EType::Ortho);
    }
    else if (direction == "right") {
        look_at(m_target + m_distance * Vec3d::UnitX(), m_target, Vec3d::UnitZ());
        auto_type(EType::Ortho);
    }
    else if (direction == "top") {
        look_at(m_target + m_distance * Vec3d::UnitZ(), m_target, Vec3d::UnitY());
        auto_type(EType::Ortho);
    }
    else if (direction == "bottom") {
        look_at(m_target - m_distance * Vec3d::UnitZ(), m_target, -Vec3d::UnitY());
        auto_type(EType::Ortho);
    }
    else if (direction == "front") {
        look_at(m_target - m_distance * Vec3d::UnitY(), m_target, Vec3d::UnitZ());
        auto_type(EType::Ortho);
    }
    else if (direction == "rear") {
        look_at(m_target + m_distance * Vec3d::UnitY(), m_target, Vec3d::UnitZ());
        auto_type(EType::Ortho);
    }
    else if (direction == "topfront") {
        look_at(m_target - 0.707 * m_distance * Vec3d::UnitY() + 0.707 * m_distance * Vec3d::UnitZ(), m_target, Vec3d::UnitY() + Vec3d::UnitZ());
        auto_type(EType::Perspective);
    }
    else if (direction == "plate") {
        look_at(m_target - 0.707 * m_distance * Vec3d::UnitY() + 0.707 * m_distance * Vec3d::UnitZ(), m_target, Vec3d::UnitY() + Vec3d::UnitZ());
        auto_type(EType::Perspective);
    }
}
void Camera::select_view(ViewAngleType type)
{
    switch (type) {
    case Slic3r::GUI::Camera::ViewAngleType::Iso: {
        select_view("iso");
        break;
    }
    case Slic3r::GUI::Camera::ViewAngleType::Top_Front: {
        select_view("topfront");
        break;
    }
    case Slic3r::GUI::Camera::ViewAngleType::Left: {
        select_view("left");
        break;
    }
    case Slic3r::GUI::Camera::ViewAngleType::Right: {
        select_view("right");
        break;
    }
    case Slic3r::GUI::Camera::ViewAngleType::Top_Plate:
    case Slic3r::GUI::Camera::ViewAngleType::Top: {
        select_view("top");
        break;
    }
    case Slic3r::GUI::Camera::ViewAngleType::Bottom: {
        select_view("bottom");
        break;
    }
    case Slic3r::GUI::Camera::ViewAngleType::Front: {
        select_view("front");
        break;
    }
    case Slic3r::GUI::Camera::ViewAngleType::Rear: {
        select_view("rear");
        break;
    }
    default: break;
    }
}

double Camera::get_near_left() const
{
    switch (m_type)
    {
    case EType::Perspective:
        return m_frustrum_zs.first * (m_projection_matrix.matrix()(0, 2) - 1.0) / m_projection_matrix.matrix()(0, 0);
    default:
    case EType::Ortho:
        return -1.0 / m_projection_matrix.matrix()(0, 0) - 0.5 * m_projection_matrix.matrix()(0, 0) * m_projection_matrix.matrix()(0, 3);
    }
}

double Camera::get_near_right() const
{
    switch (m_type)
    {
    case EType::Perspective:
        return m_frustrum_zs.first * (m_projection_matrix.matrix()(0, 2) + 1.0) / m_projection_matrix.matrix()(0, 0);
    default:
    case EType::Ortho:
        return 1.0 / m_projection_matrix.matrix()(0, 0) - 0.5 * m_projection_matrix.matrix()(0, 0) * m_projection_matrix.matrix()(0, 3);
    }
}

double Camera::get_near_top() const
{
    switch (m_type)
    {
    case EType::Perspective:
        return m_frustrum_zs.first * (m_projection_matrix.matrix()(1, 2) + 1.0) / m_projection_matrix.matrix()(1, 1);
    default:
    case EType::Ortho:
        return 1.0 / m_projection_matrix.matrix()(1, 1) - 0.5 * m_projection_matrix.matrix()(1, 1) * m_projection_matrix.matrix()(1, 3);
    }
}

double Camera::get_near_bottom() const
{
    switch (m_type)
    {
    case EType::Perspective:
        return m_frustrum_zs.first * (m_projection_matrix.matrix()(1, 2) - 1.0) / m_projection_matrix.matrix()(1, 1);
    default:
    case EType::Ortho:
        return -1.0 / m_projection_matrix.matrix()(1, 1) - 0.5 * m_projection_matrix.matrix()(1, 1) * m_projection_matrix.matrix()(1, 3);
    }
}

double Camera::get_near_width() const
{
    switch (m_type)
    {
    case EType::Perspective:
        return 2.0 * m_frustrum_zs.first / m_projection_matrix.matrix()(0, 0);
    default:
    case EType::Ortho:
        return 2.0 / m_projection_matrix.matrix()(0, 0);
    }
}

double Camera::get_near_height() const
{
    switch (m_type)
    {
    case EType::Perspective:
        return 2.0 * m_frustrum_zs.first / m_projection_matrix.matrix()(1, 1);
    default:
    case EType::Ortho:
        return 2.0 / m_projection_matrix.matrix()(1, 1);
    }
}

double Camera::get_fov() const
{
    switch (m_type)
    {
    case EType::Perspective:
        return 2.0 * Geometry::rad2deg(std::atan(1.0 / m_projection_matrix.matrix()(1, 1)));
    default:
    case EType::Ortho:
        return 0.0;
    };
}

void Camera::set_viewport(int x, int y, unsigned int w, unsigned int h)
{
    m_viewport = { 0, 0, int(w), int(h) };
}

void Camera::apply_viewport() const
{
    glsafe(::glViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]));
}

void Camera::apply_projection(const BoundingBoxf3& box, double near_z, double far_z)
{
    double w = 0.0;
    double h = 0.0;

    m_frustrum_zs = calc_tight_frustrum_zs_around(box);

    if (near_z > 0.0)
        m_frustrum_zs.first = std::max(std::min(m_frustrum_zs.first, near_z), FrustrumMinNearZ);

    if (far_z > 0.0)
        m_frustrum_zs.second = std::max(m_frustrum_zs.second, far_z);

    w = 0.5 * (double)m_viewport[2];
    h = 0.5 * (double)m_viewport[3];

    const double inv_zoom = get_inv_zoom();
    w *= inv_zoom;
    h *= inv_zoom;

    switch (m_type)
    {
    default:
    case EType::Ortho:
    {
        m_gui_scale = 1.0;
        break;
    }
    case EType::Perspective:
    {
        // scale near plane to keep w and h constant on the plane at z = m_distance
        const double scale = m_frustrum_zs.first / m_distance;
        w *= scale;
        h *= scale;
        m_gui_scale = scale;
        break;
    }
    }

    apply_projection(-w, w, -h, h, m_frustrum_zs.first, m_frustrum_zs.second);
}

void Camera::apply_projection(double left, double right, double bottom, double top, double near_z, double far_z)
{
    assert(left != right && bottom != top && near_z != far_z);
    const double inv_dx = 1.0 / (right - left);
    const double inv_dy = 1.0 / (top - bottom);
    const double inv_dz = 1.0 / (far_z - near_z);

    switch (m_type)
    {
    default:
    case EType::Ortho:
    {
        m_projection_matrix.matrix() << 2.0 * inv_dx,          0.0,           0.0,   -(left + right) * inv_dx,
                                                 0.0, 2.0 * inv_dy,           0.0,   -(bottom + top) * inv_dy,
                                                 0.0,          0.0, -2.0 * inv_dz, -(near_z + far_z) * inv_dz,
                                                 0.0,          0.0,           0.0,                        1.0;
        break;
    }
    case EType::Perspective:
    {
        m_projection_matrix.matrix() << 2.0 * near_z * inv_dx,                   0.0,    (left + right) * inv_dx,                            0.0,
                                                          0.0, 2.0 * near_z * inv_dy,    (bottom + top) * inv_dy,                            0.0,
                                                          0.0,                   0.0, -(near_z + far_z) * inv_dz, -2.0 * near_z * far_z * inv_dz,
                                                          0.0,                   0.0,                       -1.0,                            0.0;
        break;
    }
    }
}

void Camera::zoom_to_box(const BoundingBoxf3& box, double margin_factor)
{
    // Calculate the zoom factor needed to adjust the view around the given box.
    const double zoom = calc_zoom_to_bounding_box_factor(box, margin_factor);
    if (zoom > 0.0) {
        m_zoom = zoom;
        // center view around box center
        set_target(box.center());
    }
}

void Camera::zoom_to_volumes(const GLVolumePtrs& volumes, double margin_factor)
{
    Vec3d center;
    const double zoom = calc_zoom_to_volumes_factor(volumes, center, margin_factor);
    if (zoom > 0.0) {
        m_zoom = zoom;
        // center view around the calculated center
        set_target(center);
    }
}

#if ENABLE_CAMERA_STATISTICS
void Camera::debug_render() const
{
    ImGuiWrapper& imgui = *wxGetApp().imgui();
    imgui.begin(std::string("Camera statistics"), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    std::string type = get_type_as_string();
    if (wxGetApp().plater()->get_mouse3d_controller().connected()
        || (wxGetApp().app_config->get_bool("use_free_camera"))
        )
        type += "/free";
    else
        type += "/constrained";

    Vec3f position = get_position().cast<float>();
    Vec3f target = m_target.cast<float>();
    float distance = (float)get_distance();
    float zenit = (float)m_zenit;
    Vec3f forward = get_dir_forward().cast<float>();
    Vec3f right = get_dir_right().cast<float>();
    Vec3f up = get_dir_up().cast<float>();
    float nearZ = (float)m_frustrum_zs.first;
    float farZ = (float)m_frustrum_zs.second;
    float deltaZ = farZ - nearZ;
    float zoom = (float)m_zoom;
    float fov = (float)get_fov();
    std::array<int, 4>viewport = get_viewport();
    float gui_scale = (float)get_gui_scale();

    ImGui::InputText("Type", type.data(), type.length(), ImGuiInputTextFlags_ReadOnly);
    ImGui::Separator();
    ImGui::InputFloat3("Position", position.data(), "%.6f", ImGuiInputTextFlags_ReadOnly);
    ImGui::InputFloat3("Target", target.data(), "%.6f", ImGuiInputTextFlags_ReadOnly);
    ImGui::InputFloat("Distance", &distance, 0.0f, 0.0f, "%.6f", ImGuiInputTextFlags_ReadOnly);
    ImGui::Separator();
    ImGui::InputFloat("Zenit", &zenit, 0.0f, 0.0f, "%.6f", ImGuiInputTextFlags_ReadOnly);
    ImGui::Separator();
    ImGui::InputFloat3("Forward", forward.data(), "%.6f", ImGuiInputTextFlags_ReadOnly);
    ImGui::InputFloat3("Right", right.data(), "%.6f", ImGuiInputTextFlags_ReadOnly);
    ImGui::InputFloat3("Up", up.data(), "%.6f", ImGuiInputTextFlags_ReadOnly);
    ImGui::Separator();
    ImGui::InputFloat("Near Z", &nearZ, 0.0f, 0.0f, "%.6f", ImGuiInputTextFlags_ReadOnly);
    ImGui::InputFloat("Far Z", &farZ, 0.0f, 0.0f, "%.6f", ImGuiInputTextFlags_ReadOnly);
    ImGui::InputFloat("Delta Z", &deltaZ, 0.0f, 0.0f, "%.6f", ImGuiInputTextFlags_ReadOnly);
    ImGui::Separator();
    ImGui::InputFloat("Zoom", &zoom, 0.0f, 0.0f, "%.6f", ImGuiInputTextFlags_ReadOnly);
    ImGui::InputFloat("Fov", &fov, 0.0f, 0.0f, "%.6f", ImGuiInputTextFlags_ReadOnly);
    ImGui::Separator();
    ImGui::InputInt4("Viewport", viewport.data(), ImGuiInputTextFlags_ReadOnly);
    ImGui::Separator();
    ImGui::InputFloat("GUI scale", &gui_scale, 0.0f, 0.0f, "%.6f", ImGuiInputTextFlags_ReadOnly);
    imgui.end();
}
#endif // ENABLE_CAMERA_STATISTICS

void Camera::rotate_on_sphere_with_target(double delta_azimut_rad, double delta_zenit_rad, bool apply_limits, Vec3d target)
{
    m_zenit += Geometry::rad2deg(delta_zenit_rad);
    if (apply_limits) {
        if (m_zenit > 90.0f) {
            delta_zenit_rad -= Geometry::deg2rad(m_zenit - 90.0f);
            m_zenit = 90.0f;
        }
        else if (m_zenit < -90.0f) {
            delta_zenit_rad -= Geometry::deg2rad(m_zenit + 90.0f);
            m_zenit = -90.0f;
        }
    }

    Vec3d translation = m_view_matrix.translation() + m_view_rotation * target;
    auto rot_z = Eigen::AngleAxisd(delta_azimut_rad, Vec3d::UnitZ());
    m_view_rotation *= rot_z * Eigen::AngleAxisd(delta_zenit_rad, rot_z.inverse() * get_dir_right());
    m_view_rotation.normalize();
    m_view_matrix.fromPositionOrientationScale(m_view_rotation * (-target) + translation, m_view_rotation, Vec3d(1., 1., 1.));
}


void Camera::rotate_on_sphere(double delta_azimut_rad, double delta_zenit_rad, bool apply_limits)
{
    m_zenit += Geometry::rad2deg(delta_zenit_rad);
    if (apply_limits) {
        if (m_zenit > 90.0f) {
            delta_zenit_rad -= Geometry::deg2rad(m_zenit - 90.0f);
            m_zenit = 90.0f;
        }
        else if (m_zenit < -90.0f) {
            delta_zenit_rad -= Geometry::deg2rad(m_zenit + 90.0f);
            m_zenit = -90.0f;
        }
    }

    const Vec3d translation = m_view_matrix.translation() + m_view_rotation * m_target;
    const auto rot_z = Eigen::AngleAxisd(delta_azimut_rad, Vec3d::UnitZ());
    m_view_rotation *= rot_z * Eigen::AngleAxisd(delta_zenit_rad, rot_z.inverse() * get_dir_right());
    m_view_rotation.normalize();
    m_view_matrix.fromPositionOrientationScale(m_view_rotation * (- m_target) + translation, m_view_rotation, Vec3d(1., 1., 1.));
}

//BBS rotate with target
void Camera::rotate_local_with_target(const Vec3d& rotation_rad, Vec3d target)
{
    double angle = rotation_rad.norm();
    if (std::abs(angle) > EPSILON) {
	    Vec3d translation = m_view_matrix.translation() + m_view_rotation * target;
	    Vec3d axis        = m_view_rotation.conjugate() * rotation_rad.normalized();
        m_view_rotation *= Eigen::Quaterniond(Eigen::AngleAxisd(angle, axis));
        m_view_rotation.normalize();
	    m_view_matrix.fromPositionOrientationScale(m_view_rotation * (-target) + translation, m_view_rotation, Vec3d(1., 1., 1.));
	    update_zenit();
	}
}

// Virtual trackball, rotate around an axis, where the eucledian norm of the axis gives the rotation angle in radians.
void Camera::rotate_local_around_target(const Vec3d& rotation_rad)
{
    const double angle = rotation_rad.norm();
    if (std::abs(angle) > EPSILON) {
        const Vec3d translation = m_view_matrix.translation() + m_view_rotation * m_target;
        const Vec3d axis = m_view_rotation.conjugate() * rotation_rad.normalized();
        m_view_rotation *= Eigen::Quaterniond(Eigen::AngleAxisd(angle, axis));
        m_view_rotation.normalize();
	    m_view_matrix.fromPositionOrientationScale(m_view_rotation * (-m_target) + translation, m_view_rotation, Vec3d(1., 1., 1.));
	    update_zenit();
	}
}

void Camera::set_rotation(const Transform3d& rotation)
{
    const Vec3d translation = m_view_matrix.translation() + m_view_rotation * m_target;
    m_view_rotation = Eigen::Quaterniond(rotation.matrix().template block<3, 3>(0, 0));
    m_view_rotation.normalize();
    m_view_matrix.fromPositionOrientationScale(m_view_rotation * (-m_target) + translation, m_view_rotation, Vec3d(1., 1., 1.));
    update_zenit();
}

std::pair<double, double> Camera::calc_tight_frustrum_zs_around(const BoundingBoxf3& box)
{
    std::pair<double, double> ret;
    auto& [near_z, far_z] = ret;

    // box in eye space
    const BoundingBoxf3 eye_box = box.transformed(m_view_matrix);
    near_z = -eye_box.max.z();
    far_z  = -eye_box.min.z();

    // apply margin
    near_z -= FrustrumZMargin;
    far_z += FrustrumZMargin;

    // ensure min size
    if (far_z - near_z < FrustrumMinZRange) {
        const double mid_z = 0.5 * (near_z + far_z);
        const double half_size = 0.5 * FrustrumMinZRange;
        near_z = mid_z - half_size;
        far_z = mid_z + half_size;
    }

    if (near_z < FrustrumMinNearZ) {
        const double delta = FrustrumMinNearZ - near_z;
        set_distance(m_distance + delta);
        near_z += delta;
        far_z += delta;
    }
// The following is commented out because it causes flickering of the 3D scene GUI
// when the bounding box of the scene gets large enough
// We need to introduce some smarter code to move the camera back and forth in such case
//    else if (near_z > 2.0 * FrustrumMinNearZ && m_distance > DefaultDistance) {
//        float delta = m_distance - DefaultDistance;
//        set_distance(DefaultDistance);
//        near_z -= delta;
//        far_z -= delta;
//    }

    return ret;
}

double Camera::calc_zoom_to_bounding_box_factor(const BoundingBoxf3& box, double margin_factor) const
{
    const double max_bb_size = box.max_size();
    if (max_bb_size == 0.0)
        return -1.0;

    // project the box vertices on a plane perpendicular to the camera forward axis
    // then calculates the vertices coordinate on this plane along the camera xy axes

    const Vec3d right = get_dir_right();
    const Vec3d up = get_dir_up();
    const Vec3d forward = get_dir_forward();
    const Vec3d bb_center = box.center();

    // box vertices in world space
    const std::vector<Vec3d> vertices = {
        box.min,
        { box.max(0), box.min(1), box.min(2) },
        { box.max(0), box.max(1), box.min(2) },
        { box.min(0), box.max(1), box.min(2) },
        { box.min(0), box.min(1), box.max(2) },
        { box.max(0), box.min(1), box.max(2) },
        box.max,
        { box.min(0), box.max(1), box.max(2) }
    };

    double min_x = DBL_MAX;
    double min_y = DBL_MAX;
    double max_x = -DBL_MAX;
    double max_y = -DBL_MAX;

    for (const Vec3d& v : vertices) {
        // project vertex on the plane perpendicular to camera forward axis
        const Vec3d pos = v - bb_center;
        const Vec3d proj_on_plane = pos - pos.dot(forward) * forward;

        // calculates vertex coordinate along camera xy axes
        const double x_on_plane = proj_on_plane.dot(right);
        const double y_on_plane = proj_on_plane.dot(up);

        min_x = std::min(min_x, x_on_plane);
        min_y = std::min(min_y, y_on_plane);
        max_x = std::max(max_x, x_on_plane);
        max_y = std::max(max_y, y_on_plane);
    }

    double dx = max_x - min_x;
    double dy = max_y - min_y;
    if (dx <= 0.0 || dy <= 0.0)
        return -1.0f;

    dx *= margin_factor;
    dy *= margin_factor;

    return std::min((double)m_viewport[2] / dx, (double)m_viewport[3] / dy);
}

double Camera::calc_zoom_to_volumes_factor(const GLVolumePtrs& volumes, Vec3d& center, double margin_factor) const
{
    if (volumes.empty())
        return -1.0;

    // project the volumes vertices on a plane perpendicular to the camera forward axis
    // then calculates the vertices coordinate on this plane along the camera xy axes

    const Vec3d right = get_dir_right();
    const Vec3d up = get_dir_up();
    const Vec3d forward = get_dir_forward();

    BoundingBoxf3 box;
    for (const GLVolume* volume : volumes) {
        box.merge(volume->transformed_bounding_box());
    }
    center = box.center();

    double min_x = DBL_MAX;
    double min_y = DBL_MAX;
    double max_x = -DBL_MAX;
    double max_y = -DBL_MAX;

    for (const GLVolume* volume : volumes) {
        const Transform3d& transform = volume->world_matrix();
        const TriangleMesh* hull = volume->convex_hull();
        if (hull == nullptr)
            continue;

        for (const Vec3f& vertex : hull->its.vertices) {
            const Vec3d v = transform * vertex.cast<double>();

            // project vertex on the plane perpendicular to camera forward axis
            const Vec3d pos = v - center;
            const Vec3d proj_on_plane = pos - pos.dot(forward) * forward;

            // calculates vertex coordinate along camera xy axes
            const double x_on_plane = proj_on_plane.dot(right);
            const double y_on_plane = proj_on_plane.dot(up);

            min_x = std::min(min_x, x_on_plane);
            min_y = std::min(min_y, y_on_plane);
            max_x = std::max(max_x, x_on_plane);
            max_y = std::max(max_y, y_on_plane);
        }
    }

    center += 0.5 * (max_x + min_x) * right + 0.5 * (max_y + min_y) * up;

    const double dx = margin_factor * (max_x - min_x);
    const double dy = margin_factor * (max_y - min_y);

    if (dx <= 0.0 || dy <= 0.0)
        return -1.0f;

    return std::min((double)m_viewport[2] / dx, (double)m_viewport[3] / dy);
}

void Camera::set_distance(double distance)
{
    if(distance < EPSILON || distance > 1.0e6)
        return;
        
    if (m_distance != distance) {
        m_view_matrix.translate((distance - m_distance) * get_dir_forward());
        m_distance = distance;
        
        update_target();
    }
}

void Camera::load_camera_view(Camera& cam)
{
    m_target = cam.get_target();
    m_zoom = cam.get_zoom();
    m_scene_box = cam.get_scene_box();
    m_viewport = cam.get_viewport();
    m_view_matrix = cam.get_view_matrix();
    m_projection_matrix = cam.get_projection_matrix();
    m_view_rotation = cam.get_view_rotation();
    m_frustrum_zs = cam.get_z_range();
    m_zenit = cam.get_zenit();
}

void Camera::look_at(const Vec3d& position, const Vec3d& target, const Vec3d& up)
{
    const Vec3d unit_z = (position - target).normalized();
    const Vec3d unit_x = up.cross(unit_z).normalized();
    const Vec3d unit_y = unit_z.cross(unit_x).normalized();

    m_target = target;
    m_distance = (position - target).norm();
    const Vec3d new_position = m_target + m_distance * unit_z;

    m_view_matrix(0, 0) = unit_x.x();
    m_view_matrix(0, 1) = unit_x.y();
    m_view_matrix(0, 2) = unit_x.z();
    m_view_matrix(0, 3) = -unit_x.dot(new_position);

    m_view_matrix(1, 0) = unit_y.x();
    m_view_matrix(1, 1) = unit_y.y();
    m_view_matrix(1, 2) = unit_y.z();
    m_view_matrix(1, 3) = -unit_y.dot(new_position);

    m_view_matrix(2, 0) = unit_z.x();
    m_view_matrix(2, 1) = unit_z.y();
    m_view_matrix(2, 2) = unit_z.z();
    m_view_matrix(2, 3) = -unit_z.dot(new_position);

    m_view_matrix(3, 0) = 0.0;
    m_view_matrix(3, 1) = 0.0;
    m_view_matrix(3, 2) = 0.0;
    m_view_matrix(3, 3) = 1.0;

    // Initialize the rotation quaternion from the rotation submatrix of of m_view_matrix.
    m_view_rotation = Eigen::Quaterniond(m_view_matrix.matrix().template block<3, 3>(0, 0));
    m_view_rotation.normalize();

    update_zenit();
}

void Camera::set_default_orientation()
{
    m_zenit = 45.0f;
    const double theta_rad = Geometry::deg2rad(-(double)m_zenit);
    const double phi_rad = Geometry::deg2rad(45.0);
    const double sin_theta = ::sin(theta_rad);
    const Vec3d camera_pos = m_target + m_distance * Vec3d(sin_theta * ::sin(phi_rad), sin_theta * ::cos(phi_rad), ::cos(theta_rad));
    m_view_rotation = Eigen::AngleAxisd(theta_rad, Vec3d::UnitX()) * Eigen::AngleAxisd(phi_rad, Vec3d::UnitZ());
    m_view_rotation.normalize();
    m_view_matrix.fromPositionOrientationScale(m_view_rotation * (-camera_pos), m_view_rotation, Vec3d::Ones());
}

Vec3d Camera::validate_target(const Vec3d& target) const
{
    BoundingBoxf3 test_box = m_scene_box;
    test_box.translate(-m_scene_box.center());
    // We may let this factor be customizable
    //BBS enlarge scene box factor
    static const double ScaleFactor = 3.0;
    test_box.scale(ScaleFactor);
    test_box.translate(m_scene_box.center());

    return { std::clamp(target(0), test_box.min(0), test_box.max(0)),
             std::clamp(target(1), test_box.min(1), test_box.max(1)),
             std::clamp(target(2), test_box.min(2), test_box.max(2)) };
}

void Camera::update_zenit()
{
    m_zenit = Geometry::rad2deg(0.5 * M_PI - std::acos(std::clamp(-get_dir_forward().dot(Vec3d::UnitZ()), -1.0, 1.0)));
}

void Camera::update_target() {
    Vec3d temptarget = get_position() + m_distance * get_dir_forward();
    if (!(temptarget-m_target).isApprox(Vec3d::Zero())){
        m_target = temptarget;
    } 
}
} // GUI
} // Slic3r

