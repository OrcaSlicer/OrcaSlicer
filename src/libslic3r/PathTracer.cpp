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

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_invoke.h>

namespace Slic3r {
namespace {

// Primitives a leaf holds at most. A bead test costs several triangle tests, so beads take a leaf
// each: the deeper tree culls more of them than its extra boxes cost.
constexpr size_t leaf_triangle_count = 4;
constexpr size_t leaf_bead_count = 1;
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

// Binned SAH BVH over the bounds of primitives, packed for the GPU path tracer.
class BvhBuilder
{
public:
    // Builds over the bounds and centroids of the primitives, which it consumes, with at most leaf_size
    // primitives in a leaf.
    BvhBuilder(std::vector<Bounds> prim_bounds, std::vector<Vec3f> prim_centroids, size_t leaf_size, const std::atomic<bool>* cancel_token = nullptr)
        : m_prim_bounds(std::move(prim_bounds)), m_prim_centroids(std::move(prim_centroids)), m_leaf_size(leaf_size), m_cancel_token(cancel_token)
    {
        assert(m_prim_bounds.size() == m_prim_centroids.size());
        if (cancelled())
            return;
        const size_t n = m_prim_bounds.size();
        m_indices.resize(n);
        std::iota(m_indices.begin(), m_indices.end(), size_t{0});
        if (n > 0) {
            m_nodes.resize(2 * n + 64);
            m_node_count.store(0, std::memory_order_relaxed);
            build_node(0, n, 0);
            const size_t total_nodes = m_node_count.load(std::memory_order_relaxed);
            m_nodes.resize(total_nodes);
            m_nodes.shrink_to_fit();
        }
        m_prim_bounds.clear();
        m_prim_bounds.shrink_to_fit();
        m_prim_centroids.clear();
        m_prim_centroids.shrink_to_fit();
    }

    bool cancelled() const { return m_cancel_token != nullptr && m_cancel_token->load(std::memory_order_relaxed); }
    bool empty() const { return m_nodes.empty(); }
    // Primitive of each leaf slot, the order the packed primitives follow.
    const std::vector<size_t>& indices() const { return m_indices; }

    // Nodes flattened breadth first, so that the children of each inner node are contiguous.
    std::vector<PathTraceGpuNode> pack_nodes() const
    {
        std::vector<PathTraceGpuNode> packed(m_nodes.size());
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
            assert(dst_idx >= 0 && dst_idx < int(packed.size()));
            PathTraceGpuNode& dst = packed[dst_idx];

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

        packed.resize(next_child_idx > 1 ? next_child_idx : 1);
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
        if (count <= m_leaf_size || depth >= max_bvh_depth) {
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

    std::vector<size_t> m_indices;
    std::vector<Bounds> m_prim_bounds;
    std::vector<Vec3f> m_prim_centroids;
    std::vector<BvhNode> m_nodes;
    std::atomic<uint32_t> m_node_count{0};
    size_t m_leaf_size;
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
                                 const Vec4f& albedo,
                                 bool smooth_normals)
{
    if (indices.empty() || vertices.empty())
        return;
    const Vec4f clamped_albedo = albedo.cwiseMax(0.0f).cwiseMin(1.0f);

    const size_t needed = triangles.size() + indices.size();
    if (needed > triangles.capacity()) {
        triangles.reserve(std::max(needed, triangles.capacity() * 2));
    }
    if (!smooth_normals) {
        for (const Vec3i32& face : indices) {
            PathTraceTriangle triangle;
            triangle.a = vertices[size_t(face.x())];
            triangle.b = vertices[size_t(face.y())];
            triangle.c = vertices[size_t(face.z())];
            triangle.albedo = clamped_albedo;
            triangles.push_back(std::move(triangle));
        }
        return;
    }
    // Face normals scaled by twice the face area, which weighs the average, and the faces around each
    // vertex in compressed rows.
    std::vector<Vec3f> face_normals(indices.size());
    std::vector<uint32_t> row_start(vertices.size() + 1, 0);
    for (size_t f = 0; f < indices.size(); ++f) {
        const Vec3i32& face = indices[f];
        face_normals[f] = (vertices[size_t(face.y())] - vertices[size_t(face.x())])
                              .cross(vertices[size_t(face.z())] - vertices[size_t(face.x())]);
        for (int k = 0; k < 3; ++k)
            ++row_start[size_t(face[k]) + 1];
    }
    for (size_t v = 0; v < vertices.size(); ++v)
        row_start[v + 1] += row_start[v];
    std::vector<uint32_t> vertex_faces(row_start.back());
    std::vector<uint32_t> fill(row_start.begin(), row_start.end() - 1);
    for (size_t f = 0; f < indices.size(); ++f)
        for (int k = 0; k < 3; ++k)
            vertex_faces[fill[size_t(indices[f][k])]++] = uint32_t(f);

    for (size_t f = 0; f < indices.size(); ++f) {
        const Vec3i32& face = indices[f];
        PathTraceTriangle triangle;
        triangle.a = vertices[size_t(face.x())];
        triangle.b = vertices[size_t(face.y())];
        triangle.c = vertices[size_t(face.z())];
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
                                 const Vec3f& albedo,
                                 bool smooth_normals)
{
    append_path_trace_triangles(triangles, vertices, indices, Vec4f(albedo.x(), albedo.y(), albedo.z(), 1.0f), smooth_normals);
}

namespace {

float invalid_vertex_id_bits()
{
    float bits;
    const uint32_t invalid_v_id = UINT32_MAX;
    std::memcpy(&bits, &invalid_v_id, sizeof(float));
    return bits;
}

// Packs the primitives in the order of the BVH leaves; pack(source, slot) fills a slot's geometry.
template<typename Primitive, typename Pack>
PathTraceGpuScene pack_path_trace_scene(const BvhBuilder& bvh, const std::vector<Primitive>& primitives, Pack pack)
{
    if (bvh.cancelled() || bvh.empty())
        return {};
    const std::vector<size_t>& indices = bvh.indices();
    PathTraceGpuScene packed;
    packed.triangles.resize(indices.size());
    packed.shadings.resize(indices.size());
    packed.original_indices.resize(indices.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, indices.size()),
        [&](const tbb::blocked_range<size_t>& range) {
            for (size_t i = range.begin(); i < range.end(); ++i) {
                packed.original_indices[i] = uint32_t(indices[i]);
                const Primitive& source = primitives[indices[i]];
                PathTraceGpuTriangle& slot = packed.triangles[i];
                PathTraceGpuShading& shading = packed.shadings[i];
                for (Eigen::Index channel = 0; channel < 4; ++channel)
                    shading.albedo[size_t(channel)] = source.albedo[channel];
                shading.params = {0.20f, 0.0f, 0.0f, 0.0f};
                pack(source, slot);
                slot.a[3] = -1.0f; // default tag_z
                slot.b[3] = invalid_vertex_id_bits();
                slot.c[3] = std::clamp(source.albedo[3], 0.0f, 1.0f); // opacity
            }
        });
    packed.nodes = bvh.pack_nodes();
    return packed;
}

// Newton steps of a ray towards a bead's surface, BEAD_NEWTON_STEPS in pathtrace.comp.
constexpr int bead_newton_steps = 12;
// Distance from a bead's surface at which a ray has reached it, in mm, BEAD_TOLERANCE in pathtrace.comp.
constexpr float bead_tolerance = 1e-4f;

// Distance from the flat region of a bead minus its radius, and its gradient, in the bead's frame:
// bead_distance() in pathtrace.comp.
float bead_distance(const Vec3f& p, float length, float flat_half_width, float radius, Vec3f& grad)
{
    const Vec2f v{p.x() - std::clamp(p.x(), 0.0f, length), p.y()};
    const float dl = v.norm();
    const float s = std::max(dl - flat_half_width, 0.0f);
    const Vec2f h = dl > 0.0f ? Vec2f(v * (s / dl)) : Vec2f::Zero();
    const Vec3f delta{h.x(), h.y(), p.z()};
    const float dist = delta.norm();
    grad = dist > 0.0f ? Vec3f(delta / dist) : Vec3f::UnitZ();
    return dist - radius;
}

// Distance from o along d, at most t_max, at which a ray from outside reaches the surface of a bead,
// or -1: bead_newton() in pathtrace.comp. The distance from the flat region is convex along the ray,
// so Newton's method from outside converges monotonically and never steps through the surface.
float bead_newton(const Vec3f& o, const Vec3f& d, float t_max, float length, float flat_half_width, float radius)
{
    float t = 0.0f;
    for (int i = 0; i < bead_newton_steps; ++i) {
        Vec3f grad;
        const float f = bead_distance(o + t * d, length, flat_half_width, radius, grad);
        if (f <= bead_tolerance)
            return t;
        const float slope = grad.dot(d);
        if (slope >= 0.0f)
            return -1.0f;
        t -= f / slope;
        if (t > t_max)
            return -1.0f;
    }
    return -1.0f;
}

} // namespace

Eigen::Matrix3f path_trace_bead_frame(const PathTraceBead& bead)
{
    const Vec3f segment = bead.p1 - bead.p0;
    const float length = segment.norm();
    const Vec3f axis = length > 1e-7f ? Vec3f(segment / length) : Vec3f::UnitX();
    Vec3f right{axis.y(), -axis.x(), 0.0f};
    const float right_length = right.norm();
    right = right_length > 1e-4f ? Vec3f(right / right_length) : Vec3f::UnitX();
    Eigen::Matrix3f frame;
    frame << axis, right, right.cross(axis);
    return frame;
}

void path_trace_bead_bounds(const PathTraceBead& bead, Vec3f& min, Vec3f& max)
{
    // The disk of the flat region lies in the plane of the segment and its right; the round sides
    // reach the radius further in every direction.
    const Eigen::Matrix3f frame = path_trace_bead_frame(bead);
    const Vec3f disk = (frame.col(0).cwiseAbs2() + frame.col(1).cwiseAbs2()).cwiseSqrt() * bead.flat_half_width();
    const Vec3f extent = disk + Vec3f::Constant(bead.radius());
    min = bead.p0.cwiseMin(bead.p1) - extent;
    max = bead.p0.cwiseMax(bead.p1) + extent;
}

bool intersect_path_trace_bead(const PathTraceBead& bead, const Vec3f& origin, const Vec3f& direction, float& t_in, float& t_out)
{
    const Eigen::Matrix3f frame = path_trace_bead_frame(bead);
    const float length = (bead.p1 - bead.p0).norm();
    const float flat = bead.flat_half_width();
    const float radius = bead.radius();
    // The ray in the bead's frame, from p0, which keeps the precision of a bead far from the origin.
    const Vec3f o = frame.transpose() * (origin - bead.p0);
    const Vec3f d = frame.transpose() * direction;
    const float reach = flat + radius;
    const Vec3f lo{-reach, -reach, -radius};
    const Vec3f hi{length + reach, reach, radius};
    float t0 = std::numeric_limits<float>::lowest();
    float t1 = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(d[axis]) < 1e-12f) {
            if (o[axis] < lo[axis] || o[axis] > hi[axis])
                return false;
            continue;
        }
        const float a = (lo[axis] - o[axis]) / d[axis];
        const float b = (hi[axis] - o[axis]) / d[axis];
        t0 = std::max(t0, std::min(a, b));
        t1 = std::min(t1, std::max(a, b));
    }
    if (t1 < t0)
        return false;
    // The ray's line passes the stadium section of the bead's body on one side: a miss.
    const float side = std::abs(d.z() * o.y() - d.y() * o.z()) - std::abs(flat * d.z());
    if (side > 0.0f && side * side > radius * radius * (d.y() * d.y() + d.z() * d.z()))
        return false;
    const float entry = bead_newton(o + t0 * d, d, t1 - t0, length, flat, radius);
    if (entry < 0.0f)
        return false;
    t_in = t0 + entry;
    const float exit = bead_newton(o + t1 * d, -d, t1 - t0, length, flat, radius);
    t_out = exit >= 0.0f ? t1 - exit : t_in;
    return true;
}

Vec3f path_trace_bead_normal(const PathTraceBead& bead, const Vec3f& point)
{
    const Eigen::Matrix3f frame = path_trace_bead_frame(bead);
    Vec3f grad;
    bead_distance(frame.transpose() * (point - bead.p0), (bead.p1 - bead.p0).norm(), bead.flat_half_width(), bead.radius(), grad);
    return frame * grad;
}

PathTraceGpuScene build_path_trace_gpu_scene(const std::vector<PathTraceTriangle>& triangles, const std::atomic<bool>* cancel_token, bool with_normals)
{
    if (cancel_token != nullptr && cancel_token->load(std::memory_order_relaxed))
        return {};
    std::vector<Bounds> bounds(triangles.size());
    std::vector<Vec3f> centroids(triangles.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, triangles.size()),
        [&](const tbb::blocked_range<size_t>& range) {
            for (size_t i = range.begin(); i < range.end(); ++i) {
                const PathTraceTriangle& tri = triangles[i];
                bounds[i].include(tri.a);
                bounds[i].include(tri.b);
                bounds[i].include(tri.c);
                centroids[i] = (tri.a + tri.b + tri.c) * (1.0f / 3.0f);
            }
        });
    const BvhBuilder bvh(std::move(bounds), std::move(centroids), leaf_triangle_count, cancel_token);
    PathTraceGpuScene packed = pack_path_trace_scene(bvh, triangles, [](const PathTraceTriangle& source, PathTraceGpuTriangle& slot) {
        for (size_t axis = 0; axis < 3; ++axis) {
            slot.a[axis] = source.a[axis];
            slot.b[axis] = source.b[axis];
            slot.c[axis] = source.c[axis];
        }
    });
    if (!with_normals)
        return packed;
    packed.normals.resize(packed.triangles.size());
    for (size_t i = 0; i < packed.triangles.size(); ++i) {
        const PathTraceTriangle& source = triangles[packed.original_indices[i]];
        packed.normals[i] = {source.vertex_normals[0], source.vertex_normals[1], source.vertex_normals[2], source.smooth ? 1u : 0u};
    }
    return packed;
}

PathTraceGpuScene build_path_trace_gpu_scene(const std::vector<PathTraceBead>& beads, const std::atomic<bool>* cancel_token)
{
    if (cancel_token != nullptr && cancel_token->load(std::memory_order_relaxed))
        return {};
    std::vector<Bounds> bounds(beads.size());
    std::vector<Vec3f> centroids(beads.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, beads.size()),
        [&](const tbb::blocked_range<size_t>& range) {
            for (size_t i = range.begin(); i < range.end(); ++i) {
                path_trace_bead_bounds(beads[i], bounds[i].min, bounds[i].max);
                centroids[i] = 0.5f * (beads[i].p0 + beads[i].p1);
            }
        });
    const BvhBuilder bvh(std::move(bounds), std::move(centroids), leaf_bead_count, cancel_token);
    return pack_path_trace_scene(bvh, beads, [](const PathTraceBead& source, PathTraceGpuTriangle& slot) {
        for (size_t axis = 0; axis < 3; ++axis) {
            slot.a[axis] = source.p0[axis];
            slot.b[axis] = source.p1[axis];
        }
        slot.c[0] = source.flat_half_width();
        slot.c[1] = source.radius();
        slot.c[2] = 0.0f;
    });
}

} // namespace Slic3r
