#include "CanvasTarget.hpp"

#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "slic3r/GUI/MeshUtils.hpp"

#include <Eigen/Core>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

struct RayHit
{
    double t; // distance along the ray
    Vec3d  p;
    Vec3d  n; // the hit triangle's unit normal
};

// The points where a scene-space ray meets a mesh placed by `world`, nearest first.
std::vector<RayHit> ray_hits(const AABBMesh& mesh, const Transform3d& world, const Vec3d& origin, const Vec3d& dir)
{
    const Transform3d inverse = world.inverse();
    std::vector<RayHit> out;
    for (const AABBMesh::hit_result& hit : mesh.query_ray_hits(inverse * origin, (inverse.linear() * dir).normalized()))
        if (hit.is_hit()) {
            const Vec3d p = world * hit.position();
            out.push_back({(p - origin).dot(dir), p, (inverse.linear().transpose() * hit.normal()).normalized()});
        }
    std::sort(out.begin(), out.end(), [](const RayHit& a, const RayHit& b) { return a.t < b.t; });
    return out;
}

} // namespace

std::optional<Vec2d> project_point(const CanvasView& view, const Vec3d& point)
{
    const Eigen::Vector4d clip = view.projection.matrix() * view.view.matrix() * Eigen::Vector4d(point.x(), point.y(), point.z(), 1.);
    if (clip.w() <= 0.)
        return std::nullopt;
    const Vec3d ndc = clip.head<3>() / clip.w();
    if (std::abs(ndc.x()) > 1. || std::abs(ndc.y()) > 1. || std::abs(ndc.z()) > 1.)
        return std::nullopt;
    const double x = view.viewport[0] + 0.5 * (ndc.x() + 1.) * view.viewport[2];
    const double y = view.viewport[1] + 0.5 * (1. - ndc.y()) * view.viewport[3];
    return Vec2d(x / view.scale, y / view.scale);
}

std::pair<Vec3d, Vec3d> pixel_ray(const CanvasView& view, const Vec2d& pixel)
{
    const double    x       = 2. * (pixel.x() * view.scale - view.viewport[0]) / view.viewport[2] - 1.;
    const double    y       = 1. - 2. * (pixel.y() * view.scale - view.viewport[1]) / view.viewport[3];
    const Eigen::Matrix4d inverse = (view.projection.matrix() * view.view.matrix()).inverse();
    Eigen::Vector4d near_point = inverse * Eigen::Vector4d(x, y, -1., 1.);
    Eigen::Vector4d far_point  = inverse * Eigen::Vector4d(x, y, 1., 1.);
    const Vec3d     origin     = near_point.head<3>() / near_point.w();
    const Vec3d     end        = far_point.head<3>() / far_point.w();
    return {origin, (end - origin).normalized()};
}

std::optional<Vec3d> spot_point(const AABBMesh& mesh, const Transform3d& world, const std::string& spot)
{
    BoundingBoxf3 box;
    for (const Vec3f& v : mesh.vertices())
        box.merge(world * v.cast<double>());
    if (!box.defined)
        return std::nullopt;
    const Vec3d  c      = box.center();
    const double margin = 1. + box.size().maxCoeff();
    Vec3d        origin, dir;
    if (spot == "top")
        origin = {c.x(), c.y(), box.max.z() + margin}, dir = -Vec3d::UnitZ();
    else if (spot == "bottom")
        origin = {c.x(), c.y(), box.min.z() - margin}, dir = Vec3d::UnitZ();
    else if (spot == "front")
        origin = {c.x(), box.min.y() - margin, c.z()}, dir = Vec3d::UnitY();
    else if (spot == "back")
        origin = {c.x(), box.max.y() + margin, c.z()}, dir = -Vec3d::UnitY();
    else if (spot == "left")
        origin = {box.min.x() - margin, c.y(), c.z()}, dir = Vec3d::UnitX();
    else if (spot == "right")
        origin = {box.max.x() + margin, c.y(), c.z()}, dir = -Vec3d::UnitX();
    else
        return std::nullopt;
    const auto hits = ray_hits(mesh, world, origin, dir);
    if (hits.empty())
        return std::nullopt;
    return hits.front().p;
}

std::string check_surface(const Vec3d&                     point,
                          const std::pair<Vec3d, Vec3d>&   ray,
                          const std::vector<SurfaceMesh>&  meshes,
                          const ClippingPlane*             clip,
                          const std::vector<Polygon>&      beds,
                          double                           tolerance)
{
    const auto& [origin, dir] = ray;
    const auto  on_bed = [&](const Vec3d& p) {
        return std::any_of(beds.begin(), beds.end(), [&](const Polygon& bed) { return bed.contains(Point::new_scale(p.x(), p.y())); });
    };
    // On the target's surface: the nearest point of its mesh is within tolerance. Not demanded of
    // the ray's hit, which can slip between two triangles when it passes along their shared edge.
    double off = std::numeric_limits<double>::infinity();
    bool   clipped_target = false;
    for (const SurfaceMesh& m : meshes)
        if (m.target && m.mesh != nullptr) {
            const Transform3d inverse = m.world.inverse();
            int               face    = -1;
            Vec3d             closest;
            m.mesh->squared_distance(inverse * point, face, closest);
            off            = std::min(off, (m.world * closest - point).norm());
            clipped_target = clipped_target || m.clipped;
        }
    if (off > tolerance)
        return "the point is not on the part's surface";
    // A part sunk into the bed is drawn see-through below it, and clicks there do not reach it.
    if (point.z() < -tolerance && on_bed(point))
        return "the point is below the bed";
    // Within the tolerance of the cut, a click can land on either side of it.
    if (clipped_target && clip != nullptr && clip->distance(point) < tolerance)
        return "the point is on the side of the clipping plane that is cut away";

    // What the ray meets, nearest first: each mesh's surfaces (a clipped mesh's only where the plane
    // keeps them) and the bed, which only a ray looking down meets: from below it is drawn see-through
    // and the GUI paints and picks through it.
    struct Hit
    {
        double             t;
        Vec3d              p;
        Vec3d              n;    // the hit triangle's unit normal
        const SurfaceMesh* mesh; // null for the bed
    };
    std::vector<Hit> hits;
    for (const SurfaceMesh& m : meshes) {
        if (m.mesh == nullptr)
            continue;
        for (const RayHit& h : ray_hits(*m.mesh, m.world, origin, dir))
            if (!(m.clipped && clip != nullptr && clip->is_point_clipped(h.p)))
                hits.push_back({h.t, h.p, h.n, &m});
    }
    if (dir.z() < -1e-9) {
        const double t = -origin.z() / dir.z();
        const Vec3d  p = origin + t * dir;
        if (t > 0. && on_bed(p))
            hits.push_back({t, p, Vec3d::UnitZ(), nullptr});
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.t < b.t; });

    // The first thing the ray meets is the part, within the slack of the point, on a triangle whose
    // plane holds the point: the ray through a rounded pixel may meet the point's own surface nearer
    // to the camera or beyond it (a slope), but not another one (a face past an edge, a layer above,
    // the near side of a thin wall).
    const double along = (point - origin).dot(dir);
    const double slack = tolerance + 2. * (point - origin - along * dir).norm();
    for (const Hit& hit : hits) {
        // Not `near`: windows.h defines that as a macro.
        const bool close_by = hit.mesh != nullptr && hit.mesh->target && (hit.p - point).norm() <= slack;
        if (close_by && std::abs(hit.n.dot(point - hit.p)) <= tolerance)
            return {};
        if (hit.t < along - tolerance) {
            if (hit.mesh == nullptr)
                return "the bed is in front of the point";
            if (hit.mesh->target)
                return "a nearer surface of the same part is in front of the point";
            return (hit.mesh->name.empty() ? std::string("another part or object") : hit.mesh->name) + " is in front of the point";
        }
        if (hit.mesh == nullptr)
            return "the click at the point's pixel lands on the bed beside the part";
        if (!hit.mesh->target)
            return "the click at the point's pixel lands on " +
                   (hit.mesh->name.empty() ? std::string("another part or object") : hit.mesh->name) + " beside the part";
        if (close_by)
            return "the click at the point's pixel lands on another face of the part";
        return "the click at the point's pixel lands on the part away from the point";
    }
    return "the click at the point's pixel misses the part";
}

}}} // namespace Slic3r::GUI::Mcp
