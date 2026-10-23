#include "PathTracer.hpp"
#include "Point.hpp"

#include <Eigen/Core>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

#include <tbb/parallel_for.h>
#include <tbb/parallel_invoke.h>

namespace Slic3r {
namespace {

constexpr float ray_epsilon = 1e-4f;
constexpr float pi = 3.14159265358979323846f;
constexpr size_t leaf_triangle_count = 4;
constexpr int max_bvh_depth = 24;
constexpr size_t sah_bin_count = 12;

struct Ray
{
    Vec3f origin;
    Vec3f direction;
};

struct Bounds
{
    Vec3f min{Vec3f::Constant(std::numeric_limits<float>::max())};
    Vec3f max{Vec3f::Constant(std::numeric_limits<float>::lowest())};

    void include(const Vec3f& point)
    {
        min = min.cwiseMin(point);
        max = max.cwiseMax(point);
    }

    void include(const Bounds& other)
    {
        min = min.cwiseMin(other.min);
        max = max.cwiseMax(other.max);
    }

    float half_area() const
    {
        const Vec3f d = (max - min).cwiseMax(0.0f);
        return d.x() * d.y() + d.y() * d.z() + d.z() * d.x();
    }
};

struct BvhNode
{
    Bounds bounds;
    size_t first{0};
    size_t count{0};
    int left{-1};
    int right{-1};
    int split_axis{0};
};

struct Hit
{
    float distance{std::numeric_limits<float>::max()};
    Vec3f position{Vec3f::Zero()};
    Vec3f normal{Vec3f::Zero()};
    Vec4f albedo{Vec4f::Zero()};
};

class Random
{
public:
    explicit Random(uint32_t seed) : m_state(seed ? seed : 1u) {}

    float next()
    {
        m_state ^= m_state << 13;
        m_state ^= m_state >> 17;
        m_state ^= m_state << 5;
        return float(m_state >> 8) * (1.0f / 16777216.0f);
    }

private:
    uint32_t m_state;
};

class Scene
{
public:
    explicit Scene(const std::vector<PathTraceTriangle>& triangles,
                   const std::array<float, 4>& clipping_plane = {0.0f, 0.0f, 0.0f, 1.0f},
                   const std::atomic<bool>* cancel_token = nullptr)
        : m_triangles(triangles), m_clipping_plane(clipping_plane), m_cancel_token(cancel_token)
    {
        if (m_cancel_token != nullptr && m_cancel_token->load(std::memory_order_relaxed))
            return;
        const size_t n = triangles.size();
        m_indices.resize(n);
        std::iota(m_indices.begin(), m_indices.end(), size_t{0});
        if (n > 0) {
            m_prim_bounds.resize(n);
            m_prim_centroids.resize(n);
            tbb::parallel_for(tbb::blocked_range<size_t>(0, n),
                [&](const tbb::blocked_range<size_t>& range) {
                    for (size_t i = range.begin(); i < range.end(); ++i) {
                        const PathTraceTriangle& tri = triangles[i];
                        Bounds b;
                        b.include(tri.a);
                        b.include(tri.b);
                        b.include(tri.c);
                        m_prim_bounds[i] = b;
                        m_prim_centroids[i] = (tri.a + tri.b + tri.c) * (1.0f / 3.0f);
                    }
                });
            m_nodes.resize(2 * n + 64);
            m_node_count.store(0, std::memory_order_relaxed);
            build_node(0, n, 0);
            const size_t total_nodes = m_node_count.load(std::memory_order_relaxed);
            m_nodes.resize(total_nodes);
            m_nodes.shrink_to_fit();
            m_prim_bounds.clear();
            m_prim_bounds.shrink_to_fit();
            m_prim_centroids.clear();
            m_prim_centroids.shrink_to_fit();
        }
    }

    bool intersect(const Ray& ray, Hit& hit) const
    {
        if (m_nodes.empty())
            return false;

        bool found = false;
        std::array<int, 128> stack{};
        size_t stack_size = 1;
        stack[0] = 0;
        while (stack_size > 0) {
            const BvhNode& node = m_nodes[stack[--stack_size]];
            if (!intersects_bounds(node.bounds, ray, hit.distance))
                continue;
            if (node.count > 0) {
                for (size_t i = node.first; i < node.first + node.count; ++i)
                    if (intersect_triangle(m_triangles[m_indices[i]], ray, hit))
                        found = true;
            } else {
                if (stack_size + 2 > stack.size())
                    return found;
                stack[stack_size++] = node.left;
                stack[stack_size++] = node.right;
            }
        }
        return found;
    }

    PathTraceGpuScene pack_for_gpu() const
    {
        if (m_cancel_token != nullptr && m_cancel_token->load(std::memory_order_relaxed))
            return {};
        if (m_nodes.empty())
            return {};

        PathTraceGpuScene packed;
        packed.triangles.resize(m_indices.size());
        packed.shadings.resize(m_indices.size());
        packed.nodes.resize(m_nodes.size());
        packed.original_indices.resize(m_indices.size());

        tbb::parallel_for(tbb::blocked_range<size_t>(0, m_indices.size()),
            [&](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i < range.end(); ++i) {
                    packed.original_indices[i] = uint32_t(m_indices[i]);
                    const PathTraceTriangle& source = m_triangles[m_indices[i]];
                    PathTraceGpuTriangle& triangle = packed.triangles[i];
                    PathTraceGpuShading& shading = packed.shadings[i];
                    for (size_t axis = 0; axis < 3; ++axis) {
                        triangle.a[axis] = source.a[axis];
                        triangle.b[axis] = source.b[axis];
                        triangle.c[axis] = source.c[axis];
                        shading.albedo[axis] = source.albedo[axis];
                    }
                    triangle.a[3] = -1.0f; // default tag_z
                    float invalid_v_id_f;
                    const uint32_t invalid_v_id = UINT32_MAX;
                    std::memcpy(&invalid_v_id_f, &invalid_v_id, sizeof(float));
                    triangle.b[3] = invalid_v_id_f;
                    triangle.c[3] = std::clamp(source.albedo[3], 0.0f, 1.0f); // opacity
                    shading.albedo[3] = source.albedo[3];
                    shading.params = {0.20f, 0.0f, 0.0f, 0.0f};
                }
            });

        if (m_nodes.empty())
            return packed;

        packed.nodes.resize(m_nodes.size());

        // Breadth-First flattening so that each inner node's children are placed contiguously: [child, child + 1]
        std::vector<int> new_indices(m_nodes.size(), -1);
        std::vector<int> queue;
        queue.reserve(m_nodes.size());

        new_indices[0] = 0;
        queue.push_back(0);
        size_t next_child_idx = 1;
        size_t head = 0;

        while (head < queue.size()) {
            const int u = queue[head++];
            const BvhNode& source = m_nodes[u];
            const int dst_idx = new_indices[u];
            assert(dst_idx >= 0 && dst_idx < int(packed.nodes.size()));
            PathTraceGpuNode& dst = packed.nodes[dst_idx];

            for (size_t axis = 0; axis < 3; ++axis) {
                dst.bounds_min[axis] = source.bounds.min[axis];
                dst.bounds_max[axis] = source.bounds.max[axis];
            }

            if (source.count > 0) {
                // Leaf node
                dst.left_or_first = int32_t(source.first);
                dst.count_and_axis = uint32_t(source.count) & 0x3FFFFFFFu;
            } else {
                // Inner node: allocate contiguous child pair [next_child_idx, next_child_idx + 1]
                const int left_child_dst = int(next_child_idx);
                const int right_child_dst = int(next_child_idx + 1);
                next_child_idx += 2;

                dst.left_or_first = left_child_dst;
                const uint32_t axis = uint32_t(std::clamp(source.split_axis, 0, 2));
                dst.count_and_axis = (axis << 30); // count is 0 for inner nodes

                if (source.left >= 0 && size_t(source.left) < m_nodes.size()) {
                    new_indices[source.left] = left_child_dst;
                    queue.push_back(source.left);
                }
                if (source.right >= 0 && size_t(source.right) < m_nodes.size()) {
                    new_indices[source.right] = right_child_dst;
                    queue.push_back(source.right);
                }
            }
        }

        packed.nodes.resize(next_child_idx > 1 ? next_child_idx : 1);
        return packed;
    }

private:
    int build_node(size_t first, size_t count, int depth)
    {
        if ((depth & 3) == 0 && m_cancel_token != nullptr && m_cancel_token->load(std::memory_order_relaxed))
            return -1;

        const uint32_t node_index = m_node_count.fetch_add(1, std::memory_order_relaxed);
        if (node_index >= m_nodes.size())
            return -1;

        Bounds bounds;
        Bounds centroids;
        for (size_t i = first; i < first + count; ++i) {
            const size_t idx = m_indices[i];
            bounds.include(m_prim_bounds[idx]);
            centroids.include(m_prim_centroids[idx]);
        }
        m_nodes[node_index].bounds = bounds;
        if (count <= leaf_triangle_count || depth >= max_bvh_depth) {
            m_nodes[node_index].first = first;
            m_nodes[node_index].count = count;
            m_nodes[node_index].left = -1;
            m_nodes[node_index].right = -1;
            return int(node_index);
        }

        const Vec3f extent = centroids.max - centroids.min;
        int best_axis = -1;
        size_t best_split_bin = 0;
        float best_cost = std::numeric_limits<float>::max();

        if (count >= 12) {
            struct Bin
            {
                Bounds bounds;
                size_t count{0};
            };
            std::array<std::array<Bin, sah_bin_count>, 3> axis_bins{};
            std::array<float, 3> scale{0.0f, 0.0f, 0.0f};
            std::array<float, 3> min_c{0.0f, 0.0f, 0.0f};
            int valid_axes[3];
            int num_valid = 0;

            for (int a = 0; a < 3; ++a) {
                if (extent[a] > 1e-5f) {
                    scale[a] = float(sah_bin_count) / extent[a];
                    min_c[a] = centroids.min[a];
                    valid_axes[num_valid++] = a;
                }
            }

            for (size_t i = first; i < first + count; ++i) {
                const size_t idx = m_indices[i];
                const Vec3f& c = m_prim_centroids[idx];
                const Bounds& b = m_prim_bounds[idx];
                for (int k = 0; k < num_valid; ++k) {
                    const int a = valid_axes[k];
                    const int bin_idx = std::clamp(int((c[a] - min_c[a]) * scale[a]), 0, int(sah_bin_count) - 1);
                    axis_bins[a][size_t(bin_idx)].count++;
                    axis_bins[a][size_t(bin_idx)].bounds.include(b);
                }
            }

            for (int k = 0; k < num_valid; ++k) {
                const int axis = valid_axes[k];
                const auto& bins = axis_bins[axis];

                std::array<float, sah_bin_count - 1> left_area{};
                std::array<size_t, sah_bin_count - 1> left_count{};
                Bounds cur_left;
                size_t cur_left_cnt = 0;
                for (size_t b = 0; b + 1 < sah_bin_count; ++b) {
                    if (bins[b].count > 0) {
                        cur_left.include(bins[b].bounds);
                        cur_left_cnt += bins[b].count;
                    }
                    left_area[b] = cur_left_cnt > 0 ? cur_left.half_area() : 0.0f;
                    left_count[b] = cur_left_cnt;
                }

                Bounds cur_right;
                size_t cur_right_cnt = 0;
                for (size_t s = sah_bin_count - 1; s >= 1; --s) {
                    if (bins[s].count > 0) {
                        cur_right.include(bins[s].bounds);
                        cur_right_cnt += bins[s].count;
                    }
                    if (left_count[s - 1] == 0 || cur_right_cnt == 0)
                        continue;
                    const float cost = left_area[s - 1] * float(left_count[s - 1]) + cur_right.half_area() * float(cur_right_cnt);
                    if (cost < best_cost) {
                        best_cost = cost;
                        best_axis = axis;
                        best_split_bin = s;
                    }
                }
            }
        }

        size_t middle = first + count / 2;
        if (best_axis >= 0) {
            const float scale = float(sah_bin_count) / extent[best_axis];
            const float min_c = centroids.min[best_axis];
            auto it = std::partition(m_indices.begin() + first, m_indices.begin() + first + count,
                [&](size_t idx) {
                    const int b = std::clamp(int((m_prim_centroids[idx][best_axis] - min_c) * scale), 0, int(sah_bin_count) - 1);
                    return size_t(b) < best_split_bin;
                });
            const size_t split_idx = size_t(it - m_indices.begin());
            if (split_idx > first && split_idx < first + count) {
                middle = split_idx;
            } else {
                best_axis = -1;
            }
        }

        if (best_axis < 0) {
            Eigen::Index axis = 0;
            extent.maxCoeff(&axis);
            best_axis = int(axis);
            std::nth_element(m_indices.begin() + first, m_indices.begin() + middle, m_indices.begin() + first + count,
                [&](size_t lhs, size_t rhs) {
                    return m_prim_centroids[lhs][axis] < m_prim_centroids[rhs][axis];
                });
        }

        int left = -1;
        int right = -1;
        if (count > 2048) {
            tbb::parallel_invoke(
                [&] { left = build_node(first, middle - first, depth + 1); },
                [&] { right = build_node(middle, first + count - middle, depth + 1); }
            );
        } else {
            left = build_node(first, middle - first, depth + 1);
            right = build_node(middle, first + count - middle, depth + 1);
        }

        m_nodes[node_index].left = left;
        m_nodes[node_index].right = right;
        m_nodes[node_index].split_axis = best_axis;
        m_nodes[node_index].first = 0;
        m_nodes[node_index].count = 0;
        return int(node_index);
    }

    bool intersects_bounds(const Bounds& bounds, const Ray& ray, float max_distance) const
    {
        const Vec3f plane_n{m_clipping_plane[0], m_clipping_plane[1], m_clipping_plane[2]};
        const bool clipping_active = plane_n.squaredNorm() > 0.5f;
        if (clipping_active && bounds.min.z() >= -0.01f) {
            const Vec3f p_max{
                plane_n.x() >= 0.0f ? bounds.max.x() : bounds.min.x(),
                plane_n.y() >= 0.0f ? bounds.max.y() : bounds.min.y(),
                plane_n.z() >= 0.0f ? bounds.max.z() : bounds.min.z()
            };
            if (plane_n.dot(p_max) + m_clipping_plane[3] < 0.0f)
                return false;
        }
        float near_distance = ray_epsilon;
        float far_distance = max_distance;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(ray.direction[axis]) < 1e-8f) {
                if (ray.origin[axis] < bounds.min[axis] || ray.origin[axis] > bounds.max[axis])
                    return false;
                continue;
            }
            const float inverse_direction = 1.0f / ray.direction[axis];
            float first = (bounds.min[axis] - ray.origin[axis]) * inverse_direction;
            float second = (bounds.max[axis] - ray.origin[axis]) * inverse_direction;
            if (first > second)
                std::swap(first, second);
            near_distance = std::max(near_distance, first);
            far_distance = std::min(far_distance, second);
            if (far_distance < near_distance)
                return false;
        }
        if (clipping_active && bounds.min.z() >= -0.01f) {
            const float denom = plane_n.dot(ray.direction);
            const float dist = plane_n.dot(ray.origin) + m_clipping_plane[3];
            if (denom > 1e-5f)
                near_distance = std::max(near_distance, -dist / denom);
            else if (denom < -1e-5f)
                far_distance = std::min(far_distance, -dist / denom);
            else if (dist < 0.0f)
                return false;
            if (far_distance < near_distance)
                return false;
        }
        return true;
    }

    bool intersect_triangle(const PathTraceTriangle& triangle, const Ray& ray, Hit& hit) const
    {
        const Vec3f plane_n{m_clipping_plane[0], m_clipping_plane[1], m_clipping_plane[2]};
        if (triangle.albedo.x() >= -1.0f && plane_n.squaredNorm() > 0.5f) {
            const float da = plane_n.dot(triangle.a) + m_clipping_plane[3];
            const float db = plane_n.dot(triangle.b) + m_clipping_plane[3];
            const float dc = plane_n.dot(triangle.c) + m_clipping_plane[3];
            if (da < 0.0f && db < 0.0f && dc < 0.0f)
                return false;
        }
        const Vec3f edge1 = triangle.b - triangle.a;
        const Vec3f edge2 = triangle.c - triangle.a;
        const Vec3f cross = ray.direction.cross(edge2);
        const float determinant = edge1.dot(cross);
        if (std::abs(determinant) < 1e-8f)
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
        if (distance < ray_epsilon || distance >= hit.distance)
            return false;

        const Vec3f hit_pos = ray.origin + ray.direction * distance;
        if (triangle.albedo.x() >= -1.0f && (plane_n.dot(hit_pos) + m_clipping_plane[3] < 0.0f))
            return false;

        hit.distance = distance;
        hit.position = hit_pos;
        hit.normal = edge1.cross(edge2).normalized();
        if (hit.normal.dot(ray.direction) > 0.0f)
            hit.normal = -hit.normal;
        const Vec4f alb = triangle.albedo;
        hit.albedo = alb.cwiseMax(0.0f).cwiseMin(1.0f);
        return true;
    }

    const std::vector<PathTraceTriangle>& m_triangles;
    std::array<float, 4> m_clipping_plane{0.0f, 0.0f, 0.0f, 1.0f};
    std::vector<size_t> m_indices;
    std::vector<Bounds> m_prim_bounds;
    std::vector<Vec3f> m_prim_centroids;
    std::vector<BvhNode> m_nodes;
    std::atomic<uint32_t> m_node_count{0};
    const std::atomic<bool>* m_cancel_token{nullptr};
};

Vec3f sky(const Vec3f& direction)
{
    const float amount = std::clamp(direction.z() * 0.5f + 0.5f, 0.0f, 1.0f);
    const Vec3f horizon{0.82f, 0.84f, 0.87f};
    const Vec3f zenith{0.43f, 0.57f, 0.78f};
    return horizon * (1.0f - amount) + zenith * amount;
}

Vec3f cosine_hemisphere(const Vec3f& normal, Random& random)
{
    const float angle = 2.0f * pi * random.next();
    const float radius = std::sqrt(random.next());
    const float x = radius * std::cos(angle);
    const float y = radius * std::sin(angle);
    const float z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
    const Vec3f helper = std::abs(normal.z()) < 0.999f ? Vec3f::UnitZ() : Vec3f::UnitY();
    const Vec3f tangent = helper.cross(normal).normalized();
    const Vec3f bitangent = normal.cross(tangent);
    return (tangent * x + bitangent * y + normal * z).normalized();
}

Vec3f trace(const Scene& scene, Ray ray, uint32_t max_bounces, Random& random)
{
    const Vec3f light_direction = Vec3f{-0.45f, -0.55f, 0.70f}.normalized();
    const Vec3f light_color{1.0f, 0.92f, 0.78f};
    Vec3f radiance = Vec3f::Zero();
    Vec3f throughput = Vec3f::Ones();

    for (uint32_t bounce = 0; bounce < max_bounces; ++bounce) {
        Hit hit;
        if (!scene.intersect(ray, hit)) {
            radiance += throughput.cwiseProduct(sky(ray.direction));
            break;
        }

        Ray shadow_ray{hit.position + hit.normal * ray_epsilon * 2.0f, light_direction};
        Hit shadow_hit;
        if (!scene.intersect(shadow_ray, shadow_hit)) {
            const float direct = std::max(0.0f, hit.normal.dot(light_direction));
            radiance += throughput.cwiseProduct(hit.albedo.head<3>()).cwiseProduct(light_color) * (direct * 0.75f);
        }

        throughput = throughput.cwiseProduct(hit.albedo.head<3>());
        ray.origin = hit.position + hit.normal * ray_epsilon * 2.0f;
        ray.direction = cosine_hemisphere(hit.normal, random);
    }
    return radiance;
}

uint8_t to_srgb(float value)
{
    value = std::max(0.0f, value);
    const float mapped = value / (1.0f + value);
    const float srgb = mapped <= 0.0031308f ? mapped * 12.92f : 1.055f * std::pow(mapped, 1.0f / 2.4f) - 0.055f;
    return uint8_t(std::lround(std::clamp(srgb, 0.0f, 1.0f) * 255.0f));
}

} // namespace

void append_path_trace_triangles(std::vector<PathTraceTriangle>& triangles,
                                 const std::vector<Vec3f>& vertices,
                                 const std::vector<Vec3i32>& indices,
                                 const Transform3d& transform,
                                 const Vec4f& albedo)
{
    if (indices.empty() || vertices.empty())
        return;
    const Vec4f clamped_albedo = albedo.cwiseMax(0.0f).cwiseMin(1.0f);
    std::vector<Vec3f> transformed_vertices(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i)
        transformed_vertices[i] = (transform * vertices[i].cast<double>()).cast<float>();

    const size_t needed = triangles.size() + indices.size();
    if (needed > triangles.capacity()) {
        triangles.reserve(std::max(needed, triangles.capacity() * 2));
    }
    for (const Vec3i32& face : indices) {
        PathTraceTriangle triangle;
        triangle.a = transformed_vertices[size_t(face.x())];
        triangle.b = transformed_vertices[size_t(face.y())];
        triangle.c = transformed_vertices[size_t(face.z())];
        triangle.albedo = clamped_albedo;
        triangles.push_back(std::move(triangle));
    }
}

void append_path_trace_triangles(std::vector<PathTraceTriangle>& triangles,
                                 const std::vector<Vec3f>& vertices,
                                 const std::vector<Vec3i32>& indices,
                                 const Transform3d& transform,
                                 const Vec3f& albedo)
{
    append_path_trace_triangles(triangles, vertices, indices, transform, Vec4f(albedo.x(), albedo.y(), albedo.z(), 1.0f));
}

PathTraceGpuScene build_path_trace_gpu_scene(const std::vector<PathTraceTriangle>& triangles, const std::atomic<bool>* cancel_token)
{
    return Scene(triangles, {0.0f, 0.0f, 0.0f, 1.0f}, cancel_token).pack_for_gpu();
}

PathTraceImage render_path_traced_image(const std::vector<PathTraceTriangle>& triangles,
                                        const PathTraceCamera& camera,
                                        const PathTraceSettings& settings,
                                        const std::function<bool()>& was_canceled,
                                        const std::function<void(float)>& report_progress,
                                        const std::function<void(const PathTraceImage&)>& report_image)
{
    if (settings.width == 0 || settings.height == 0 || settings.samples_per_pixel == 0 ||
        settings.sample_batch_size == 0 || settings.max_bounces == 0)
        throw std::invalid_argument("Path tracing settings must be non-zero.");
    if (camera.right.squaredNorm() == 0.0f || camera.up.squaredNorm() == 0.0f || camera.forward.squaredNorm() == 0.0f ||
        (camera.orthographic ? camera.orthographic_height <= 0.0f :
            (camera.vertical_fov_degrees <= 0.0f || camera.vertical_fov_degrees >= 179.0f)))
        throw std::invalid_argument("Path tracing camera is invalid.");

    PathTraceImage image;
    image.width = settings.width;
    image.height = settings.height;
    const size_t pixel_count = size_t(settings.width) * settings.height;
    image.rgb.resize(pixel_count * 3);
    std::vector<Vec3f> accumulated_radiance(pixel_count, Vec3f::Zero());
    const Scene scene(triangles, settings.clipping_plane);
    const Vec3f right = camera.right.normalized();
    const Vec3f up = camera.up.normalized();
    const Vec3f forward = camera.forward.normalized();
    const float aspect = float(settings.width) / float(settings.height);
    const float half_fov = camera.orthographic ? 0.0f : std::tan(camera.vertical_fov_degrees * pi / 360.0f);
    const float half_ortho_height = camera.orthographic ? camera.orthographic_height * 0.5f : 0.0f;

    for (uint32_t sample_begin = 0; sample_begin < settings.samples_per_pixel; sample_begin += settings.sample_batch_size) {
        if (was_canceled && was_canceled()) {
            image.canceled = true;
            image.rgb.clear();
            return image;
        }
        const uint32_t sample_end = std::min(settings.samples_per_pixel, sample_begin + settings.sample_batch_size);
        std::atomic<bool> canceled{false};
        tbb::parallel_for(tbb::blocked_range<uint32_t>(0, settings.height), [&](const tbb::blocked_range<uint32_t>& range) {
            if ((was_canceled && was_canceled()) || canceled.load(std::memory_order_relaxed))
                return;
            for (uint32_t y = range.begin(); y != range.end(); ++y) {
                if (was_canceled && was_canceled()) {
                    canceled.store(true, std::memory_order_relaxed);
                    return;
                }
                for (uint32_t x = 0; x < settings.width; ++x) {
                    const size_t pixel = size_t(y) * settings.width + x;
                    const uint32_t pixel_seed = (y * settings.width + x + 1u) * 0x9e3779b9u;
                    for (uint32_t sample = sample_begin; sample < sample_end; ++sample) {
                        Random random(pixel_seed ^ (sample * 0x85ebca6bu));
                        const float normalized_x = 2.0f * (float(x) + random.next()) / float(settings.width) - 1.0f;
                        const float normalized_y = 1.0f - 2.0f * (float(y) + random.next()) / float(settings.height);
                        Ray ray;
                        if (camera.orthographic) {
                            ray.origin = camera.position + right * (normalized_x * aspect * half_ortho_height) + up * (normalized_y * half_ortho_height);
                            ray.direction = forward;
                        } else {
                            const float screen_x = normalized_x * aspect * half_fov;
                            const float screen_y = normalized_y * half_fov;
                            ray.origin = camera.position;
                            ray.direction = (forward + right * screen_x + up * screen_y).normalized();
                        }
                        accumulated_radiance[pixel] += trace(scene, ray, settings.max_bounces, random);
                    }
                }
            }
        });
        if (canceled.load(std::memory_order_relaxed) || (was_canceled && was_canceled())) {
            image.canceled = true;
            image.rgb.clear();
            return image;
        }
        if (report_progress)
            report_progress(float(sample_end) / float(settings.samples_per_pixel));

        image.samples_per_pixel = sample_end;
        for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
            const Vec3f color = accumulated_radiance[pixel] / float(sample_end);
            const size_t offset = pixel * 3;
            image.rgb[offset] = to_srgb(color.x());
            image.rgb[offset + 1] = to_srgb(color.y());
            image.rgb[offset + 2] = to_srgb(color.z());
        }
        if (report_image)
            report_image(image);
    }
    return image;
}

} // namespace Slic3r