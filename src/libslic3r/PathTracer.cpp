#include "PathTracer.hpp"
#include "Point.hpp"

#include <Eigen/Core>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <numeric>

#include <tbb/parallel_for.h>
#include <tbb/parallel_invoke.h>

namespace Slic3r {
namespace {

constexpr size_t leaf_triangle_count = 4;
constexpr int max_bvh_depth = 24;
constexpr size_t sah_bin_count = 12;

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

// Binned SAH BVH over the triangles, packed for the GPU path tracer.
class BvhBuilder
{
public:
    explicit BvhBuilder(const std::vector<PathTraceTriangle>& triangles, const std::atomic<bool>* cancel_token = nullptr)
        : m_triangles(triangles), m_cancel_token(cancel_token)
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

    PathTraceGpuScene pack_for_gpu() const
    {
        if (m_cancel_token != nullptr && m_cancel_token->load(std::memory_order_relaxed))
            return {};
        if (m_nodes.empty())
            return {};

        PathTraceGpuScene packed;
        packed.triangles.resize(m_indices.size());
        packed.shadings.resize(m_indices.size());
        packed.normals.resize(m_indices.size());
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
                    packed.normals[i] = {source.vertex_normals[0], source.vertex_normals[1], source.vertex_normals[2], source.smooth ? 1u : 0u};
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

    const std::vector<PathTraceTriangle>& m_triangles;
    std::vector<size_t> m_indices;
    std::vector<Bounds> m_prim_bounds;
    std::vector<Vec3f> m_prim_centroids;
    std::vector<BvhNode> m_nodes;
    std::atomic<uint32_t> m_node_count{0};
    const std::atomic<bool>* m_cancel_token{nullptr};
};

// Faces around a vertex bending by less than this from a corner's face are smoothed into its normal.
constexpr float SMOOTHING_CREASE_COS = 0.8660254f; // cos(30 deg)

} // namespace

uint32_t pack_unit_vector_oct(const Vec3f& n)
{
    const float l1 = std::abs(n.x()) + std::abs(n.y()) + std::abs(n.z());
    if (l1 <= 0.0f)
        return pack_unit_vector_oct(Vec3f::UnitZ());
    float x = n.x() / l1;
    float y = n.y() / l1;
    if (n.z() < 0.0f) {
        const float fx = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        const float fy = (1.0f - std::abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = fx;
        y = fy;
    }
    const auto snorm16 = [](float v) {
        return uint32_t(uint16_t(int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))));
    };
    return snorm16(x) | (snorm16(y) << 16);
}

Vec3f unpack_unit_vector_oct(uint32_t packed)
{
    const auto snorm = [](uint32_t bits) { return std::clamp(float(int16_t(uint16_t(bits))) / 32767.0f, -1.0f, 1.0f); };
    Vec3f n{snorm(packed & 0xFFFFu), snorm(packed >> 16), 0.0f};
    n.z() = 1.0f - std::abs(n.x()) - std::abs(n.y());
    const float t = std::max(-n.z(), 0.0f);
    n.x() += n.x() >= 0.0f ? -t : t;
    n.y() += n.y() >= 0.0f ? -t : t;
    return n.normalized();
}

void append_path_trace_triangles(std::vector<PathTraceTriangle>& triangles,
                                 const std::vector<Vec3f>& vertices,
                                 const std::vector<Vec3i32>& indices,
                                 const Transform3d& transform,
                                 const Vec4f& albedo,
                                 bool smooth_normals)
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
    if (!smooth_normals) {
        for (const Vec3i32& face : indices) {
            PathTraceTriangle triangle;
            triangle.a = transformed_vertices[size_t(face.x())];
            triangle.b = transformed_vertices[size_t(face.y())];
            triangle.c = transformed_vertices[size_t(face.z())];
            triangle.albedo = clamped_albedo;
            triangles.push_back(std::move(triangle));
        }
        return;
    }
    // Face normals scaled by twice the face area, which weighs the average, and the faces around each
    // vertex in compressed rows.
    std::vector<Vec3f> face_normals(indices.size());
    std::vector<uint32_t> row_start(transformed_vertices.size() + 1, 0);
    for (size_t f = 0; f < indices.size(); ++f) {
        const Vec3i32& face = indices[f];
        face_normals[f] = (transformed_vertices[size_t(face.y())] - transformed_vertices[size_t(face.x())])
                              .cross(transformed_vertices[size_t(face.z())] - transformed_vertices[size_t(face.x())]);
        for (int k = 0; k < 3; ++k)
            ++row_start[size_t(face[k]) + 1];
    }
    for (size_t v = 0; v < transformed_vertices.size(); ++v)
        row_start[v + 1] += row_start[v];
    std::vector<uint32_t> vertex_faces(row_start.back());
    std::vector<uint32_t> fill(row_start.begin(), row_start.end() - 1);
    for (size_t f = 0; f < indices.size(); ++f)
        for (int k = 0; k < 3; ++k)
            vertex_faces[fill[size_t(indices[f][k])]++] = uint32_t(f);

    for (size_t f = 0; f < indices.size(); ++f) {
        const Vec3i32& face = indices[f];
        PathTraceTriangle triangle;
        triangle.a = transformed_vertices[size_t(face.x())];
        triangle.b = transformed_vertices[size_t(face.y())];
        triangle.c = transformed_vertices[size_t(face.z())];
        triangle.albedo = clamped_albedo;
        const float face_len = face_normals[f].norm();
        const Vec3f face_unit = face_len > 0.0f ? Vec3f(face_normals[f] / face_len) : Vec3f::UnitZ();
        for (int k = 0; k < 3; ++k) {
            Vec3f sum = Vec3f::Zero();
            const size_t v = size_t(face[k]);
            for (uint32_t r = row_start[v]; r < row_start[v + 1]; ++r) {
                const Vec3f& other = face_normals[vertex_faces[r]];
                const float other_len = other.norm();
                if (other_len > 0.0f && face_unit.dot(other) >= SMOOTHING_CREASE_COS * other_len)
                    sum += other;
            }
            triangle.vertex_normals[size_t(k)] = pack_unit_vector_oct(sum.squaredNorm() > 0.0f ? Vec3f(sum.normalized()) : face_unit);
        }
        triangle.smooth = true;
        triangles.push_back(std::move(triangle));
    }
}

void append_path_trace_triangles(std::vector<PathTraceTriangle>& triangles,
                                 const std::vector<Vec3f>& vertices,
                                 const std::vector<Vec3i32>& indices,
                                 const Transform3d& transform,
                                 const Vec3f& albedo,
                                 bool smooth_normals)
{
    append_path_trace_triangles(triangles, vertices, indices, transform, Vec4f(albedo.x(), albedo.y(), albedo.z(), 1.0f), smooth_normals);
}

PathTraceGpuScene build_path_trace_gpu_scene(const std::vector<PathTraceTriangle>& triangles, const std::atomic<bool>* cancel_token)
{
    return BvhBuilder(triangles, cancel_token).pack_for_gpu();
}

} // namespace Slic3r