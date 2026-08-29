#pragma once

#include <boost/filesystem/path.hpp>
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

constexpr uint16_t FORMAT_MAJOR = 1;
constexpr uint16_t FORMAT_MINOR = 0;
constexpr uint32_t HEADER_BYTE_SIZE = 256;
constexpr uint32_t VERTEX_RECORD_SIZE = 32;
constexpr uint32_t GROUP_RECORD_SIZE = 24;
constexpr uint32_t MATERIAL_RECORD_SIZE = 40;
constexpr uint32_t POINT_RECORD_SIZE = 8;
constexpr uint64_t DEFAULT_MAX_FILE_SIZE = 4ull << 30;
constexpr uint32_t FLAG_SPIRAL_VASE = 1u << 0;
constexpr uint32_t FLAG_INDEXED = 1u << 1;

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
    std::array<uint8_t, 4> rgba{{128,128,128,255}};
    std::string preset_id;
    std::string display_name;
};
struct Snapshot {
    uint64_t scene_id{0};
    int32_t plate_index{-1};
    uint32_t flags{FLAG_INDEXED};
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Group> groups;
    std::vector<MaterialSlot> materials;
    std::vector<Point> printable_area;
    std::vector<Point> bed_excluded_area;
    std::vector<Point> wrapping_excluded_area;
    float z_offset{0};
    float printable_height{0};
    std::array<float,3> bounds_min{};
    std::array<float,3> bounds_max{};
};
struct Header {
    uint16_t major_version{0}, minor_version{0};
    uint32_t header_size{0}, vertex_record_size{0}, group_record_size{0}, point_record_size{0};
    uint64_t scene_id{0};
    int32_t plate_index{-1};
    uint32_t flags{0};
    uint64_t vertex_count{0}, index_count{0}, group_count{0};
    uint32_t material_slot_count{0}, printable_area_count{0}, excluded_area_count{0}, bed_excluded_area_count{0};
    uint64_t vertices_offset{0}, indices_offset{0}, groups_offset{0}, materials_offset{0};
    uint64_t printable_area_offset{0}, excluded_area_offset{0}, strings_offset{0}, file_size{0};
    float z_offset{0}, printable_height{0};
    std::array<float,3> bounds_min{}, bounds_max{};
};

Snapshot capture(const GUI::PreviewTriangleMesh& mesh, const GCodeProcessorResult& result,
                 int plate_index, uint64_t expected_scene_id, const Pointfs& fallback_printable_area = {});
std::vector<uint8_t> serialize(const Snapshot& snapshot);
Header validate(const uint8_t* data, size_t size, uint64_t max_file_size = DEFAULT_MAX_FILE_SIZE);
Header validate(const std::vector<uint8_t>& data, uint64_t max_file_size = DEFAULT_MAX_FILE_SIZE);
void write_atomic(const Snapshot& snapshot, const boost::filesystem::path& target);

} // namespace PreviewGeometrySnapshot
} // namespace Slic3r
