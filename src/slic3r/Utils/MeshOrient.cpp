// MeshOrient.cpp — CLI orientation primitives. See MeshOrient.hpp.
#include "MeshOrient.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Point.hpp"

#include <boost/log/trivial.hpp>
#include <boost/format.hpp>

#include <Eigen/Geometry>

#include <cmath>
#include <map>
#include <unordered_map>
#include <limits>
#include <utility>

namespace Slic3r {
namespace MeshOrient {

namespace {

// Per-triangle data: outward normal (unit) + area (mm²).
struct TriInfo {
    Vec3d  normal;
    double area;
};

// Iterate every triangle of every volume of every object, return their unit
// normals + areas (all in mesh-local coords). Skips degenerate (zero-area)
// triangles. The caller is interested in the surface composition, not which
// triangle came from where — area is the only weight that matters.
std::vector<TriInfo> collect_triangles(const Model &model)
{
    std::vector<TriInfo> tris;
    for (const ModelObject *mo : model.objects) {
        if (!mo) continue;
        for (const ModelVolume *mv : mo->volumes) {
            if (!mv || !mv->is_model_part()) continue;
            const indexed_triangle_set &its = mv->mesh().its;
            tris.reserve(tris.size() + its.indices.size());
            for (const Vec3i32 &tri : its.indices) {
                const Vec3f &a = its.vertices[tri[0]];
                const Vec3f &b = its.vertices[tri[1]];
                const Vec3f &c = its.vertices[tri[2]];
                const Vec3d ab = (b - a).cast<double>();
                const Vec3d ac = (c - a).cast<double>();
                const Vec3d cross  = ab.cross(ac);
                const double mag   = cross.norm();
                if (mag < 1e-12) continue;          // degenerate
                tris.push_back({ cross / mag, 0.5 * mag });
            }
        }
    }
    return tris;
}

// Apply a rotation that maps `target_normal_mesh` (mesh-local, will be
// normalized) to -Z, to every instance of every object. Same math as
// Selection::flattening_rotate (GUI Selection.cpp:1432) — quaternion from
// the world-space transformed normal to -Z, applied after the existing
// instance matrix (preserving offset).
void apply_ground_rotation(Model &model, const Vec3d &target_normal_mesh)
{
    const Vec3d n_mesh = target_normal_mesh.normalized();
    for (ModelObject *mo : model.objects) {
        if (!mo) continue;
        for (ModelInstance *inst : mo->instances) {
            if (!inst) continue;
            const Geometry::Transformation &t   = inst->get_transformation();
            // Transform the mesh-local normal into world coords via the
            // inverse-transpose of the rotation/scale (3x3 block).
            const Vec3d tnormal = t.get_matrix().matrix().block(0, 0, 3, 3)
                                      .inverse().transpose() * n_mesh;
            const Eigen::Quaterniond q = Eigen::Quaterniond()
                .setFromTwoVectors(tnormal.normalized(), -Vec3d::UnitZ());
            const Transform3d rotation(q);
            // Compose new matrix: offset * new_rotation * old_no_offset.
            const Transform3d new_matrix = t.get_offset_matrix()
                                         * rotation
                                         * t.get_matrix_no_offset();
            inst->set_transformation(Geometry::Transformation(new_matrix));
        }
        //ORCA: lift each instance so its grounded face sits exactly at Z=0.
        //      ModelObject::ensure_on_bed() is a no-op for CLI-loaded instances
        //      (it skips any instance whose auto_drop flag is false, and CLI
        //      loaders default that to false). Slicer-chat 2026-06-22 hit this:
        //      after Quaterniond rotation the grounded face landed at z≈-1e-9
        //      due to FP, the slicer then rejected the model with the cryptic
        //      "No layers were detected" error. Apply the lift directly: per
        //      instance, compute its world-coord bbox min.z and shift the
        //      offset by -min.z (zero if already on or above bed).
        for (size_t i = 0; i < mo->instances.size(); ++i) {
            ModelInstance *inst = mo->instances[i];
            if (!inst) continue;
            const BoundingBoxf3 ib = mo->instance_bounding_box(i, false);
            const double min_z = ib.min.z();
            if (min_z != 0.0) {                          // covers below AND above
                Vec3d o = inst->get_offset();
                o.z() -= min_z;
                inst->set_offset(o);
            }
        }
    }
}

} // namespace

bool ground_largest_face(Model &model, std::string *out_chosen_normal)
{
    const std::vector<TriInfo> tris = collect_triangles(model);
    if (tris.empty()) {
        BOOST_LOG_TRIVIAL(error) << "MeshOrient: model has no triangles to ground";
        return false;
    }

    // Cluster triangles by quantized normal direction. Quantize each normal
    // component to 0.001 (≈ 0.06° angular precision) — coplanar triangles
    // from triangulation share a normal to many decimals, so this groups
    // them while keeping distinct face orientations apart.
    struct QKey { int x, y, z; };
    struct QKeyHash { size_t operator()(const QKey &k) const noexcept {
        return (size_t(uint32_t(k.x)) * 73856093u)
             ^ (size_t(uint32_t(k.y)) * 19349663u)
             ^ (size_t(uint32_t(k.z)) * 83492791u);
    }};
    struct QKeyEq { bool operator()(const QKey &a, const QKey &b) const noexcept {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }};

    auto quantize = [](const Vec3d &n) -> QKey {
        return { int(std::lround(n.x() * 1000.0)),
                 int(std::lround(n.y() * 1000.0)),
                 int(std::lround(n.z() * 1000.0)) };
    };

    // Bucket sum-of-area per quantized normal, plus the area-weighted normal
    // sum so we can recover a precise representative direction at the end.
    struct Bucket { double area = 0.0; Vec3d weighted_normal = Vec3d::Zero(); };
    std::unordered_map<QKey, Bucket, QKeyHash, QKeyEq> buckets;
    buckets.reserve(tris.size() / 4 + 1);

    for (const TriInfo &t : tris) {
        Bucket &b = buckets[quantize(t.normal)];
        b.area            += t.area;
        b.weighted_normal += t.area * t.normal;
    }

    // Pick the bucket with maximum accumulated area.
    double max_area = -1.0;
    Vec3d  best_normal = Vec3d::UnitZ();
    for (const auto &kv : buckets) {
        if (kv.second.area > max_area) {
            max_area    = kv.second.area;
            best_normal = kv.second.weighted_normal.normalized();
        }
    }

    BOOST_LOG_TRIVIAL(info) << boost::format(
        "MeshOrient::ground_largest_face: %1% triangles, %2% distinct normals, "
        "largest cluster area=%3$.3f mm² normal=(%4$+.4f, %5$+.4f, %6$+.4f)")
        % tris.size() % buckets.size() % max_area
        % best_normal.x() % best_normal.y() % best_normal.z();

    if (out_chosen_normal)
        *out_chosen_normal = (boost::format("(%1$+.4f,%2$+.4f,%3$+.4f)")
                              % best_normal.x() % best_normal.y() % best_normal.z()).str();

    apply_ground_rotation(model, best_normal);
    return true;
}

bool ground_face_normal(Model &model, const Vec3d &target_normal)
{
    if (target_normal.norm() < 1e-9) {
        BOOST_LOG_TRIVIAL(error) << "MeshOrient::ground_face_normal: zero-length target normal";
        return false;
    }
    const Vec3d target = target_normal.normalized();

    const std::vector<TriInfo> tris = collect_triangles(model);
    if (tris.empty()) {
        BOOST_LOG_TRIVIAL(error) << "MeshOrient::ground_face_normal: model has no triangles";
        return false;
    }

    // Of all the triangles whose normal best matches `target`, pick the
    // representative as the area-weighted average of the cluster of
    // triangles within an angular tolerance of the best hit (0.5° ≈ 1e-4
    // in dot-product terms). This handles re-triangulated meshes where the
    // matching face was split into many triangles.
    double best_dot = -2.0;
    for (const TriInfo &t : tris)
        best_dot = std::max(best_dot, t.normal.dot(target));

    const double tol = 1e-4;
    Vec3d  weighted_normal = Vec3d::Zero();
    double cluster_area    = 0.0;
    for (const TriInfo &t : tris) {
        const double d = t.normal.dot(target);
        if (d >= best_dot - tol) {
            weighted_normal += t.area * t.normal;
            cluster_area    += t.area;
        }
    }
    const Vec3d chosen = weighted_normal.normalized();

    BOOST_LOG_TRIVIAL(info) << boost::format(
        "MeshOrient::ground_face_normal: target=(%1$+.4f,%2$+.4f,%3$+.4f) "
        "→ chosen=(%4$+.4f,%5$+.4f,%6$+.4f) cluster_area=%7$.3f mm² "
        "best_dot=%8$.6f")
        % target.x() % target.y() % target.z()
        % chosen.x() % chosen.y() % chosen.z()
        % cluster_area % best_dot;

    apply_ground_rotation(model, chosen);
    return true;
}

bool ground_face_point(Model &model, const Vec3d &point_mesh)
{
    // First pass: compute the mesh bbox centroid in mesh-local coords. When
    // multiple faces overlap the click point (internal + external surfaces
    // of a wall, both sides of an edge), we use this centroid to disambiguate:
    // prefer the face whose normal points AWAY from the centroid (the
    // "outward-facing" surface — what an operator pointing at the part
    // intuitively means by "this face").
    BoundingBoxf3 bbox;
    bool          have_box = false;
    for (const ModelObject *mo : model.objects) {
        if (!mo) continue;
        for (const ModelVolume *mv : mo->volumes) {
            if (!mv || !mv->is_model_part()) continue;
            for (const Vec3f &v : mv->mesh().its.vertices) {
                if (!have_box) { bbox.min = bbox.max = v.cast<double>(); have_box = true; }
                else            bbox.merge(v.cast<double>());
            }
        }
    }
    Vec3d mesh_centroid = Vec3d::Zero();
    if (have_box) mesh_centroid = 0.5 * (bbox.min + bbox.max);

    // Find every triangle that contains `point_mesh` (barycentric check),
    // then pick the most outward-facing one (highest dot product of normal
    // with the centroid→point direction). Ties broken by larger area.
    Vec3d   best_normal     = Vec3d::UnitZ();
    double  best_outward    = -2.0;
    double  best_area       = -1.0;
    int     hits            = 0;
    Vec3d outward_ref(0.0, 0.0, -1.0);
    if ((point_mesh - mesh_centroid).norm() > 1e-9)
        outward_ref = (point_mesh - mesh_centroid).normalized();

    for (const ModelObject *mo : model.objects) {
        if (!mo) continue;
        for (const ModelVolume *mv : mo->volumes) {
            if (!mv || !mv->is_model_part()) continue;
            const indexed_triangle_set &its = mv->mesh().its;
            for (const Vec3i32 &tri : its.indices) {
                const Vec3d a = its.vertices[tri[0]].cast<double>();
                const Vec3d b = its.vertices[tri[1]].cast<double>();
                const Vec3d c = its.vertices[tri[2]].cast<double>();
                const Vec3d ab = b - a;
                const Vec3d ac = c - a;
                const Vec3d cross = ab.cross(ac);
                const double area2 = cross.norm();           // 2× area
                if (area2 < 1e-12) continue;
                const Vec3d n = cross / area2;

                // Plane distance (point must lie in the triangle's plane).
                const Vec3d ap = point_mesh - a;
                const double plane_d = std::abs(ap.dot(n));
                if (plane_d > 1e-3) continue;

                // Barycentrics via projection onto ab/ac basis.
                const double d00 = ab.dot(ab);
                const double d01 = ab.dot(ac);
                const double d11 = ac.dot(ac);
                const double d20 = ap.dot(ab);
                const double d21 = ap.dot(ac);
                const double denom = d00 * d11 - d01 * d01;
                if (std::abs(denom) < 1e-12) continue;
                const double v = (d11 * d20 - d01 * d21) / denom;
                const double w = (d00 * d21 - d01 * d20) / denom;
                const double u = 1.0 - v - w;
                const double tol = 1e-3;
                if (u < -tol || v < -tol || w < -tol) continue;

                ++hits;
                const double outward = n.dot(outward_ref);
                const double tri_area = 0.5 * area2;
                // Outward direction wins; area is the tiebreaker.
                if (outward > best_outward + 1e-6 ||
                    (std::abs(outward - best_outward) <= 1e-6 && tri_area > best_area)) {
                    best_outward = outward;
                    best_area    = tri_area;
                    best_normal  = n;
                }
            }
        }
    }

    if (hits == 0) {
        BOOST_LOG_TRIVIAL(error) << boost::format(
            "MeshOrient::ground_face_point: no triangle contains point "
            "(%1$.3f, %2$.3f, %3$.3f) — give a point ON the mesh surface")
            % point_mesh.x() % point_mesh.y() % point_mesh.z();
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << boost::format(
        "MeshOrient::ground_face_point: point=(%1$.3f,%2$.3f,%3$.3f) hits=%4% "
        "chosen_normal=(%5$+.4f,%6$+.4f,%7$+.4f) area=%8$.3f mm²")
        % point_mesh.x() % point_mesh.y() % point_mesh.z()
        % hits
        % best_normal.x() % best_normal.y() % best_normal.z()
        % best_area;

    apply_ground_rotation(model, best_normal);
    return true;
}

bool center_on_bed(Model &model, const Vec2d &bed_center)
{
    // Compute the combined bounding box of every instance in world coords,
    // then translate every instance by (bed_center - bbox_xy_centroid).
    // Z is left alone — ensure_on_bed sets that.
    if (model.objects.empty()) {
        BOOST_LOG_TRIVIAL(error) << "MeshOrient::center_on_bed: model is empty";
        return false;
    }
    BoundingBoxf3 combined;
    bool          have_box = false;
    for (const ModelObject *mo : model.objects) {
        if (!mo) continue;
        for (size_t i = 0; i < mo->instances.size(); ++i) {
            const BoundingBoxf3 ib = mo->instance_bounding_box(i);
            if (!have_box) { combined = ib; have_box = true; }
            else            combined.merge(ib);
        }
    }
    if (!have_box) {
        BOOST_LOG_TRIVIAL(error) << "MeshOrient::center_on_bed: no instances to center";
        return false;
    }
    const Vec2d centroid_xy(0.5 * (combined.min.x() + combined.max.x()),
                            0.5 * (combined.min.y() + combined.max.y()));
    const Vec3d shift(bed_center.x() - centroid_xy.x(),
                      bed_center.y() - centroid_xy.y(),
                      0.0);
    for (ModelObject *mo : model.objects) {
        if (!mo) continue;
        for (ModelInstance *inst : mo->instances) {
            if (!inst) continue;
            inst->set_offset(inst->get_offset() + shift);
        }
    }
    BOOST_LOG_TRIVIAL(info) << boost::format(
        "MeshOrient::center_on_bed: bed=(%1$.1f,%2$.1f) centroid=(%3$.1f,%4$.1f) shift=(%5$+.1f,%6$+.1f)")
        % bed_center.x() % bed_center.y()
        % centroid_xy.x() % centroid_xy.y()
        % shift.x() % shift.y();
    return true;
}

} // namespace MeshOrient
} // namespace Slic3r
