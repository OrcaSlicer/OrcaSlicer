#pragma once

#include <boost/filesystem/path.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
struct GCodeProcessorResult;

namespace PreviewGeometrySnapshot {

// ORPV v1 is renderer-neutral, fixed-width, and explicitly little-endian. These constants are
// duplicated by external readers so changing them requires a format-major bump.
constexpr uint16_t FORMAT_MAJOR = 1;
constexpr uint16_t FORMAT_MINOR = 0;
constexpr uint32_t HEADER_BYTE_SIZE = 128;
constexpr uint32_t VERTEX_RECORD_SIZE = 36;
constexpr uint32_t MATERIAL_RECORD_SIZE = 40;
constexpr uint32_t POINT_RECORD_SIZE = 8;
constexpr uint64_t DEFAULT_MAX_VERTICES = 20'000'000;
constexpr uint64_t DEFAULT_MAX_FILE_SIZE = 1ull << 30;

enum PathFlags : uint8_t {
    PathStart    = 1 << 0,
    PathEnd      = 1 << 1,
    InternalOnly = 1 << 2,
};

struct Point {
    float x{0.0f};
    float y{0.0f};
};

struct Vertex {
    float    x{0.0f};
    float    y{0.0f};
    float    z{0.0f};
    float    width{0.0f};
    float    height{0.0f};
    uint32_t gcode_id{0};
    uint32_t layer_id{0};
    uint8_t  move_type{0};
    uint8_t  extrusion_role{0};
    uint8_t  extruder_id{0};
    uint8_t  colour_id{0};
    uint8_t  path_flags{0};
};

struct MaterialSlot {
    uint8_t                 extruder_id{0};
    std::array<uint8_t, 4>  rgba{{128, 128, 128, 255}};
    std::string             preset_id;
    std::string             display_name;
};

struct Snapshot {
    uint64_t                  scene_id{0};
    int32_t                   plate_index{-1};
    uint32_t                  flags{0};
    std::vector<Vertex>       vertices;
    std::vector<MaterialSlot> materials;
    std::vector<Point>        printable_area;
    std::vector<Point>        bed_excluded_area;
    std::vector<Point>        wrapping_excluded_area;
    float                     z_offset{0.0f};
    float                     printable_height{0.0f};
};

struct Header {
    uint16_t major_version{0};
    uint16_t minor_version{0};
    uint32_t header_size{0};
    uint32_t vertex_record_size{0};
    uint32_t material_record_size{0};
    uint32_t point_record_size{0};
    uint64_t scene_id{0};
    int32_t  plate_index{-1};
    uint32_t flags{0};
    uint64_t vertex_count{0};
    uint32_t material_slot_count{0};
    uint32_t printable_area_count{0};
    uint32_t excluded_area_count{0};
    uint32_t bed_excluded_area_count{0};
    uint64_t vertices_offset{0};
    uint64_t materials_offset{0};
    uint64_t printable_area_offset{0};
    uint64_t excluded_area_offset{0};
    uint64_t strings_offset{0};
    uint64_t file_size{0};
    float    z_offset{0.0f};
    float    printable_height{0.0f};
};

Snapshot capture(const GCodeProcessorResult& result, int plate_index, uint64_t expected_scene_id);
std::vector<uint8_t> serialize(const Snapshot& snapshot);
Header validate(const uint8_t* data, size_t size,
                uint64_t max_vertices = DEFAULT_MAX_VERTICES,
                uint64_t max_file_size = DEFAULT_MAX_FILE_SIZE);
Header validate(const std::vector<uint8_t>& data,
                uint64_t max_vertices = DEFAULT_MAX_VERTICES,
                uint64_t max_file_size = DEFAULT_MAX_FILE_SIZE);
void write_atomic(const Snapshot& snapshot, const boost::filesystem::path& target);

} // namespace PreviewGeometrySnapshot
} // namespace Slic3r
