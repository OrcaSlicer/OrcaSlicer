#include "CameraUtils.hpp"
#include <igl/project.h> // projecting points
#include <igl/unproject.h>

#include "slic3r/GUI/3DScene.hpp" // GLVolume
#include "libslic3r/Geometry/ConvexHull.hpp"

using namespace Slic3r;
using namespace GUI;

Points CameraUtils::project(const Camera &            camera,
                            const std::vector<Vec3d> &points)
{
    Vec4i32 viewport(camera.get_viewport().data());

    // Convert our std::vector to Eigen dynamic matrix.
    Eigen::Matrix<double, Eigen::Dynamic, 3, Eigen::DontAlign>
        pts(points.size(), 3);
    for (size_t i = 0; i < points.size(); ++i)
        pts.block<1, 3>(i, 0) = points[i];

    // Get the projections.
    Eigen::Matrix<double, Eigen::Dynamic, 3, Eigen::DontAlign> projections;
    igl::project(pts, camera.get_view_matrix().matrix(),
                    camera.get_projection_matrix().matrix(), viewport, projections);

    Points result;
    result.reserve(points.size());
    int window_height = viewport[3];

    // convert to points --> loss precision
    for (int i = 0; i < projections.rows(); ++i) {
        double x = projections(i, 0);
        double y = projections(i, 1);
        // opposit direction o Y
        result.emplace_back(x, window_height - y);
    }
    return result;
}

Slic3r::Point CameraUtils::project(const Camera &camera, const Vec3d &point)
{
    // IMPROVE: do it faster when you need it (inspire in project multi point)
    return project(camera, std::vector{point}).front();
}

Slic3r::Polygon CameraUtils::create_hull2d(const Camera &  camera,
                                   const GLVolume &volume)
{
    std::vector<Vec3d>  vertices;
    const TriangleMesh *hull = volume.convex_hull();
    if (hull != nullptr) {
        const indexed_triangle_set &its = hull->its;        
        vertices.reserve(its.vertices.size());
        // cast vector
        for (const Vec3f &vertex : its.vertices)
            vertices.emplace_back(vertex.cast<double>());
    } else {
        // Negative volume doesn't have convex hull so use bounding box
        auto bb = volume.bounding_box();
        Vec3d &min = bb.min;
        Vec3d &max = bb.max;
        vertices   = {min,
                    Vec3d(min.x(), min.y(), max.z()),
                    Vec3d(min.x(), max.y(), min.z()),
                    Vec3d(min.x(), max.y(), max.z()),
                    Vec3d(max.x(), min.y(), min.z()),
                    Vec3d(max.x(), min.y(), max.z()),
                    Vec3d(max.x(), max.y(), min.z()),
                    max};
    }

    const Transform3d &trafoMat =
        volume.get_instance_transformation().get_matrix() *
        volume.get_volume_transformation().get_matrix();
    for (Vec3d &vertex : vertices)
        vertex = trafoMat * vertex.cast<double>();

    Points vertices_2d = project(camera, vertices);
    return Geometry::convex_hull(vertices_2d);
}

void CameraUtils::ray_from_screen_pos(const Camera &camera, const Vec2d &position, Vec3d &point, Vec3d &direction) {
    switch (camera.get_type()) {
    case Camera::EType::Ortho:       return ray_from_ortho_screen_pos(camera, position, point, direction);
    case Camera::EType::Perspective: return ray_from_persp_screen_pos(camera, position, point, direction);
    default: break;
    }
}

Vec3d CameraUtils::screen_point(const Camera &camera, const Vec2d &position)
{ 
    double height = camera.get_viewport().data()[3];
    // Y coordinate has opposit direction
    return Vec3d(position.x(), height - position.y(), 0.);
}

void CameraUtils::ray_from_ortho_screen_pos(const Camera &camera, const Vec2d &position, Vec3d &point, Vec3d &direction)
{
    assert(camera.get_type() == Camera::EType::Ortho);
    Matrix4d modelview  = camera.get_view_matrix().matrix();
    Matrix4d projection = camera.get_projection_matrix().matrix();
    Vec4i32    viewport(camera.get_viewport().data());
    igl::unproject(screen_point(camera,position), modelview, projection, viewport, point);
    direction = camera.get_dir_forward();
}
void CameraUtils::ray_from_persp_screen_pos(const Camera &camera, const Vec2d &position, Vec3d &point, Vec3d &direction)
{
    assert(camera.get_type() == Camera::EType::Perspective);
    Matrix4d modelview  = camera.get_view_matrix().matrix();
    Matrix4d projection = camera.get_projection_matrix().matrix();
    Vec4i32    viewport(camera.get_viewport().data());
    igl::unproject(screen_point(camera, position), modelview, projection, viewport, point);
    direction = point - camera.get_position();
}

Vec2d CameraUtils::get_z0_position(const Camera &camera, const Vec2d & coor)
{
    Vec3d p0, dir;
    ray_from_screen_pos(camera, coor, p0, dir);

    // is approx zero
    if ((fabs(dir.z()) - 1e-4) < 0)
        return Vec2d(std::numeric_limits<double>::max(), 
                     std::numeric_limits<double>::max());

    // find position of ray cross plane(z = 0)
    double t = p0.z() / dir.z();
    Vec3d p = p0 - t * dir;
    return Vec2d(p.x(), p.y());
}
