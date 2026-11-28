#pragma once

#include "Point.hpp"

#include <Eigen/Core>
#include <algorithm>
#include <array>
#include <cstdint>
#include <atomic>
#include <vector>

namespace Slic3r {

struct PathTraceAlbedo
{
    Vec4f data{0.72f, 0.72f, 0.72f, 1.0f};

    PathTraceAlbedo() = default;
    PathTraceAlbedo(const Vec4f& v) : data(v) {}
    PathTraceAlbedo(const Vec3f& v) : data(v.x(), v.y(), v.z(), 1.0f) {}
    PathTraceAlbedo(float r, float g, float b, float a = 1.0f) : data(r, g, b, a) {}

    PathTraceAlbedo& operator=(const Vec3f& v) { data = Vec4f(v.x(), v.y(), v.z(), 1.0f); return *this; }
    PathTraceAlbedo& operator=(const Vec4f& v) { data = v; return *this; }

    float& operator[](Eigen::Index i) { return data[i]; }
    float operator[](Eigen::Index i) const { return data[i]; }
    float& x() { return data.x(); }
    float x() const { return data.x(); }
    float& y() { return data.y(); }
    float y() const { return data.y(); }
    float& z() { return data.z(); }
    float z() const { return data.z(); }
    float& w() { return data.w(); }
    float w() const { return data.w(); }

    operator Vec4f&() { return data; }
    operator const Vec4f&() const { return data; }
    operator Vec3f() const { return data.head<3>(); }
};

struct PathTraceTriangle
{
    Vec3f a{Vec3f::Zero()};
    Vec3f b{Vec3f::Zero()};
    Vec3f c{Vec3f::Zero()};
    PathTraceAlbedo albedo{};
    // Shading normals at a, b and c, encoded by pack_unit_vector_oct(), when smooth is set.
    std::array<uint32_t, 3> vertex_normals{};
    bool smooth{false};
};

// An extrusion of the G-code preview, traced as one analytic primitive. Its section is a stadium,
// width by height: a box of (width - height) by height between two half cylinders of radius
// height / 2. The section is swept from p0 to p1 and turned around the vertical axis at both ends,
// so that consecutive beads join like real extrusions, as the union of two rounded beads. In the
// bead's frame (x along the segment, y across it, z up) it is the set of points within the radius of
// the flat region a disk of radius flat_half_width() sweeps from p0 to p1: a convex body.
struct PathTraceBead
{
    Vec3f p0{Vec3f::Zero()};
    Vec3f p1{Vec3f::Zero()};
    float width{0.0f};
    float height{0.0f};
    PathTraceAlbedo albedo{};

    // Radius of the round sides; a bead narrower than high is round.
    float radius() const { return 0.5f * std::max(std::min(width, height), 0.0f); }
    // Half the width of the flat top and bottom.
    float flat_half_width() const { return 0.5f * std::max(width - height, 0.0f); }
};

// Frame of a bead: x along the segment, y to its right, z up, as the shader builds it.
Eigen::Matrix3f path_trace_bead_frame(const PathTraceBead& bead);

// Axis aligned bounds of a bead, tight for any slope of its segment.
void path_trace_bead_bounds(const PathTraceBead& bead, Vec3f& min, Vec3f& max);

// Distances at which a ray enters and leaves a bead, by the method the shader uses; false on a miss.
// direction must be a unit vector. Mirrors bead_interval() in pathtrace.comp, which the tests check
// through it.
bool intersect_path_trace_bead(const PathTraceBead& bead, const Vec3f& origin, const Vec3f& direction, float& t_in, float& t_out);

// Outward unit normal of a bead's surface at a point on it.
Vec3f path_trace_bead_normal(const PathTraceBead& bead, const Vec3f& point);

// Shading normals of a GPU triangle: xyz the encoded normals at its vertices, w 1 when they apply.
using PathTraceGpuNormals = std::array<uint32_t, 4>;
static_assert(sizeof(PathTraceGpuNormals) == 16, "PathTraceGpuNormals must be 16 bytes");

// Unit vector in 32 bits, octahedron mapped to two snorm16, the layout GLSL unpackSnorm2x16() reads.
uint32_t pack_unit_vector_oct(const Vec3f& n);
Vec3f unpack_unit_vector_oct(uint32_t packed);

// A triangle, or a bead in the BVH of a bead instance: a.xyz its p0, b.xyz its p1, c.x the half width
// of its flat top and c.y its radius.
struct alignas(16) PathTraceGpuTriangle
{
    std::array<float, 4> a{}; // xyz: vertex A, w: tag_z
    std::array<float, 4> b{}; // xyz: vertex B, w: the bits of the vertex ID, which the shader reads as a uint
    std::array<float, 4> c{}; // xyz: vertex C, w: opacity (alpha)
};
static_assert(sizeof(PathTraceGpuTriangle) == 48, "PathTraceGpuTriangle must be 48 bytes");

struct alignas(16) PathTraceGpuShading
{
    std::array<float, 4> albedo{0.72f, 0.72f, 0.72f, 1.0f};
    std::array<float, 4> params{0.20f, 0.0f, 0.0f, 0.0f}; // x: layer_height, y: role, z: flags, w: reserved
};
static_assert(sizeof(PathTraceGpuShading) == 32, "PathTraceGpuShading must be 32 bytes");

struct alignas(16) PathTraceGpuNode
{
    std::array<float, 3> bounds_min{};
    int32_t left_or_first{-1};
    std::array<float, 3> bounds_max{};
    uint32_t count_and_axis{0};

    uint32_t count() const { return count_and_axis & 0x3FFFFFFFu; }
    uint32_t split_axis() const { return count_and_axis >> 30; }
    bool is_leaf() const { return count() > 0; }
};
static_assert(sizeof(PathTraceGpuNode) == 32, "PathTraceGpuNode size must be 32 bytes");

struct alignas(16) PathTraceGpuInstance
{
    std::array<float, 16> world_to_local{};
    std::array<float, 16> local_to_world{}; // the upper left 3x3; [12]: the flags of the object
    std::array<float, 4> bounds_min{};
    std::array<float, 4> bounds_max{};
    std::array<uint32_t, 4> data{}; // x: root_node, y: node_count, z: tri_offset, w: tri_count
};
static_assert(sizeof(PathTraceGpuInstance) == 176, "PathTraceGpuInstance size must be 176 bytes");

struct PathTraceGpuScene
{
    std::vector<PathTraceGpuTriangle> triangles;
    std::vector<PathTraceGpuShading> shadings;
    std::vector<PathTraceGpuNormals> normals;
    std::vector<PathTraceGpuNode> nodes;
    std::vector<uint32_t> original_indices;
};

// Appends a mesh. Its triangles are smooth: the shading normal at a corner averages the
// faces around the vertex that bend from the corner's face by less than the crease angle, so that
// curved surfaces shade round and the edges of a part stay sharp. Without smooth_normals every
// triangle shades flat.
void append_path_trace_triangles(std::vector<PathTraceTriangle>& triangles,
                                 const std::vector<Vec3f>& vertices,
                                 const std::vector<Vec3i32>& indices,
                                 const Vec4f& albedo,
                                 bool smooth_normals = true);

void append_path_trace_triangles(std::vector<PathTraceTriangle>& triangles,
                                 const std::vector<Vec3f>& vertices,
                                 const std::vector<Vec3i32>& indices,
                                 const Vec3f& albedo,
                                 bool smooth_normals = true);

// Without with_normals the scene has no shading normals, which saves their memory when nothing reads them.
PathTraceGpuScene build_path_trace_gpu_scene(const std::vector<PathTraceTriangle>& triangles, const std::atomic<bool>* cancel_token = nullptr, bool with_normals = true);
// The BVH of beads, packed as PathTraceGpuTriangle; normals stay empty.
PathTraceGpuScene build_path_trace_gpu_scene(const std::vector<PathTraceBead>& beads, const std::atomic<bool>* cancel_token = nullptr);

// Camera of a path traced frame.
struct PathTraceCamera
{
    Vec3f position{Vec3f::Zero()};
    Vec3f right{Vec3f::UnitX()};
    Vec3f up{Vec3f::UnitY()};
    Vec3f forward{-Vec3f::UnitZ()};
    bool orthographic{false};
    float orthographic_height{0.0f};
    float vertical_fov_degrees{45.0f};
};

} // namespace Slic3r
