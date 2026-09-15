#include "LayOnFace.hpp"

#include "Geometry.hpp"
#include "Geometry/ConvexHull.hpp"
#include "Model.hpp"
#include "TriangleMesh.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace Slic3r {

std::vector<LayOnFacePlane> lay_on_face_planes(const ModelObject &object, const Transform3d &inst_matrix)
{
    // An object can only rest on its convex hull, so candidate faces are taken from the hull of all model parts.
    TriangleMesh ch;
    for (const ModelVolume* vol : object.volumes) {
        if (vol->type() != ModelVolumeType::MODEL_PART)
            continue;
        TriangleMesh vol_ch = vol->get_convex_hull();
        vol_ch.transform(vol->get_matrix());
        ch.merge(vol_ch);
    }
    ch = ch.convex_hull_3d();
    std::vector<LayOnFacePlane> planes;

    // Following constants are used for discarding too small polygons.
    const float minimal_area = 5.f; // in square mm (world coordinates)
    const float minimal_side = 1.f; // mm
    const float minimal_angle = 1.f; // degree, initial value was 10, but cause bugs

    // Now we'll go through all the facets and append Points of facets sharing the same normal.
    // This part is still performed in mesh coordinate system.
    const int                num_of_facets  = ch.facets_count();
    const std::vector<Vec3f> face_normals   = its_face_normals(ch.its);
    const std::vector<Vec3i32> face_neighbors = its_face_neighbors(ch.its);
    std::vector<int>         facet_queue(num_of_facets, 0);
    std::vector<bool>        facet_visited(num_of_facets, false);
    int                      facet_queue_cnt = 0;
    const stl_normal*        normal_ptr      = nullptr;
    int                      facet_idx       = 0;
    while (1) {
        // Find next unvisited triangle:
        for (; facet_idx < num_of_facets; ++ facet_idx)
            if (!facet_visited[facet_idx]) {
                facet_queue[facet_queue_cnt ++] = facet_idx;
                facet_visited[facet_idx] = true;
                normal_ptr = &face_normals[facet_idx];
                planes.emplace_back();
                break;
            }
        if (facet_idx == num_of_facets)
            break; // Everything was visited already

        while (facet_queue_cnt > 0) {
            int facet_idx = facet_queue[-- facet_queue_cnt];
            const stl_normal& this_normal = face_normals[facet_idx];
            if (std::abs(this_normal(0) - (*normal_ptr)(0)) < 0.001 && std::abs(this_normal(1) - (*normal_ptr)(1)) < 0.001 && std::abs(this_normal(2) - (*normal_ptr)(2)) < 0.001) {
                const Vec3i32 face = ch.its.indices[facet_idx];
                for (int j=0; j<3; ++j)
                    planes.back().outline.emplace_back(ch.its.vertices[face[j]].cast<double>());

                facet_visited[facet_idx] = true;
                for (int j = 0; j < 3; ++ j)
                    if (int neighbor_idx = face_neighbors[facet_idx][j]; neighbor_idx >= 0 && ! facet_visited[neighbor_idx])
                        facet_queue[facet_queue_cnt ++] = neighbor_idx;
            }
        }
        planes.back().normal = normal_ptr->cast<double>();

        Pointf3s& verts = planes.back().outline;
        // Now we'll transform all the points into world coordinates, so that the areas, angles and distances
        // make real sense.
        verts = transform(verts, inst_matrix);

        // if this is a just a very small triangle, remove it to speed up further calculations (it would be rejected later anyway):
        if (verts.size() == 3 &&
            ((verts[0] - verts[1]).norm() < minimal_side
            || (verts[0] - verts[2]).norm() < minimal_side
            || (verts[1] - verts[2]).norm() < minimal_side))
            planes.pop_back();
    }

    // Let's prepare transformation of the normal vector from mesh to instance coordinates.
    const Matrix3d normal_matrix = inst_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();

    // Now we'll go through all the polygons, transform the points into xy plane to process them:
    for (unsigned int polygon_id=0; polygon_id < planes.size(); ++polygon_id) {
        Pointf3s& polygon = planes[polygon_id].outline;
        const Vec3d& normal = planes[polygon_id].normal;

        // transform the normal according to the instance matrix:
        const Vec3d normal_transformed = normal_matrix * normal;

        // We are going to rotate about z and y to flatten the plane
        Eigen::Quaterniond q;
        Transform3d& m = planes[polygon_id].to_plane_frame;
        m = Transform3d::Identity();
        m.matrix().block(0, 0, 3, 3) = q.setFromTwoVectors(normal_transformed, Vec3d::UnitZ()).toRotationMatrix();
        polygon = transform(polygon, m);

        // Now to remove the inner points. We'll misuse Geometry::convex_hull for that, but since
        // it works in fixed point representation, we will rescale the polygon to avoid overflows.
        // And yes, it is a nasty thing to do. Whoever has time is free to refactor.
        Vec3d bb_size = BoundingBoxf3(polygon).size();
        float sf = std::min(1./bb_size(0), 1./bb_size(1));
        Transform3d tr = Geometry::scale_transform({ sf, sf, 1.f });
        polygon = transform(polygon, tr);
        polygon = Slic3r::Geometry::convex_hull(polygon);
        polygon = transform(polygon, tr.inverse());

        // Calculate area of the polygons and discard ones that are too small
        float& area = planes[polygon_id].area;
        area = 0.f;
        for (unsigned int i = 0; i < polygon.size(); i++) // Shoelace formula
            area += polygon[i](0)*polygon[i + 1 < polygon.size() ? i + 1 : 0](1) - polygon[i + 1 < polygon.size() ? i + 1 : 0](0)*polygon[i](1);
        area = 0.5f * std::abs(area);

        bool discard = false;
        if (area < minimal_area)
            discard = true;
        else {
            // We also check the inner angles and discard polygons with angles smaller than the following threshold
            const double angle_threshold = ::cos(minimal_angle * (double)PI / 180.0);

            for (unsigned int i = 0; i < polygon.size(); ++i) {
                const Vec3d& prec = polygon[(i == 0) ? polygon.size() - 1 : i - 1];
                const Vec3d& curr = polygon[i];
                const Vec3d& next = polygon[(i == polygon.size() - 1) ? 0 : i + 1];

                if ((prec - curr).normalized().dot((next - curr).normalized()) > angle_threshold) {
                    discard = true;
                    break;
                }
            }
        }

        if (discard) {
            planes[polygon_id--] = std::move(planes.back());
            planes.pop_back();
            continue;
        }

        const Vec3d centroid = std::accumulate(polygon.begin(), polygon.end(), Vec3d(0.0, 0.0, 0.0)) / double(polygon.size());
        planes[polygon_id].center = inst_matrix.inverse() * (m.inverse() * centroid);
    }

    std::sort(planes.rbegin(), planes.rend(), [](const LayOnFacePlane& a, const LayOnFacePlane& b) { return a.area < b.area; });
    return planes;
}

int find_largest_plane(const std::vector<LayOnFacePlane> &planes)
{
    // The plane frame maps the instance normal to +Z, so the normal's z in instance coordinates is element (2, 2).
    auto downward = [](const LayOnFacePlane &plane) { return -plane.to_plane_frame.linear()(2, 2); };
    // Areas are floats from rounded geometry, so faces within 0.1% count as equal.
    int best = -1;
    for (size_t i = 0; i < planes.size() && planes[i].area >= planes.front().area * (1. - 1e-3); ++i)
        if (best < 0 || downward(planes[i]) > downward(planes[best]))
            best = int(i);
    return best;
}

int find_plane_by_normal(const std::vector<LayOnFacePlane> &planes, const Vec3d &direction)
{
    const Vec3d dir  = direction.normalized();
    int         best = -1;
    double      best_dot = -2.;
    for (size_t i = 0; i < planes.size(); ++i)
        if (const double dot = planes[i].normal.dot(dir); dot > best_dot) {
            best_dot = dot;
            best     = int(i);
        }
    return best;
}

int find_plane_at_point(const std::vector<LayOnFacePlane> &planes, const Transform3d &instance_matrix_no_offset,
                        const Vec3d &point, double tolerance)
{
    const Vec3d instance_point = instance_matrix_no_offset * point;
    for (size_t i = 0; i < planes.size(); ++i) {
        const Pointf3s &outline = planes[i].outline;
        if (outline.empty())
            continue;
        const Vec3d p = planes[i].to_plane_frame * instance_point;
        // Facets with slightly different normals are merged into one face, so the outline is not exactly flat.
        const double z = std::accumulate(outline.begin(), outline.end(), 0., [](double sum, const Vec3d &v) { return sum + v.z(); }) / double(outline.size());
        if (std::abs(p.z() - z) > tolerance)
            continue;
        // The outline is convex: the point is inside when it is not on both sides of its edges.
        bool left = false, right = false;
        for (size_t j = 0; j < outline.size(); ++j) {
            const Vec2d  a    = outline[j].head<2>();
            const Vec2d  edge = outline[(j + 1) % outline.size()].head<2>() - a;
            const double len  = edge.norm();
            if (len < EPSILON)
                continue;
            const double side = cross2(edge, Vec2d(p.head<2>() - a)) / len;
            left  |= side > tolerance;
            right |= side < -tolerance;
        }
        if (!(left && right))
            return int(i);
    }
    return -1;
}

void lay_on_face(ModelObject &object, size_t instance_idx, const Vec3d &normal)
{
    ModelInstance                  &instance = *object.instances[instance_idx];
    const Geometry::Transformation &trafo    = instance.get_transformation();
    // Same rotation as Selection::flattening_rotate(): turn the transformed normal to point down.
    const Vec3d       tnormal  = trafo.get_matrix().matrix().block(0, 0, 3, 3).inverse().transpose() * normal;
    const Transform3d rotation = Transform3d(Eigen::Quaterniond().setFromTwoVectors(tnormal, -Vec3d::UnitZ()));
    instance.set_transformation(Geometry::Transformation(trafo.get_offset_matrix() * rotation * trafo.get_matrix_no_offset()));
    // Drop this instance only: ensure_on_bed() skips instances without auto_drop and measures the first instance.
    object.translate_instance(instance_idx, -object.instance_bounding_box(instance_idx).min.z() * Vec3d::UnitZ());
}

} // namespace Slic3r
