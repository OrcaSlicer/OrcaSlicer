#include "PreviewGeometrySnapshot.hpp"

#include "libslic3r/Color.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <boost/filesystem.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace Slic3r::PreviewGeometrySnapshot {
namespace {

constexpr uint32_t FLAG_SPIRAL_VASE = 1 << 0;

uint64_t checked_add(uint64_t lhs, uint64_t rhs)
{
    if (rhs > std::numeric_limits<uint64_t>::max() - lhs)
        throw std::runtime_error("ORPV size addition overflow");
    return lhs + rhs;
}

uint64_t checked_mul(uint64_t lhs, uint64_t rhs)
{
    if (lhs != 0 && rhs > std::numeric_limits<uint64_t>::max() / lhs)
        throw std::runtime_error("ORPV size multiplication overflow");
    return lhs * rhs;
}

void require_range(size_t size, uint64_t offset, uint64_t length)
{
    if (offset > size || length > size - offset)
        throw std::runtime_error("ORPV field is outside the file");
}

template<typename T> void put_le(std::vector<uint8_t>& data, size_t offset, T value)
{
    static_assert(std::is_integral_v<T>);
    if (offset > data.size() || sizeof(T) > data.size() - offset)
        throw std::runtime_error("ORPV writer offset overflow");
    using U = std::make_unsigned_t<T>;
    const U bits = static_cast<U>(value);
    for (size_t i = 0; i < sizeof(T); ++i)
        data[offset + i] = static_cast<uint8_t>(bits >> (8 * i));
}

void put_float(std::vector<uint8_t>& data, size_t offset, float value)
{
    static_assert(sizeof(float) == sizeof(uint32_t));
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    put_le(data, offset, bits);
}

template<typename T> T get_le(const uint8_t* data, size_t size, uint64_t offset)
{
    static_assert(std::is_integral_v<T>);
    require_range(size, offset, sizeof(T));
    using U = std::make_unsigned_t<T>;
    U bits = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
        bits |= static_cast<U>(data[offset + i]) << (8 * i);
    return static_cast<T>(bits);
}

float get_float(const uint8_t* data, size_t size, uint64_t offset)
{
    const uint32_t bits = get_le<uint32_t>(data, size, offset);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool is_renderable_extrusion(const GCodeProcessorResult::MoveVertex& move)
{
    return move.type == EMoveType::Extrude && move.width > 0.0f && move.height > 0.0f;
}

bool starts_new_path(const GCodeProcessorResult::MoveVertex& previous,
                     const GCodeProcessorResult::MoveVertex& current)
{
    return previous.type != EMoveType::Extrude || previous.extrusion_role != current.extrusion_role ||
           previous.extruder_id != current.extruder_id || previous.cp_color_id != current.cp_color_id ||
           previous.layer_id != current.layer_id || previous.internal_only != current.internal_only;
}

uint8_t channel(float value)
{
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

void validate_finite(float value, const char* field)
{
    if (!std::isfinite(value))
        throw std::runtime_error(std::string("ORPV contains non-finite ") + field);
}

} // namespace

Snapshot capture(const GCodeProcessorResult& result, int plate_index, uint64_t expected_scene_id)
{
    static_assert(sizeof(GCodeProcessorResult::MoveVertex::cp_color_id) == sizeof(uint8_t),
                  "ORPV v1 colour_id must widen if cp_color_id widens");
    static_assert(static_cast<uint8_t>(EMoveType::Extrude) == 10,
                  "ORPV v1 move_type encoding must be versioned if EMoveType changes");
    static_assert(static_cast<unsigned int>(erCount) <= std::numeric_limits<uint8_t>::max(),
                  "ORPV v1 extrusion_role must widen if ExtrusionRole widens");

    Snapshot snapshot;
    std::vector<GCodeProcessorResult::MoveVertex> moves;
    std::vector<std::string> extruder_colors;
    std::vector<std::string> filament_ids;
    size_t filaments_count = 0;
    const auto copy_points = [](const Pointfs& source, std::vector<Point>& target) {
        target.reserve(source.size());
        for (const Vec2d& point : source)
            target.push_back({static_cast<float>(point.x()), static_cast<float>(point.y())});
    };

    // The caller suppresses background processing while this compact copy is made. Keep only the
    // copy under result_mutex; validation and path normalization happen after releasing it.
    {
        std::lock_guard<std::mutex> lock(result.result_mutex);
        if (result.id != expected_scene_id)
            throw std::runtime_error("The slice result changed before ORPV capture");
        if (result.moves.empty())
            throw std::runtime_error("Cannot build an ORPV snapshot from an empty result");
        if (result.moves.size() > DEFAULT_MAX_VERTICES)
            throw std::runtime_error("ORPV vertex limit exceeded");

        snapshot.scene_id = result.id;
        snapshot.plate_index = plate_index;
        snapshot.flags = result.spiral_vase_mode ? FLAG_SPIRAL_VASE : 0;
        snapshot.z_offset = result.z_offset;
        snapshot.printable_height = result.printable_height;
        moves = result.moves;
        extruder_colors = result.extruder_colors;
        filament_ids = result.settings_ids.filament;
        filaments_count = result.filaments_count;
        copy_points(result.printable_area, snapshot.printable_area);
        copy_points(result.bed_exclude_area, snapshot.bed_excluded_area);
        copy_points(result.wrapping_exclude_area, snapshot.wrapping_excluded_area);
    }

    validate_finite(snapshot.z_offset, "z offset");
    validate_finite(snapshot.printable_height, "printable height");
    if (snapshot.printable_height < 0.0f)
        throw std::runtime_error("ORPV printable height is negative");

    snapshot.vertices.reserve(moves.size());
    const auto make_vertex = [](const GCodeProcessorResult::MoveVertex& move, const Vec3f& position, uint8_t flags) {
        Vertex vertex;
        vertex.x = position.x();
        vertex.y = position.y();
        vertex.z = position.z();
        vertex.width = move.width;
        vertex.height = move.height;
        vertex.gcode_id = move.gcode_id;
        vertex.layer_id = move.layer_id;
        vertex.move_type = static_cast<uint8_t>(EMoveType::Extrude);
        vertex.extrusion_role = static_cast<uint8_t>(move.extrusion_role);
        vertex.extruder_id = move.extruder_id;
        vertex.colour_id = move.cp_color_id;
        vertex.path_flags = flags | (move.internal_only ? InternalOnly : 0);
        validate_finite(vertex.x, "vertex x");
        validate_finite(vertex.y, "vertex y");
        validate_finite(vertex.z, "vertex z");
        validate_finite(vertex.width, "vertex width");
        validate_finite(vertex.height, "vertex height");
        return vertex;
    };

    // MoveVertex stores move destinations. Each extrusion segment therefore starts at the previous
    // move's position and ends at the current extrusion position. Duplicate that anchor only when a
    // path starts; travel/wipe/control moves never become renderable ORPV primitives.
    for (size_t i = 1; i < moves.size(); ++i) {
        const auto& previous = moves[i - 1];
        const auto& current = moves[i];
        if (!is_renderable_extrusion(current))
            continue;

        const bool begins_path = snapshot.vertices.empty() || !is_renderable_extrusion(previous) ||
                                 starts_new_path(previous, current);
        if (begins_path) {
            if (!snapshot.vertices.empty())
                snapshot.vertices.back().path_flags |= PathEnd;
            snapshot.vertices.push_back(make_vertex(current, previous.position, PathStart));
        }
        snapshot.vertices.push_back(make_vertex(current, current.position, 0));
        if (snapshot.vertices.size() > DEFAULT_MAX_VERTICES)
            throw std::runtime_error("ORPV vertex limit exceeded after path anchoring");
    }
    if (snapshot.vertices.empty())
        throw std::runtime_error("Cannot build an ORPV snapshot without extrusion moves");
    snapshot.vertices.back().path_flags |= PathEnd;

    const size_t material_count = std::max({extruder_colors.size(), filament_ids.size(), filaments_count});
    if (material_count > std::numeric_limits<uint8_t>::max() + size_t{1})
        throw std::runtime_error("ORPV material-slot limit exceeded");
    snapshot.materials.reserve(material_count);
    for (size_t i = 0; i < material_count; ++i) {
        MaterialSlot material;
        material.extruder_id = static_cast<uint8_t>(i);
        ColorRGBA color = ColorRGBA::GRAY();
        if (i < extruder_colors.size())
            decode_color(extruder_colors[i], color);
        material.rgba = {{channel(color.r()), channel(color.g()), channel(color.b()), channel(color.a())}};
        if (i < filament_ids.size()) {
            material.preset_id = filament_ids[i];
            material.display_name = filament_ids[i];
        }
        snapshot.materials.push_back(std::move(material));
    }

    for (const Point& point : snapshot.printable_area) {
        validate_finite(point.x, "plate x");
        validate_finite(point.y, "plate y");
    }
    for (const Point& point : snapshot.bed_excluded_area) {
        validate_finite(point.x, "plate x");
        validate_finite(point.y, "plate y");
    }
    for (const Point& point : snapshot.wrapping_excluded_area) {
        validate_finite(point.x, "plate x");
        validate_finite(point.y, "plate y");
    }
    return snapshot;
}

std::vector<uint8_t> serialize(const Snapshot& snapshot)
{
    if (snapshot.vertices.empty())
        throw std::runtime_error("Cannot serialize an empty ORPV snapshot");
    if (snapshot.vertices.size() > DEFAULT_MAX_VERTICES)
        throw std::runtime_error("ORPV vertex limit exceeded");
    if (snapshot.materials.size() > std::numeric_limits<uint32_t>::max() ||
        snapshot.printable_area.size() > std::numeric_limits<uint32_t>::max() ||
        snapshot.bed_excluded_area.size() > std::numeric_limits<uint32_t>::max() ||
        snapshot.wrapping_excluded_area.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("ORPV record count exceeds v1 limits");

    const uint64_t vertices_offset = HEADER_BYTE_SIZE;
    const uint64_t materials_offset = checked_add(vertices_offset, checked_mul(snapshot.vertices.size(), VERTEX_RECORD_SIZE));
    const uint64_t printable_offset = checked_add(materials_offset, checked_mul(snapshot.materials.size(), MATERIAL_RECORD_SIZE));
    const uint64_t excluded_offset = checked_add(printable_offset, checked_mul(snapshot.printable_area.size(), POINT_RECORD_SIZE));
    const uint64_t excluded_count = checked_add(snapshot.bed_excluded_area.size(), snapshot.wrapping_excluded_area.size());
    const uint64_t strings_offset = checked_add(excluded_offset, checked_mul(excluded_count, POINT_RECORD_SIZE));

    uint64_t file_size = strings_offset;
    for (const MaterialSlot& material : snapshot.materials) {
        file_size = checked_add(file_size, material.preset_id.size());
        file_size = checked_add(file_size, material.display_name.size());
    }
    if (file_size > DEFAULT_MAX_FILE_SIZE || file_size > std::numeric_limits<size_t>::max())
        throw std::runtime_error("ORPV file-size limit exceeded");

    std::vector<uint8_t> data(static_cast<size_t>(file_size), 0);
    data[0] = 'O'; data[1] = 'R'; data[2] = 'P'; data[3] = 'V';
    put_le<uint16_t>(data, 4, FORMAT_MAJOR);
    put_le<uint16_t>(data, 6, FORMAT_MINOR);
    put_le<uint32_t>(data, 8, HEADER_BYTE_SIZE);
    put_le<uint32_t>(data, 12, VERTEX_RECORD_SIZE);
    put_le<uint32_t>(data, 16, MATERIAL_RECORD_SIZE);
    put_le<uint32_t>(data, 20, POINT_RECORD_SIZE);
    put_le<uint64_t>(data, 24, snapshot.scene_id);
    put_le<int32_t>(data, 32, snapshot.plate_index);
    put_le<uint32_t>(data, 36, snapshot.flags);
    put_le<uint64_t>(data, 40, snapshot.vertices.size());
    put_le<uint32_t>(data, 48, static_cast<uint32_t>(snapshot.materials.size()));
    put_le<uint32_t>(data, 52, static_cast<uint32_t>(snapshot.printable_area.size()));
    put_le<uint32_t>(data, 56, static_cast<uint32_t>(excluded_count));
    put_le<uint32_t>(data, 60, static_cast<uint32_t>(snapshot.bed_excluded_area.size()));
    put_le<uint64_t>(data, 64, vertices_offset);
    put_le<uint64_t>(data, 72, materials_offset);
    put_le<uint64_t>(data, 80, printable_offset);
    put_le<uint64_t>(data, 88, excluded_offset);
    put_le<uint64_t>(data, 96, strings_offset);
    put_le<uint64_t>(data, 104, file_size);
    put_float(data, 112, snapshot.z_offset);
    put_float(data, 116, snapshot.printable_height);

    for (size_t i = 0; i < snapshot.vertices.size(); ++i) {
        const Vertex& vertex = snapshot.vertices[i];
        const size_t offset = static_cast<size_t>(vertices_offset) + i * VERTEX_RECORD_SIZE;
        put_float(data, offset, vertex.x);
        put_float(data, offset + 4, vertex.y);
        put_float(data, offset + 8, vertex.z);
        put_float(data, offset + 12, vertex.width);
        put_float(data, offset + 16, vertex.height);
        put_le<uint32_t>(data, offset + 20, vertex.gcode_id);
        put_le<uint32_t>(data, offset + 24, vertex.layer_id);
        data[offset + 28] = vertex.move_type;
        data[offset + 29] = vertex.extrusion_role;
        data[offset + 30] = vertex.extruder_id;
        data[offset + 31] = vertex.colour_id;
        data[offset + 32] = vertex.path_flags;
    }

    size_t string_cursor = static_cast<size_t>(strings_offset);
    for (size_t i = 0; i < snapshot.materials.size(); ++i) {
        const MaterialSlot& material = snapshot.materials[i];
        const size_t offset = static_cast<size_t>(materials_offset) + i * MATERIAL_RECORD_SIZE;
        data[offset] = material.extruder_id;
        std::copy(material.rgba.begin(), material.rgba.end(), data.begin() + offset + 1);
        put_le<uint64_t>(data, offset + 8, string_cursor);
        put_le<uint32_t>(data, offset + 16, static_cast<uint32_t>(material.preset_id.size()));
        std::copy(material.preset_id.begin(), material.preset_id.end(), data.begin() + string_cursor);
        string_cursor += material.preset_id.size();
        put_le<uint32_t>(data, offset + 20, static_cast<uint32_t>(material.display_name.size()));
        put_le<uint64_t>(data, offset + 24, string_cursor);
        std::copy(material.display_name.begin(), material.display_name.end(), data.begin() + string_cursor);
        string_cursor += material.display_name.size();
    }

    size_t point_cursor = static_cast<size_t>(printable_offset);
    const auto write_points = [&data, &point_cursor](const std::vector<Point>& points) {
        for (const Point& point : points) {
            put_float(data, point_cursor, point.x);
            put_float(data, point_cursor + 4, point.y);
            point_cursor += POINT_RECORD_SIZE;
        }
    };
    write_points(snapshot.printable_area);
    point_cursor = static_cast<size_t>(excluded_offset);
    write_points(snapshot.bed_excluded_area);
    write_points(snapshot.wrapping_excluded_area);

    validate(data);
    return data;
}

Header validate(const uint8_t* data, size_t size, uint64_t max_vertices, uint64_t max_file_size)
{
    if (data == nullptr || size < HEADER_BYTE_SIZE)
        throw std::runtime_error("ORPV file is truncated");
    if (size > max_file_size)
        throw std::runtime_error("ORPV file-size limit exceeded");
    if (data[0] != 'O' || data[1] != 'R' || data[2] != 'P' || data[3] != 'V')
        throw std::runtime_error("ORPV magic is invalid");

    Header header;
    header.major_version = get_le<uint16_t>(data, size, 4);
    header.minor_version = get_le<uint16_t>(data, size, 6);
    header.header_size = get_le<uint32_t>(data, size, 8);
    header.vertex_record_size = get_le<uint32_t>(data, size, 12);
    header.material_record_size = get_le<uint32_t>(data, size, 16);
    header.point_record_size = get_le<uint32_t>(data, size, 20);
    header.scene_id = get_le<uint64_t>(data, size, 24);
    header.plate_index = get_le<int32_t>(data, size, 32);
    header.flags = get_le<uint32_t>(data, size, 36);
    header.vertex_count = get_le<uint64_t>(data, size, 40);
    header.material_slot_count = get_le<uint32_t>(data, size, 48);
    header.printable_area_count = get_le<uint32_t>(data, size, 52);
    header.excluded_area_count = get_le<uint32_t>(data, size, 56);
    header.bed_excluded_area_count = get_le<uint32_t>(data, size, 60);
    header.vertices_offset = get_le<uint64_t>(data, size, 64);
    header.materials_offset = get_le<uint64_t>(data, size, 72);
    header.printable_area_offset = get_le<uint64_t>(data, size, 80);
    header.excluded_area_offset = get_le<uint64_t>(data, size, 88);
    header.strings_offset = get_le<uint64_t>(data, size, 96);
    header.file_size = get_le<uint64_t>(data, size, 104);
    header.z_offset = get_float(data, size, 112);
    header.printable_height = get_float(data, size, 116);

    if (header.major_version != FORMAT_MAJOR)
        throw std::runtime_error("Unsupported ORPV major version");
    if (header.header_size < HEADER_BYTE_SIZE || header.header_size > size)
        throw std::runtime_error("ORPV header size is invalid");
    if (header.vertex_record_size < VERTEX_RECORD_SIZE || header.material_record_size < MATERIAL_RECORD_SIZE ||
        header.point_record_size < POINT_RECORD_SIZE)
        throw std::runtime_error("ORPV record size is invalid");
    if (header.file_size != size)
        throw std::runtime_error("ORPV file size does not match its header");
    if (header.vertex_count == 0 || header.vertex_count > max_vertices)
        throw std::runtime_error("ORPV vertex count is invalid");
    if (header.bed_excluded_area_count > header.excluded_area_count)
        throw std::runtime_error("ORPV excluded-area counts are invalid");
    validate_finite(header.z_offset, "z offset");
    validate_finite(header.printable_height, "printable height");
    if (header.printable_height < 0.0f)
        throw std::runtime_error("ORPV printable height is negative");

    const uint64_t vertices_end = checked_add(header.vertices_offset,
                                               checked_mul(header.vertex_count, header.vertex_record_size));
    const uint64_t materials_end = checked_add(header.materials_offset,
                                                checked_mul(header.material_slot_count, header.material_record_size));
    const uint64_t printable_end = checked_add(header.printable_area_offset,
                                                checked_mul(header.printable_area_count, header.point_record_size));
    const uint64_t excluded_end = checked_add(header.excluded_area_offset,
                                               checked_mul(header.excluded_area_count, header.point_record_size));
    if (header.vertices_offset < header.header_size || header.materials_offset < vertices_end ||
        header.printable_area_offset < materials_end || header.excluded_area_offset < printable_end ||
        header.strings_offset < excluded_end || header.strings_offset > header.file_size)
        throw std::runtime_error("ORPV section offsets overlap or are out of order");
    require_range(size, header.vertices_offset, checked_mul(header.vertex_count, header.vertex_record_size));
    require_range(size, header.materials_offset, checked_mul(header.material_slot_count, header.material_record_size));
    require_range(size, header.printable_area_offset, checked_mul(header.printable_area_count, header.point_record_size));
    require_range(size, header.excluded_area_offset, checked_mul(header.excluded_area_count, header.point_record_size));

    for (uint64_t i = 0; i < header.vertex_count; ++i) {
        const uint64_t offset = header.vertices_offset + i * header.vertex_record_size;
        for (uint64_t field = 0; field < 5; ++field) {
            const float value = get_float(data, size, offset + field * 4);
            validate_finite(value, "vertex field");
            if (field >= 3 && value < 0.0f)
                throw std::runtime_error("ORPV contains negative extrusion dimensions");
        }
    }
    const auto validate_points = [data, size, &header](uint64_t offset, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            validate_finite(get_float(data, size, offset + uint64_t{i} * header.point_record_size), "plate x");
            validate_finite(get_float(data, size, offset + uint64_t{i} * header.point_record_size + 4), "plate y");
        }
    };
    validate_points(header.printable_area_offset, header.printable_area_count);
    validate_points(header.excluded_area_offset, header.excluded_area_count);

    for (uint32_t i = 0; i < header.material_slot_count; ++i) {
        const uint64_t offset = header.materials_offset + uint64_t{i} * header.material_record_size;
        const uint64_t preset_offset = get_le<uint64_t>(data, size, offset + 8);
        const uint32_t preset_length = get_le<uint32_t>(data, size, offset + 16);
        const uint32_t display_length = get_le<uint32_t>(data, size, offset + 20);
        const uint64_t display_offset = get_le<uint64_t>(data, size, offset + 24);
        if (preset_offset < header.strings_offset || display_offset < header.strings_offset)
            throw std::runtime_error("ORPV material string offset is invalid");
        require_range(size, preset_offset, preset_length);
        require_range(size, display_offset, display_length);
    }
    return header;
}

Header validate(const std::vector<uint8_t>& data, uint64_t max_vertices, uint64_t max_file_size)
{
    return validate(data.data(), data.size(), max_vertices, max_file_size);
}

void write_atomic(const Snapshot& snapshot, const boost::filesystem::path& target)
{
    namespace fs = boost::filesystem;
    const std::vector<uint8_t> data = serialize(snapshot);
    fs::create_directories(target.parent_path());

    const std::string suffix = ".tmp-" + std::to_string(snapshot.scene_id) + "-" +
                               std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const fs::path temporary(target.string() + suffix);
    try {
        {
            std::ofstream output(temporary.string(), std::ios::binary | std::ios::trunc);
            if (!output)
                throw std::runtime_error("Unable to create ORPV temporary file");
            output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            output.flush();
            if (!output)
                throw std::runtime_error("Unable to write ORPV temporary file");
        }

        boost::system::error_code ec;
        fs::rename(temporary, target, ec);
        if (ec) {
            if (fs::exists(target)) {
                std::ifstream existing(target.string(), std::ios::binary | std::ios::ate);
                const uint64_t existing_size = existing ? static_cast<uint64_t>(existing.tellg()) : 0;
                std::array<uint8_t, 32> existing_header{};
                if (existing_size == data.size()) {
                    existing.seekg(0);
                    existing.read(reinterpret_cast<char*>(existing_header.data()), existing_header.size());
                }
                if (existing && existing_header[0] == 'O' && existing_header[1] == 'R' && existing_header[2] == 'P' &&
                    existing_header[3] == 'V' && get_le<uint64_t>(existing_header.data(), existing_header.size(), 24) == snapshot.scene_id) {
                    fs::remove(temporary);
                    return;
                }
            }
            throw std::runtime_error("Unable to publish ORPV snapshot: " + ec.message());
        }
    } catch (...) {
        boost::system::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

} // namespace Slic3r::PreviewGeometrySnapshot
