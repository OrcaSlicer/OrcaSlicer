// MeshInspect.cpp — CLI mesh-inspection primitives. See MeshInspect.hpp.
#include "MeshInspect.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Point.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r {
namespace MeshInspect {

namespace {

// Per-triangle data: unit outward normal + area (mm²). Iterated once per
// object to feed the planar-face clusterer.
struct TriInfo {
    Vec3d  normal;
    double area;
};

std::vector<TriInfo> collect_triangles_object(const ModelObject &mo)
{
    std::vector<TriInfo> tris;
    for (const ModelVolume *mv : mo.volumes) {
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
    return tris;
}

// One planar-face cluster: every triangle whose normal quantized to the same
// 0.001-precision (≈ 0.06° angular) lattice point. `normal` is the
// area-weighted mean of the cluster's triangle normals (smoother than any
// raw triangle normal).
struct FaceCluster {
    Vec3d  normal;
    double area_mm2;
    int    n_triangles;
};

// Cluster triangles by quantized normal direction; return sorted by area
// (largest first). Coplanar triangles from triangulation share a normal to
// many decimals — quantizing to 0.001 groups them while keeping distinct
// face orientations apart.
std::vector<FaceCluster> compute_face_clusters(const std::vector<TriInfo> &tris)
{
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

    struct Bucket { double area = 0.0; int n = 0; Vec3d weighted_normal = Vec3d::Zero(); };
    std::unordered_map<QKey, Bucket, QKeyHash, QKeyEq> buckets;
    buckets.reserve(tris.size() / 4 + 1);

    for (const TriInfo &t : tris) {
        Bucket &b = buckets[quantize(t.normal)];
        b.area            += t.area;
        b.n               += 1;
        b.weighted_normal += t.area * t.normal;
    }

    std::vector<FaceCluster> clusters;
    clusters.reserve(buckets.size());
    for (const auto &kv : buckets)
        clusters.push_back({ kv.second.weighted_normal.normalized(),
                             kv.second.area, kv.second.n });

    std::sort(clusters.begin(), clusters.end(),
              [](const FaceCluster &a, const FaceCluster &b) { return a.area_mm2 > b.area_mm2; });
    return clusters;
}

// Minimal JSON emitter. Output is small and flat — hand-formatting beats
// pulling in a JSON library for this scope. Floats use %.6g (enough for
// mesh coords, no trailing noise); strings backslash-escape quotes and
// backslashes only — mesh-local numbers and file paths don't carry control
// characters in this pipeline.
std::string json_str(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

std::string json_num(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

std::string json_vec3(const Vec3d &v)
{
    return "[" + json_num(v.x()) + ", " + json_num(v.y()) + ", " + json_num(v.z()) + "]";
}

} // namespace

void inspect_to_json(const Model &model, const std::string &source_path,
                     std::ostream &out, int top_n_clusters)
{
    out << "{\n";
    out << "  " << json_str("source") << ": " << json_str(source_path) << ",\n";
    out << "  " << json_str("note") << ": " << json_str(
        "Coordinates are mesh-local. bbox_world reflects instance 0's transform.") << ",\n";
    out << "  " << json_str("objects") << ": [";

    bool first_obj = true;
    for (size_t oi = 0; oi < model.objects.size(); ++oi) {
        const ModelObject *mo = model.objects[oi];
        if (!mo) continue;

        const std::vector<TriInfo>     tris     = collect_triangles_object(*mo);
        const std::vector<FaceCluster> clusters = compute_face_clusters(tris);

        // Mesh-local bbox (raw vertices, no instance transform).
        BoundingBoxf3 bbox;
        bool have_box = false;
        for (const ModelVolume *mv : mo->volumes) {
            if (!mv || !mv->is_model_part()) continue;
            for (const Vec3f &v : mv->mesh().its.vertices) {
                if (!have_box) { bbox.min = bbox.max = v.cast<double>(); have_box = true; }
                else            bbox.merge(v.cast<double>());
            }
        }
        Vec3d size     = have_box ? Vec3d(bbox.max - bbox.min) : Vec3d::Zero();
        Vec3d centroid = have_box ? Vec3d(0.5 * (bbox.min + bbox.max)) : Vec3d::Zero();

        // World bbox = mesh + instance offset of instance 0 (typical CLI case
        // is one instance). Skipped if there are no instances.
        BoundingBoxf3 world_bbox;
        bool have_world = false;
        if (!mo->instances.empty()) {
            world_bbox = mo->instance_bounding_box(0, false);
            have_world = true;
        }

        out << (first_obj ? "\n" : ",\n");
        first_obj = false;
        out << "    {\n";
        out << "      " << json_str("name")           << ": " << json_str(mo->name) << ",\n";
        out << "      " << json_str("triangle_count") << ": " << tris.size() << ",\n";
        out << "      " << json_str("instance_count") << ": " << mo->instances.size() << ",\n";
        out << "      " << json_str("bbox_mesh_local") << ": {\n"
            << "        " << json_str("min")  << ": " << json_vec3(bbox.min)  << ",\n"
            << "        " << json_str("max")  << ": " << json_vec3(bbox.max)  << ",\n"
            << "        " << json_str("size") << ": " << json_vec3(size)      << ",\n"
            << "        " << json_str("centroid") << ": " << json_vec3(centroid) << "\n"
            << "      },\n";
        if (have_world) {
            out << "      " << json_str("bbox_world") << ": {\n"
                << "        " << json_str("min") << ": " << json_vec3(world_bbox.min) << ",\n"
                << "        " << json_str("max") << ": " << json_vec3(world_bbox.max) << "\n"
                << "      },\n";
            out << "      " << json_str("instance_offset") << ": "
                << json_vec3(mo->instances[0]->get_offset()) << ",\n";
        }

        // Top-N planar-face clusters. Ranked by total area; the largest is
        // almost always the natural "sit-flat" candidate for orientation.
        const int n = std::min<int>(top_n_clusters, int(clusters.size()));
        out << "      " << json_str("face_clusters") << ": [";
        for (int i = 0; i < n; ++i) {
            const FaceCluster &c = clusters[i];
            out << (i == 0 ? "\n" : ",\n");
            out << "        {"
                << json_str("normal")      << ": " << json_vec3(c.normal)       << ", "
                << json_str("area_mm2")    << ": " << json_num(c.area_mm2)      << ", "
                << json_str("n_triangles") << ": " << c.n_triangles
                << "}";
        }
        if (n > 0) out << "\n      ";
        out << "],\n";
        out << "      " << json_str("total_cluster_count") << ": " << clusters.size() << "\n";
        out << "    }";
    }
    if (!first_obj) out << "\n  ";
    out << "]\n";
    out << "}\n";
}

} // namespace MeshInspect
} // namespace Slic3r
