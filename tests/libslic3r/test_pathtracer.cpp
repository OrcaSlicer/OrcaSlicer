#include "libslic3r/PathTracer.hpp"
#include "libslic3r/Point.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace Slic3r;

TEST_CASE("Path tracer produces a deterministic image of triangle geometry", "[PathTracer]")
{
    PathTraceTriangle triangle;
    triangle.a = Vec3f{-0.8f, -0.8f, 0.0f};
    triangle.b = Vec3f{0.8f, -0.8f, 0.0f};
    triangle.c = Vec3f{0.0f, 0.8f, 0.0f};

    PathTraceCamera camera;
    camera.position = Vec3f{0.0f, 0.0f, 3.0f};
    camera.forward = Vec3f{0.0f, 0.0f, -1.0f};

    PathTraceSettings settings;
    settings.width = 32;
    settings.height = 32;
    settings.samples_per_pixel = 5;
    settings.sample_batch_size = 2;
    settings.max_bounces = 2;

    std::vector<uint32_t> reported_sample_counts;
    const PathTraceImage first = render_path_traced_image({triangle}, camera, settings, {}, {},
        [&reported_sample_counts](const PathTraceImage& image) { reported_sample_counts.push_back(image.samples_per_pixel); });
    const PathTraceImage second = render_path_traced_image({triangle}, camera, settings);

    REQUIRE(first.width == 32);
    REQUIRE(first.height == 32);
    REQUIRE(first.samples_per_pixel == settings.samples_per_pixel);
    REQUIRE(reported_sample_counts == std::vector<uint32_t>{2, 4, 5});
    REQUIRE(first.rgb.size() == 32 * 32 * 3);
    REQUIRE(first.rgb == second.rgb);

    const size_t center = (16 * 32 + 16) * 3;
    const size_t corner = 0;
    REQUIRE(first.rgb[center] != first.rgb[corner]);
}

TEST_CASE("Path tracer honors cancellation", "[PathTracer]")
{
    PathTraceSettings settings;
    settings.width = 8;
    settings.height = 8;
    settings.samples_per_pixel = 1;
    settings.max_bounces = 1;

    const PathTraceImage image = render_path_traced_image({}, PathTraceCamera{}, settings, [] { return true; });
    REQUIRE(image.canceled);
    REQUIRE(image.rgb.empty());
}

TEST_CASE("Path tracer supports an orthographic camera", "[PathTracer]")
{
    PathTraceTriangle triangle;
    triangle.a = Vec3f{-0.8f, -0.8f, 0.0f};
    triangle.b = Vec3f{0.8f, -0.8f, 0.0f};
    triangle.c = Vec3f{0.0f, 0.8f, 0.0f};

    PathTraceCamera camera;
    camera.position = Vec3f{0.0f, 0.0f, 3.0f};
    camera.forward = Vec3f{0.0f, 0.0f, -1.0f};
    camera.orthographic = true;
    camera.orthographic_height = 2.0f;

    PathTraceSettings settings;
    settings.width = 16;
    settings.height = 16;
    settings.samples_per_pixel = 1;
    settings.max_bounces = 1;

    const PathTraceImage image = render_path_traced_image({triangle}, camera, settings);
    REQUIRE(image.rgb.size() == 16 * 16 * 3);
    REQUIRE(image.rgb[(8 * 16 + 8) * 3] != image.rgb[0]);
}

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

TEST_CASE("Path tracer reports the closer of two overlapping triangles", "[PathTracer]")
{
    PathTraceTriangle closer;
    closer.a = Vec3f{-0.8f, -0.8f, 0.0f};
    closer.b = Vec3f{0.8f, -0.8f, 0.0f};
    closer.c = Vec3f{0.0f, 0.8f, 0.0f};
    closer.albedo = Vec3f{1.0f, 0.0f, 0.0f};

    PathTraceTriangle farther;
    farther.a = Vec3f{-2.0f, -2.0f, -1.0f};
    farther.b = Vec3f{2.0f, -2.0f, -1.0f};
    farther.c = Vec3f{0.0f, 2.0f, -1.0f};
    farther.albedo = Vec3f{0.0f, 1.0f, 0.0f};

    PathTraceCamera camera;
    camera.position = Vec3f{0.0f, 0.0f, 3.0f};
    camera.forward = Vec3f{0.0f, 0.0f, -1.0f};

    PathTraceSettings settings;
    settings.width = 16;
    settings.height = 16;
    settings.samples_per_pixel = 1;
    settings.max_bounces = 1;

    const PathTraceImage both = render_path_traced_image({farther, closer}, camera, settings);
    const PathTraceImage only_closer = render_path_traced_image({closer}, camera, settings);
    const size_t center = (8 * 16 + 8) * 3;
    REQUIRE(both.rgb[center] == only_closer.rgb[center]);
    REQUIRE(both.rgb[center + 1] == only_closer.rgb[center + 1]);
    REQUIRE(both.rgb[center + 2] == only_closer.rgb[center + 2]);
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

TEST_CASE("Path tracer edge intersection test on quad diagonal", "[PathTracer]")
{
    // Planar quad in XY plane at z = 0 composed of two triangles sharing a diagonal edge from (0,0,0) to (1,1,0)
    PathTraceTriangle tri1;
    tri1.a = Vec3f{0.0f, 0.0f, 0.0f};
    tri1.b = Vec3f{1.0f, 0.0f, 0.0f};
    tri1.c = Vec3f{1.0f, 1.0f, 0.0f};
    tri1.albedo = Vec4f{1.0f, 0.0f, 0.0f, 1.0f};

    PathTraceTriangle tri2;
    tri2.a = Vec3f{0.0f, 0.0f, 0.0f};
    tri2.b = Vec3f{1.0f, 1.0f, 0.0f};
    tri2.c = Vec3f{0.0f, 1.0f, 0.0f};
    tri2.albedo = Vec4f{0.0f, 1.0f, 0.0f, 1.0f};

    std::vector<PathTraceTriangle> scene_triangles = {tri1, tri2};

    struct Ray {
        Vec3f origin;
        Vec3f direction;
    };

    struct Hit {
        float distance{1e30f};
        Vec3f position{Vec3f::Zero()};
        Vec3f normal{Vec3f::Zero()};
        bool hit{false};
    };

    auto intersect_triangle = [](const PathTraceTriangle& triangle, const Ray& ray, Hit& hit) -> bool {
        const Vec3f edge1 = triangle.b - triangle.a;
        const Vec3f edge2 = triangle.c - triangle.a;
        const Vec3f cross = ray.direction.cross(edge2);
        const float determinant = edge1.dot(cross);
        if (std::abs(determinant) < 1e-10f)
            return false;

        const float inverse_determinant = 1.0f / determinant;
        const Vec3f from_a = ray.origin - triangle.a;
        const float u = from_a.dot(cross) * inverse_determinant;
        if (u < 0.0f || u > 1.0f)
            return false;
        const Vec3f q = from_a.cross(edge1);
        const float v = ray.direction.dot(q) * inverse_determinant;
        if (v < 0.0f || u + v > 1.0f)
            return false;
        const float distance = edge2.dot(q) * inverse_determinant;
        if (distance < 1e-5f || distance >= hit.distance)
            return false;

        hit.distance = distance;
        hit.position = ray.origin + ray.direction * distance;
        hit.normal = edge1.cross(edge2).normalized();
        if (hit.normal.dot(ray.direction) > 0.0f)
            hit.normal = -hit.normal;
        hit.hit = true;
        return true;
    };

    auto intersect_scene = [&](const Ray& ray, Hit& hit) -> bool {
        bool found = false;
        for (const auto& tri : scene_triangles) {
            if (intersect_triangle(tri, ray, hit))
                found = true;
        }
        return found;
    };

    // Sample rays directly along the shared diagonal edge (x == y)
    constexpr int num_samples = 100;
    for (int i = 1; i < num_samples; ++i) {
        const float t = float(i) / float(num_samples);
        Ray ray;
        ray.origin = Vec3f{t, t, 2.0f};
        ray.direction = Vec3f{0.0f, 0.0f, -1.0f};

        Hit hit;
        const bool has_hit = intersect_scene(ray, hit);

        // Verify no holes occur on the shared diagonal edge
        REQUIRE(has_hit);
        REQUIRE(hit.position.x() == Catch::Approx(t).margin(1e-4f));
        REQUIRE(hit.position.y() == Catch::Approx(t).margin(1e-4f));
        REQUIRE(hit.position.z() == Catch::Approx(0.0f).margin(1e-4f));
        REQUIRE(hit.distance == Catch::Approx(2.0f).margin(1e-4f));

        // Verify consistent normal orientation without divergence (must point towards +Z)
        REQUIRE(hit.normal.z() == Catch::Approx(1.0f).margin(1e-4f));
        REQUIRE(std::abs(hit.normal.x()) < 1e-4f);
        REQUIRE(std::abs(hit.normal.y()) < 1e-4f);
    }
}
