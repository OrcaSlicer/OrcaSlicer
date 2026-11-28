#include "libslic3r/PathTracer.hpp"
#include "libslic3r/Point.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace Slic3r;

TEST_CASE("Path tracer packs triangle BVH data for the GPU", "[PathTracer]")
{
    PathTraceTriangle triangle;
    triangle.a = Vec3f{-1.0f, -1.0f, 0.0f};
    triangle.b = Vec3f{1.0f, -1.0f, 0.0f};
    triangle.c = Vec3f{0.0f, 1.0f, 0.0f};

    const PathTraceGpuScene scene = build_path_trace_gpu_scene({triangle});
    REQUIRE(scene.triangles.size() == 1);
    REQUIRE(scene.nodes.size() == 1);
    REQUIRE(scene.original_indices.size() == 1);
    REQUIRE(scene.original_indices[0] == 0);
    REQUIRE(scene.nodes[0].left_or_first == 0);
    REQUIRE(scene.nodes[0].count() == 1);
    REQUIRE(scene.nodes[0].bounds_min[0] == -1.0f);
    REQUIRE(scene.nodes[0].bounds_max[1] == 1.0f);
}

TEST_CASE("Path tracer appends mesh triangles", "[PathTracer]")
{
    const std::vector<Vec3f> vertices = {Vec3f{2.0f, 3.0f, 4.0f}, Vec3f{3.0f, 3.0f, 4.0f}, Vec3f{2.0f, 4.0f, 4.0f}};
    const std::vector<Vec3i32> indices = {Vec3i32{0, 1, 2}};

    std::vector<PathTraceTriangle> triangles;
    append_path_trace_triangles(triangles, vertices, indices, Vec3f{0.2f, 0.4f, 0.6f});

    REQUIRE(triangles.size() == 1);
    REQUIRE(triangles[0].a.x() == Catch::Approx(2.0f));
    REQUIRE(triangles[0].b.y() == Catch::Approx(3.0f));
    REQUIRE(triangles[0].c.z() == Catch::Approx(4.0f));
    REQUIRE(triangles[0].albedo.y() == Catch::Approx(0.4f));
}

TEST_CASE("Path tracer packs hierarchical BVH data for many triangles", "[PathTracer]")
{
    std::vector<PathTraceTriangle> triangles;
    for (int i = 0; i < 10; ++i) {
        PathTraceTriangle tri;
        tri.a = Vec3f{float(i), 0.0f, 0.0f};
        tri.b = Vec3f{float(i) + 1.0f, 0.0f, 0.0f};
        tri.c = Vec3f{float(i), 1.0f, 0.0f};
        triangles.push_back(tri);
    }
    const PathTraceGpuScene scene = build_path_trace_gpu_scene(triangles);
    REQUIRE(scene.triangles.size() == 10);
    REQUIRE(scene.nodes.size() > 1);
    REQUIRE(scene.nodes[0].count() == 0);
    REQUIRE(scene.nodes[0].left_or_first >= 0);
}

TEST_CASE("Path tracer packs transparent triangles and preserves opacity for GPU", "[PathTracer]")
{
    std::vector<PathTraceTriangle> triangles;
    const std::vector<Vec3f> vertices = {Vec3f{0.0f, 0.0f, 0.0f}, Vec3f{1.0f, 0.0f, 0.0f}, Vec3f{0.0f, 1.0f, 0.0f}};
    const std::vector<Vec3i32> indices = {Vec3i32{0, 1, 2}};

    append_path_trace_triangles(triangles, vertices, indices, Vec4f{0.8f, 0.2f, 0.1f, 0.45f});

    REQUIRE(triangles.size() == 1);
    REQUIRE(triangles[0].albedo.x() == Catch::Approx(0.8f));
    REQUIRE(triangles[0].albedo.w() == Catch::Approx(0.45f));

    const PathTraceGpuScene scene = build_path_trace_gpu_scene(triangles);
    REQUIRE(scene.triangles.size() == 1);
    REQUIRE(scene.shadings[0].albedo[0] == Catch::Approx(0.8f));
    REQUIRE(scene.shadings[0].albedo[3] == Catch::Approx(0.45f));
    REQUIRE(scene.triangles[0].c[3] == Catch::Approx(0.45f));
}
TEST_CASE("Path tracer packs split axis correctly for small nodes", "[PathTracer]")
{
    std::vector<PathTraceTriangle> triangles;
    for (int i = 0; i < 8; ++i) {
        PathTraceTriangle tri;
        float z = float(i) * 2.0f;
        tri.a = Vec3f{0.0f, 0.0f, z};
        tri.b = Vec3f{0.1f, 0.0f, z};
        tri.c = Vec3f{0.0f, 0.1f, z};
        triangles.push_back(tri);
    }
    const PathTraceGpuScene scene = build_path_trace_gpu_scene(triangles);
    REQUIRE(scene.triangles.size() == 8);
    REQUIRE(!scene.nodes.empty());
    REQUIRE(scene.nodes[0].count() == 0);
    REQUIRE(scene.nodes[0].split_axis() == 2);
}

TEST_CASE("Path tracer GPU scene build honors cancellation", "[PathTracer]")
{
    PathTraceTriangle triangle;
    triangle.a = Vec3f{-1.0f, -1.0f, 0.0f};
    triangle.b = Vec3f{1.0f, -1.0f, 0.0f};
    triangle.c = Vec3f{0.0f, 1.0f, 0.0f};

    std::atomic<bool> cancel_token{true};
    const PathTraceGpuScene scene = build_path_trace_gpu_scene({triangle}, &cancel_token);
    REQUIRE(scene.nodes.empty());
    REQUIRE(scene.triangles.empty());
}

TEST_CASE("Path tracer encodes unit vectors in 32 bits", "[PathTracer]")
{
    const Vec3f direction = GENERATE(Vec3f{0.0f, 0.0f, 1.0f}, Vec3f{0.0f, 0.0f, -1.0f}, Vec3f{1.0f, 0.0f, 0.0f},
                                     Vec3f{0.3f, -0.5f, 0.8f}, Vec3f{-0.7f, 0.2f, -0.6f}, Vec3f{-0.1f, -0.9f, -0.2f});
    const Vec3f unit = direction.normalized();
    const Vec3f decoded = unpack_unit_vector_oct(pack_unit_vector_oct(unit));
    REQUIRE_THAT(decoded.x(), Catch::Matchers::WithinAbs(unit.x(), 1e-3));
    REQUIRE_THAT(decoded.y(), Catch::Matchers::WithinAbs(unit.y(), 1e-3));
    REQUIRE_THAT(decoded.z(), Catch::Matchers::WithinAbs(unit.z(), 1e-3));
}

TEST_CASE("Path tracer packs shading normals only on request", "[PathTracer]")
{
    PathTraceTriangle triangle;
    triangle.a = Vec3f{-1.0f, -1.0f, 0.0f};
    triangle.b = Vec3f{1.0f, -1.0f, 0.0f};
    triangle.c = Vec3f{0.0f, 1.0f, 0.0f};

    const PathTraceGpuScene with_normals = build_path_trace_gpu_scene({triangle});
    REQUIRE(with_normals.normals.size() == with_normals.triangles.size());

    const PathTraceGpuScene without_normals = build_path_trace_gpu_scene({triangle}, nullptr, false);
    REQUIRE(without_normals.triangles.size() == 1);
    REQUIRE(without_normals.normals.empty());
}

TEST_CASE("Path tracer smooths normals across shallow folds and keeps sharp edges", "[PathTracer]")
{
    // Two triangles sharing the edge from (0, 0, 0) to (0, 1, 0), the second one folded about it.
    const auto fold = [](float degrees, bool smooth_normals = true) {
        const float angle = degrees * 3.14159265f / 180.0f;
        const std::vector<Vec3f> vertices = {Vec3f{0.0f, 0.0f, 0.0f}, Vec3f{0.0f, 1.0f, 0.0f}, Vec3f{-1.0f, 0.5f, 0.0f},
                                             Vec3f{std::cos(angle), 0.5f, std::sin(angle)}};
        const std::vector<Vec3i32> indices = {Vec3i32{0, 1, 2}, Vec3i32{1, 0, 3}};
        std::vector<PathTraceTriangle> triangles;
        append_path_trace_triangles(triangles, vertices, indices, Vec3f{0.5f, 0.5f, 0.5f}, smooth_normals);
        return triangles;
    };

    SECTION("a 20 degree fold shades smooth across the shared edge")
    {
        const std::vector<PathTraceTriangle> triangles = fold(20.0f);
        REQUIRE(triangles.size() == 2);
        REQUIRE(triangles[0].smooth);
        // The shared vertex (0, 0, 0) is corner a of the first triangle and b of the second.
        const Vec3f first = unpack_unit_vector_oct(triangles[0].vertex_normals[0]);
        const Vec3f second = unpack_unit_vector_oct(triangles[1].vertex_normals[1]);
        REQUIRE_THAT(first.dot(second), Catch::Matchers::WithinAbs(1.0, 1e-3));
        // Halfway between the two faces, so it leans 10 degrees off the flat one.
        REQUIRE_THAT(first.z(), Catch::Matchers::WithinAbs(std::cos(10.0f * 3.14159265f / 180.0f), 2e-3));
        // The corner the faces do not share keeps the face normal.
        REQUIRE_THAT(unpack_unit_vector_oct(triangles[0].vertex_normals[2]).z(), Catch::Matchers::WithinAbs(1.0, 1e-3));
    }

    SECTION("a 60 degree fold keeps the edge sharp")
    {
        const std::vector<PathTraceTriangle> triangles = fold(60.0f);
        const Vec3f first = unpack_unit_vector_oct(triangles[0].vertex_normals[0]);
        REQUIRE_THAT(first.z(), Catch::Matchers::WithinAbs(1.0, 1e-3));
    }

    SECTION("without smooth normals every triangle shades flat")
    {
        const std::vector<PathTraceTriangle> triangles = fold(20.0f, false);
        REQUIRE(triangles.size() == 2);
        REQUIRE_FALSE(triangles[0].smooth);
        REQUIRE_FALSE(triangles[1].smooth);
    }
}

namespace {

// A bead 10 mm long along x at mid height 0.1 mm, 0.45 mm wide and 0.2 mm high: round sides of
// radius 0.1 mm either side of a flat top 0.25 mm wide.
PathTraceBead test_bead(const Vec3f& offset = Vec3f::Zero())
{
    PathTraceBead bead;
    bead.p0 = offset + Vec3f{0.0f, 0.0f, 0.1f};
    bead.p1 = offset + Vec3f{10.0f, 0.0f, 0.1f};
    bead.width = 0.45f;
    bead.height = 0.2f;
    return bead;
}

// Distance from the surface of a bead in double: from the segment in the plane of the bead, less the
// half width of its flat top, then from that region less the radius.
double reference_bead_distance(const PathTraceBead& bead, const Vec3d& p)
{
    const Vec3d axis = (bead.p1 - bead.p0).cast<double>().normalized();
    const double length = (bead.p1 - bead.p0).cast<double>().norm();
    const Vec3d right = Vec3d{axis.y(), -axis.x(), 0.0}.normalized();
    const Vec3d up = right.cross(axis);
    const Vec3d q = p - bead.p0.cast<double>();
    const double x = q.dot(axis);
    const double dl = std::hypot(x - std::clamp(x, 0.0, length), q.dot(right));
    return std::hypot(std::max(dl - double(bead.flat_half_width()), 0.0), q.dot(up)) - double(bead.radius());
}

} // namespace

TEST_CASE("Path tracer bead bounds enclose the bead tightly", "[PathTracer]")
{
    const PathTraceBead bead = test_bead();
    Vec3f lo, hi;
    path_trace_bead_bounds(bead, lo, hi);
    // Half the width past the ends and to the sides, half the height above and below.
    REQUIRE_THAT(lo.x(), Catch::Matchers::WithinAbs(-0.225, 1e-6));
    REQUIRE_THAT(hi.x(), Catch::Matchers::WithinAbs(10.225, 1e-5));
    REQUIRE_THAT(hi.y(), Catch::Matchers::WithinAbs(0.225, 1e-6));
    REQUIRE_THAT(lo.z(), Catch::Matchers::WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(hi.z(), Catch::Matchers::WithinAbs(0.2, 1e-6));

    // A diagonal bead: its round ends reach half the width past each end point in x and y.
    PathTraceBead diagonal = bead;
    diagonal.p1 = Vec3f{3.0f, 4.0f, 0.1f};
    path_trace_bead_bounds(diagonal, lo, hi);
    REQUIRE_THAT(lo.x(), Catch::Matchers::WithinAbs(-0.225, 1e-6));
    REQUIRE_THAT(hi.y(), Catch::Matchers::WithinAbs(4.225, 1e-5));
    REQUIRE_THAT(hi.z(), Catch::Matchers::WithinAbs(0.2, 1e-6));
}

TEST_CASE("Path tracer builds a BVH over bead bounds", "[PathTracer]")
{
    // A wall of 20 layers of 50 beads, each 1 mm long.
    std::vector<PathTraceBead> beads;
    for (int layer = 0; layer < 20; ++layer)
        for (int i = 0; i < 50; ++i) {
            PathTraceBead bead = test_bead(Vec3f{float(i), 0.0f, 0.2f * float(layer)});
            bead.p1 = bead.p0 + Vec3f{1.0f, 0.0f, 0.0f};
            bead.albedo = Vec4f{0.1f, 0.2f, 0.3f, 0.5f};
            beads.push_back(bead);
        }
    const PathTraceGpuScene scene = build_path_trace_gpu_scene(beads);
    REQUIRE(scene.triangles.size() == beads.size());
    REQUIRE(scene.original_indices.size() == beads.size());
    REQUIRE(scene.normals.empty());
    REQUIRE(scene.nodes.size() > 1);

    // Every bead sits in one leaf, whose box holds its bounds.
    std::vector<int> seen(beads.size(), 0);
    for (const PathTraceGpuNode& node : scene.nodes) {
        if (!node.is_leaf())
            continue;
        for (uint32_t slot = uint32_t(node.left_or_first); slot < uint32_t(node.left_or_first) + node.count(); ++slot) {
            const size_t source = scene.original_indices[slot];
            ++seen[source];
            Vec3f lo, hi;
            path_trace_bead_bounds(beads[source], lo, hi);
            for (int axis = 0; axis < 3; ++axis) {
                REQUIRE(node.bounds_min[axis] <= lo[axis]);
                REQUIRE(node.bounds_max[axis] >= hi[axis]);
            }
            // Packed with its end points, flat half width, radius and opacity.
            const PathTraceGpuTriangle& packed = scene.triangles[slot];
            REQUIRE_THAT(packed.a[0], Catch::Matchers::WithinAbs(beads[source].p0.x(), 1e-6));
            REQUIRE_THAT(packed.b[0], Catch::Matchers::WithinAbs(beads[source].p1.x(), 1e-6));
            REQUIRE_THAT(packed.c[0], Catch::Matchers::WithinAbs(0.125, 1e-6));
            REQUIRE_THAT(packed.c[1], Catch::Matchers::WithinAbs(0.1, 1e-6));
            REQUIRE_THAT(packed.c[3], Catch::Matchers::WithinAbs(0.5, 1e-6));
            REQUIRE_THAT(scene.shadings[slot].albedo[2], Catch::Matchers::WithinAbs(0.3, 1e-6));
        }
    }
    for (int count : seen)
        REQUIRE(count == 1);

    std::atomic<bool> cancel_token{true};
    REQUIRE(build_path_trace_gpu_scene(beads, &cancel_token).nodes.empty());
}

TEST_CASE("Path tracer intersects a bead exactly", "[PathTracer]")
{
    const PathTraceBead bead = test_bead();
    float t_in = 0.0f;
    float t_out = 0.0f;

    SECTION("from above: the flat top at the top of the layer, crossed through its height")
    {
        REQUIRE(intersect_path_trace_bead(bead, Vec3f{5.0f, 0.05f, 10.0f}, -Vec3f::UnitZ(), t_in, t_out));
        REQUIRE_THAT(t_in, Catch::Matchers::WithinAbs(9.8, 1e-5));
        REQUIRE_THAT(t_out - t_in, Catch::Matchers::WithinAbs(0.2, 1e-5));
        REQUIRE_THAT(path_trace_bead_normal(bead, Vec3f{5.0f, 0.05f, 0.2f}).z(), Catch::Matchers::WithinAbs(1.0, 1e-6));
    }
    SECTION("from above over a round side: the half circle")
    {
        // 0.05 mm out from the flat top, the side lies sqrt(0.1^2 - 0.05^2) above mid height.
        REQUIRE(intersect_path_trace_bead(bead, Vec3f{5.0f, 0.175f, 10.0f}, -Vec3f::UnitZ(), t_in, t_out));
        const float side = std::sqrt(0.01f - 0.0025f);
        REQUIRE_THAT(t_in, Catch::Matchers::WithinAbs(9.9 - side, 1e-5));
        REQUIRE_THAT(t_out - t_in, Catch::Matchers::WithinAbs(2.0 * side, 1e-5));
        const Vec3f n = path_trace_bead_normal(bead, Vec3f{5.0f, 0.175f, 10.0f - t_in});
        REQUIRE_THAT(n.y(), Catch::Matchers::WithinAbs(0.5, 1e-4));
        REQUIRE_THAT(n.z(), Catch::Matchers::WithinAbs(side / 0.1, 1e-4));
    }
    SECTION("across at mid height: through the whole width")
    {
        REQUIRE(intersect_path_trace_bead(bead, Vec3f{5.0f, -3.0f, 0.1f}, Vec3f::UnitY(), t_in, t_out));
        REQUIRE_THAT(t_in, Catch::Matchers::WithinAbs(3.0 - 0.225, 1e-5));
        REQUIRE_THAT(t_out - t_in, Catch::Matchers::WithinAbs(0.45, 1e-5));
    }
    SECTION("along the axis: through both round ends")
    {
        REQUIRE(intersect_path_trace_bead(bead, Vec3f{-5.0f, 0.0f, 0.1f}, Vec3f::UnitX(), t_in, t_out));
        REQUIRE_THAT(t_in, Catch::Matchers::WithinAbs(5.0 - 0.225, 1e-5));
        REQUIRE_THAT(t_out - t_in, Catch::Matchers::WithinAbs(10.45, 1e-4));
        REQUIRE_THAT(path_trace_bead_normal(bead, Vec3f{-0.225f, 0.0f, 0.1f}).x(), Catch::Matchers::WithinAbs(-1.0, 1e-6));
    }
    SECTION("from inside: entered behind the origin, left ahead of it")
    {
        REQUIRE(intersect_path_trace_bead(bead, Vec3f{5.0f, 0.0f, 0.1f}, Vec3f::UnitZ(), t_in, t_out));
        REQUIRE_THAT(t_in, Catch::Matchers::WithinAbs(-0.1, 1e-5));
        REQUIRE_THAT(t_out, Catch::Matchers::WithinAbs(0.1, 1e-5));
    }
    SECTION("beside, above, and over the corner of a round end: misses")
    {
        REQUIRE_FALSE(intersect_path_trace_bead(bead, Vec3f{5.0f, 0.23f, 10.0f}, -Vec3f::UnitZ(), t_in, t_out));
        REQUIRE_FALSE(intersect_path_trace_bead(bead, Vec3f{-5.0f, 0.0f, 0.21f}, Vec3f::UnitX(), t_in, t_out));
        // Inside the corner of the box around the end, outside the round end.
        REQUIRE_FALSE(intersect_path_trace_bead(bead, Vec3f{-0.2f, 0.2f, 10.0f}, -Vec3f::UnitZ(), t_in, t_out));
    }
}

TEST_CASE("Path tracer bead intersection agrees with the distance to the bead", "[PathTracer]")
{
    // Also far from the origin, as on a large bed seen from far away: the intersection takes the ray
    // relative to the bead's start to keep its precision.
    const Vec3f offset = GENERATE(Vec3f{0.0f, 0.0f, 0.0f}, Vec3f{250.0f, 230.0f, 120.0f});
    PathTraceBead bead = test_bead(offset);
    bead.p1 = bead.p0 + Vec3f{1.5f, 0.8f, 0.0f};
    const Vec3f center = 0.5f * (bead.p0 + bead.p1);

    uint32_t state = 12345u;
    const auto uniform = [&state]() {
        state = state * 1664525u + 1013904223u;
        return float(state >> 8) / 16777216.0f;
    };
    int hits = 0;
    for (int i = 0; i < 2000; ++i) {
        // Rays from 300 mm away towards points around the bead.
        const Vec3f target = center + Vec3f{2.0f * uniform() - 1.0f, 2.0f * uniform() - 1.0f, 0.3f * (2.0f * uniform() - 1.0f)};
        const float z = 2.0f * uniform() - 1.0f;
        const float phi = 6.2831853f * uniform();
        const Vec3f dir = -Vec3f{std::sqrt(1.0f - z * z) * std::cos(phi), std::sqrt(1.0f - z * z) * std::sin(phi), z};
        const Vec3f origin = target - 300.0f * dir;
        float t_in = 0.0f;
        float t_out = 0.0f;
        const bool hit = intersect_path_trace_bead(bead, origin, dir, t_in, t_out);
        // Sphere trace the ray in double to find where it enters.
        const Vec3d o = origin.cast<double>();
        const Vec3d d = dir.cast<double>();
        double entry = -1.0;
        for (double t = 298.0; t < 302.0;) {
            const double f = reference_bead_distance(bead, o + t * d);
            if (f < 1e-7) {
                entry = t;
                break;
            }
            t += std::max(f, 1e-7);
        }
        if (entry < 0.0) {
            // A miss; a hit there can only be a graze.
            if (hit)
                REQUIRE(reference_bead_distance(bead, o + double(t_in) * d) < 2e-4);
            continue;
        }
        ++hits;
        if (!hit) {
            // Only a graze may be missed: a millimetre on, the ray is outside again or barely inside.
            REQUIRE(reference_bead_distance(bead, o + (entry + 1e-3) * d) > -1e-4);
            continue;
        }
        // The hit lies on the surface within the tolerance of the Newton steps, 1e-4 mm, and the float
        // precision of a ray 300 mm long; along a grazing ray that precision stretches.
        REQUIRE_THAT(reference_bead_distance(bead, o + double(t_in) * d), Catch::Matchers::WithinAbs(0.0, 1.5e-4));
        REQUIRE_THAT(t_in, Catch::Matchers::WithinAbs(entry, 2e-3));
        REQUIRE(t_out >= t_in);
        const Vec3f n = path_trace_bead_normal(bead, origin + t_in * dir);
        REQUIRE_THAT(n.norm(), Catch::Matchers::WithinAbs(1.0, 1e-5));
        // Facing the ray where it enters.
        REQUIRE(n.dot(dir) <= 1e-3f);
    }
    REQUIRE(hits > 100);
}
