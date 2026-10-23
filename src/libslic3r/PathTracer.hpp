#pragma once

#include "Point.hpp"

#include <Eigen/Core>
#include <array>
#include <cstdint>
#include <functional>
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
};

struct alignas(16) PathTraceGpuTriangle
{
    std::array<float, 4> a{}; // xyz: vertex A, w: tag_z
    std::array<float, 4> b{}; // xyz: vertex B, w: floatBits(raw_v_id)
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
    std::array<float, 16> normal_to_world{};
    std::array<float, 4> bounds_min{};
    std::array<float, 4> bounds_max{};
    std::array<uint32_t, 4> data{}; // x: root_node, y: node_count, z: tri_offset, w: tri_count
};
static_assert(sizeof(PathTraceGpuInstance) == 176, "PathTraceGpuInstance size must be 176 bytes");

struct PathTraceGpuScene
{
    std::vector<PathTraceGpuTriangle> triangles;
    std::vector<PathTraceGpuShading> shadings;
    std::vector<PathTraceGpuNode> nodes;
    std::vector<uint32_t> original_indices;
};

void append_path_trace_triangles(std::vector<PathTraceTriangle>& triangles,
                                 const std::vector<Vec3f>& vertices,
                                 const std::vector<Vec3i32>& indices,
                                 const Transform3d& transform,
                                 const Vec4f& albedo);

void append_path_trace_triangles(std::vector<PathTraceTriangle>& triangles,
                                 const std::vector<Vec3f>& vertices,
                                 const std::vector<Vec3i32>& indices,
                                 const Transform3d& transform,
                                 const Vec3f& albedo);

PathTraceGpuScene build_path_trace_gpu_scene(const std::vector<PathTraceTriangle>& triangles, const std::atomic<bool>* cancel_token = nullptr);

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

struct PathTraceSettings
{
    uint32_t width{960};
    uint32_t height{720};
    uint32_t samples_per_pixel{16};
    uint32_t sample_batch_size{4};
    uint32_t max_bounces{3};
    std::array<float, 4> clipping_plane{0.0f, 0.0f, 0.0f, 1.0f};
};

struct PathTraceImage
{
    uint32_t width{0};
    uint32_t height{0};
    uint32_t samples_per_pixel{0};
    std::vector<uint8_t> rgb;
    bool canceled{false};
};

PathTraceImage render_path_traced_image(const std::vector<PathTraceTriangle>& triangles,
                                        const PathTraceCamera& camera,
                                        const PathTraceSettings& settings,
                                        const std::function<bool()>& was_canceled = {},
                                        const std::function<void(float)>& report_progress = {},
                                        const std::function<void(const PathTraceImage&)>& report_image = {});

} // namespace Slic3r
