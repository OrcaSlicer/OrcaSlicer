#pragma once

#include <boost/filesystem/path.hpp>
#include <libslic3r/Format/GLB.hpp>
#include <libslic3r/Point.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
struct GCodeProcessorResult;
namespace GUI { struct PreviewTriangleMesh; }

namespace PreviewGeometrySnapshot {

inline constexpr const char* MEDIA_TYPE = GLB::MEDIA_TYPE;
constexpr uint16_t FORMAT_MAJOR = GLB::FORMAT_MAJOR;
constexpr uint16_t FORMAT_MINOR = GLB::FORMAT_MINOR;
constexpr uint64_t DEFAULT_MAX_FILE_SIZE = GLB::DEFAULT_MAX_FILE_SIZE;
constexpr uint32_t FLAG_SPIRAL_VASE = 1u << 0;

struct Format {
    const char* media_type;
    const char* extension;
    uint16_t major_version;
    uint16_t minor_version;
};

struct Point { float x{0}; float y{0}; };
struct Vertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> uv{};
};
struct Group {
    uint32_t first_index{0};
    uint32_t index_count{0};
    uint16_t material_slot{0};
    uint8_t extrusion_role{0};
    uint8_t extruder_id{0};
    uint8_t colour_id{0};
    uint32_t layer_id{0};
};
struct MaterialSlot {
    uint8_t extruder_id{0};
    std::array<uint8_t, 4> rgba{{128, 128, 128, 255}};
    std::string preset_id;
    std::string display_name;
};
struct Snapshot {
    uint64_t scene_id{0};
    int32_t plate_index{-1};
    uint32_t flags{0};
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Group> groups;
    std::vector<MaterialSlot> materials;
    std::vector<Point> printable_area;
    std::vector<Point> bed_excluded_area;
    std::vector<Point> wrapping_excluded_area;
    float z_offset{0};
    float printable_height{0};
    std::array<float, 3> bounds_min{};
    std::array<float, 3> bounds_max{};
};

struct DocumentInfo {
    uint32_t version{0};
    uint32_t file_size{0};
    uint64_t scene_id{0};
    size_t vertex_count{0};
    size_t index_count{0};
    size_t primitive_count{0};
    size_t material_count{0};
};

Snapshot capture(const GUI::PreviewTriangleMesh& mesh, const GCodeProcessorResult& result,
                 int plate_index, uint64_t expected_scene_id, const Pointfs& fallback_printable_area = {});
std::vector<uint8_t> serialize(const Snapshot& snapshot);
DocumentInfo validate(const uint8_t* data, size_t size, uint64_t max_file_size = DEFAULT_MAX_FILE_SIZE);
DocumentInfo validate(const std::vector<uint8_t>& data, uint64_t max_file_size = DEFAULT_MAX_FILE_SIZE);
void write_atomic(const Snapshot& snapshot, const boost::filesystem::path& target);
const std::vector<Format>& supported_formats();
const Format* find_format(const std::string& media_type);
void write_atomic(const Snapshot& snapshot, const boost::filesystem::path& target, const std::string& media_type);

} // namespace PreviewGeometrySnapshot
} // namespace Slic3r
