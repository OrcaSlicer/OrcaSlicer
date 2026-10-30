#include "libslic3r/PathTracer.hpp"
#include "libslic3r/Point.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <atomic>
#include <cmath>
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

TEST_CASE("Path tracer appends transformed mesh triangles", "[PathTracer]")
{
    const std::vector<Vec3f> vertices = {Vec3f{0.0f, 0.0f, 0.0f}, Vec3f{1.0f, 0.0f, 0.0f}, Vec3f{0.0f, 1.0f, 0.0f}};
    const std::vector<Vec3i32> indices = {Vec3i32{0, 1, 2}};
    Transform3d transform = Transform3d::Identity();
    transform.translate(Vec3d{2.0, 3.0, 4.0});

    std::vector<PathTraceTriangle> triangles;
    append_path_trace_triangles(triangles, vertices, indices, transform, Vec3f{0.2f, 0.4f, 0.6f});

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
    Transform3d transform = Transform3d::Identity();

    append_path_trace_triangles(triangles, vertices, indices, transform, Vec4f{0.8f, 0.2f, 0.1f, 0.45f});

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

TEST_CASE("Path tracer smooths normals across shallow folds and keeps sharp edges", "[PathTracer]")
{
    // Two triangles sharing the edge from (0, 0, 0) to (0, 1, 0), the second one folded about it.
    const auto fold = [](float degrees, bool smooth_normals = true) {
        const float angle = degrees * 3.14159265f / 180.0f;
        const std::vector<Vec3f> vertices = {Vec3f{0.0f, 0.0f, 0.0f}, Vec3f{0.0f, 1.0f, 0.0f}, Vec3f{-1.0f, 0.5f, 0.0f},
                                             Vec3f{std::cos(angle), 0.5f, std::sin(angle)}};
        const std::vector<Vec3i32> indices = {Vec3i32{0, 1, 2}, Vec3i32{1, 0, 3}};
        std::vector<PathTraceTriangle> triangles;
        append_path_trace_triangles(triangles, vertices, indices, Transform3d::Identity(), Vec3f{0.5f, 0.5f, 0.5f}, smooth_normals);
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
