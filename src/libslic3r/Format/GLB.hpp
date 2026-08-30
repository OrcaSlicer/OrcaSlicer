#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r::GLB {

constexpr char MEDIA_TYPE[] = "model/gltf-binary";
constexpr uint16_t FORMAT_MAJOR = 2;
constexpr uint16_t FORMAT_MINOR = 0;
constexpr uint64_t DEFAULT_MAX_FILE_SIZE = uint64_t{UINT32_MAX};

struct Vertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> uv{};
};

struct Primitive {
    uint32_t first_index{0};
    uint32_t index_count{0};
    int32_t material{-1};
    nlohmann::json extras;
};

struct Polyline {
    std::vector<std::array<float, 3>> points;
    bool closed{false};
    nlohmann::json extras;
};

struct Material {
    std::string name;
    std::array<uint8_t, 4> base_color{{128, 128, 128, 255}};
    nlohmann::json extras;
};

struct Scene {
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Primitive> primitives;
    std::vector<Polyline> polylines;
    std::vector<Material> materials;
    std::array<float, 3> bounds_min{};
    std::array<float, 3> bounds_max{};
    nlohmann::json extras;
    // glTF is Y-up. Exporters with Z-up data may rotate the root node without rewriting vertices.
    std::array<double, 4> root_rotation{{0, 0, 0, 1}};
};

struct DocumentInfo {
    uint32_t version{0};
    uint32_t file_size{0};
    size_t vertex_count{0};
    size_t index_count{0};
    size_t primitive_count{0};
    size_t material_count{0};
    nlohmann::json scene_extras;
};

std::vector<uint8_t> serialize(const Scene& scene);
DocumentInfo validate(const uint8_t* data, size_t size, uint64_t max_file_size = DEFAULT_MAX_FILE_SIZE);
DocumentInfo validate(const std::vector<uint8_t>& data, uint64_t max_file_size = DEFAULT_MAX_FILE_SIZE);

} // namespace Slic3r::GLB

namespace Slic3r {
// Consistent entry point with store_stl(), store_obj(), and store_drc().
void store_glb(const char* path, const GLB::Scene& scene);
}
