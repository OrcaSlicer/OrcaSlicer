#include "PreviewGeometrySnapshot.hpp"

#include "slic3r/GUI/ToolpathMeshBuilder.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#ifdef _WIN32
#include "libslic3r/Utils.hpp"
#endif

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <type_traits>

namespace Slic3r::PreviewGeometrySnapshot {
namespace {

constexpr uint32_t MATERIAL_EXTRUDER_ID_OFFSET = 0;
constexpr uint32_t MATERIAL_RGBA_OFFSET = 1;
constexpr uint32_t MATERIAL_PREFIX_RESERVED_OFFSET = 5;
constexpr uint32_t MATERIAL_PREFIX_RESERVED_SIZE = 3;
constexpr uint32_t MATERIAL_PRESET_OFFSET = 8;
constexpr uint32_t MATERIAL_PRESET_LENGTH_OFFSET = 16;
constexpr uint32_t MATERIAL_NAME_LENGTH_OFFSET = 20;
constexpr uint32_t MATERIAL_NAME_OFFSET = 24;
constexpr uint32_t MATERIAL_SUFFIX_RESERVED_OFFSET = 32;
constexpr uint32_t MATERIAL_SUFFIX_RESERVED_SIZE = 8;
static_assert(MATERIAL_SUFFIX_RESERVED_OFFSET + MATERIAL_SUFFIX_RESERVED_SIZE == MATERIAL_RECORD_SIZE);

uint64_t checked_add(uint64_t a, uint64_t b)
{
    if (b > std::numeric_limits<uint64_t>::max() - a)
        throw std::runtime_error("ORPM size overflow");
    return a + b;
}

uint64_t checked_mul(uint64_t a, uint64_t b)
{
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a)
        throw std::runtime_error("ORPM size overflow");
    return a * b;
}

void require_range(size_t size, uint64_t offset, uint64_t length)
{
    if (offset > size || length > size - offset)
        throw std::runtime_error("ORPM region out of bounds");
}

template<class T> void put_le(std::vector<uint8_t>& data, size_t offset, T value)
{
    static_assert(std::is_integral_v<T>);
    using U = std::make_unsigned_t<T>;
    if (offset > data.size() || sizeof(T) > data.size() - offset)
        throw std::runtime_error("ORPM writer overflow");
    const U encoded = static_cast<U>(value);
    for (size_t i = 0; i < sizeof(T); ++i)
        data[offset + i] = uint8_t(encoded >> (8 * i));
}

template<class T> T get_le(const uint8_t* data, size_t size, uint64_t offset)
{
    static_assert(std::is_integral_v<T>);
    require_range(size, offset, sizeof(T));
    using U = std::make_unsigned_t<T>;
    U value{};
    for (size_t i = 0; i < sizeof(T); ++i)
        value |= U(data[offset + i]) << (8 * i);
    return static_cast<T>(value);
}

void put_float(std::vector<uint8_t>& data, size_t offset, float value)
{
    uint32_t encoded;
    std::memcpy(&encoded, &value, sizeof(value));
    put_le(data, offset, encoded);
}

float get_float(const uint8_t* data, size_t size, uint64_t offset)
{
    const uint32_t encoded = get_le<uint32_t>(data, size, offset);
    float value;
    std::memcpy(&value, &encoded, sizeof(value));
    return value;
}

void finite(float value, const char* what)
{
    if (!std::isfinite(value))
        throw std::runtime_error(std::string("ORPM non-finite ") + what);
}

uint8_t channel(float value)
{
    return uint8_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

uint64_t region_end(uint64_t offset, uint64_t count, uint64_t stride, uint64_t size)
{
    const uint64_t bytes = checked_mul(count, stride);
    if (offset > size || bytes > size - offset)
        throw std::runtime_error("ORPM section out of bounds");
    return offset + bytes;
}

void require_reserved_zero(const uint8_t* data, size_t size, uint64_t offset, uint64_t length, const char* section)
{
    require_range(size, offset, length);
    if (std::any_of(data + offset, data + offset + length, [](uint8_t value) { return value != 0; }))
        throw std::runtime_error(std::string("ORPM reserved ") + section + " bytes are non-zero");
}

} // namespace

Snapshot capture(const GUI::PreviewTriangleMesh& mesh,const GCodeProcessorResult& result,int plate_index,uint64_t expected_scene_id,const Pointfs& fallback_printable_area)
{
    if(mesh.vertices.empty()||mesh.indices.empty()||mesh.groups.empty())throw std::runtime_error("Preview mesh is empty");
    Snapshot out; out.scene_id=expected_scene_id; out.plate_index=plate_index; out.flags=FLAG_INDEXED;
    std::vector<std::string> colors,filament_ids; size_t filament_count{};
    {
        std::lock_guard<std::mutex> lock(result.result_mutex);
        if(result.id!=expected_scene_id)throw std::runtime_error("Slice result changed before ORPM capture");
        out.flags|=result.spiral_vase_mode?FLAG_SPIRAL_VASE:0; out.z_offset=result.z_offset;out.printable_height=result.printable_height;
        colors=result.extruder_colors;filament_ids=result.settings_ids.filament;filament_count=result.filaments_count;
        auto copy=[](const Pointfs& src,std::vector<Point>& dst){dst.reserve(src.size());for(const Vec2d&p:src)dst.push_back({float(p.x()),float(p.y())});};
        copy(result.printable_area.empty()?fallback_printable_area:result.printable_area,out.printable_area);copy(result.bed_exclude_area,out.bed_excluded_area);copy(result.wrapping_exclude_area,out.wrapping_excluded_area);
    }
    out.vertices.reserve(mesh.vertices.size());
    for(const auto& v:mesh.vertices)out.vertices.push_back({{v.position.x(),v.position.y(),v.position.z()},{v.normal.x(),v.normal.y(),v.normal.z()},{v.uv.x(),v.uv.y()}});
    out.indices=mesh.indices; out.groups.reserve(mesh.groups.size());
    uint16_t max_slot=0;
    for(const auto& g:mesh.groups){out.groups.push_back({g.first_index,g.index_count,g.material_slot,g.extrusion_role,g.extruder_id,g.colour_id,g.layer_id});max_slot=std::max(max_slot,g.material_slot);}
    const size_t material_count=std::max({colors.size(),filament_ids.size(),filament_count,size_t(max_slot)+1});
    for(size_t i=0;i<material_count;++i){MaterialSlot m;m.extruder_id=uint8_t(std::min<size_t>(i,255));ColorRGBA c;if(i>=colors.size()||!decode_color(colors[i],c))c=ColorRGBA::GRAY();m.rgba={{channel(c.r()),channel(c.g()),channel(c.b()),channel(c.a())}};if(i<filament_ids.size()){m.preset_id=filament_ids[i];m.display_name=filament_ids[i];}out.materials.push_back(std::move(m));}
    if(mesh.bounds.defined){for(size_t i=0;i<3;++i){out.bounds_min[i]=mesh.bounds.min[int(i)];out.bounds_max[i]=mesh.bounds.max[int(i)];}}
    return out;
}

std::vector<uint8_t> serialize(const Snapshot& s)
{
    if(s.vertices.empty()||s.indices.empty()||s.groups.empty())throw std::runtime_error("ORPM has an empty required section");
    struct Ref{uint32_t off,len;};struct MRef{Ref preset,name;};std::vector<uint8_t> strings;std::vector<MRef> refs;
    auto add_string=[&](const std::string& value){if(value.size()>UINT32_MAX||strings.size()>UINT32_MAX-value.size())throw std::runtime_error("ORPM string table too large");Ref r{uint32_t(strings.size()),uint32_t(value.size())};strings.insert(strings.end(),value.begin(),value.end());return r;};
    for(const auto&m:s.materials)refs.push_back({add_string(m.preset_id),add_string(m.display_name)});
    const uint64_t vo=HEADER_BYTE_SIZE,io=checked_add(vo,checked_mul(s.vertices.size(),VERTEX_RECORD_SIZE));
    const uint64_t go=checked_add(io,checked_mul(s.indices.size(),4));
    const uint64_t mo=checked_add(go,checked_mul(s.groups.size(),GROUP_RECORD_SIZE));
    const uint64_t po=checked_add(mo,checked_mul(s.materials.size(),MATERIAL_RECORD_SIZE));
    const uint64_t eo=checked_add(po,checked_mul(s.printable_area.size(),POINT_RECORD_SIZE));
    const uint64_t so=checked_add(eo,checked_mul(s.bed_excluded_area.size()+s.wrapping_excluded_area.size(),POINT_RECORD_SIZE));
    const uint64_t fs=checked_add(so,strings.size());if(fs>DEFAULT_MAX_FILE_SIZE||fs>SIZE_MAX)throw std::runtime_error("ORPM file too large");
    std::vector<uint8_t>d(size_t(fs),0);std::memcpy(d.data(),FORMAT_NAME,4);put_le(d,4,FORMAT_MAJOR);put_le(d,6,FORMAT_MINOR);put_le(d,8,HEADER_BYTE_SIZE);put_le(d,12,VERTEX_RECORD_SIZE);put_le(d,16,GROUP_RECORD_SIZE);put_le(d,20,POINT_RECORD_SIZE);put_le(d,24,s.scene_id);put_le(d,32,s.plate_index);put_le(d,36,s.flags|FLAG_INDEXED);put_le(d,40,uint64_t(s.vertices.size()));put_le(d,48,uint64_t(s.indices.size()));put_le(d,56,uint64_t(s.groups.size()));put_le(d,64,uint32_t(s.materials.size()));put_le(d,68,uint32_t(s.printable_area.size()));put_le(d,72,uint32_t(s.bed_excluded_area.size()+s.wrapping_excluded_area.size()));put_le(d,76,uint32_t(s.bed_excluded_area.size()));put_le(d,80,vo);put_le(d,88,io);put_le(d,96,go);put_le(d,104,mo);put_le(d,112,po);put_le(d,120,eo);put_le(d,128,so);put_le(d,136,fs);put_float(d,144,s.z_offset);put_float(d,148,s.printable_height);for(size_t i=0;i<3;++i){put_float(d,152+4*i,s.bounds_min[i]);put_float(d,164+4*i,s.bounds_max[i]);}
    size_t o=size_t(vo);for(const auto&v:s.vertices){for(float f:v.position){finite(f,"position");put_float(d,o,f);o+=4;}for(float f:v.normal){finite(f,"normal");put_float(d,o,f);o+=4;}for(float f:v.uv){finite(f,"uv");put_float(d,o,f);o+=4;}}
    o=size_t(io);for(uint32_t index:s.indices){if(index>=s.vertices.size())throw std::runtime_error("ORPM index out of range");put_le(d,o,index);o+=4;}
    o=size_t(go);uint64_t covered=0;for(const auto&g:s.groups){if(g.first_index!=covered||g.index_count==0||g.index_count%3||uint64_t(g.first_index)+g.index_count>s.indices.size()||g.material_slot>=s.materials.size())throw std::runtime_error("ORPM invalid group");put_le(d,o,g.first_index);put_le(d,o+4,g.index_count);put_le(d,o+8,g.material_slot);put_le(d,o+10,g.extrusion_role);put_le(d,o+11,g.extruder_id);put_le(d,o+12,g.colour_id);put_le(d,o+16,g.layer_id);covered+=g.index_count;o+=GROUP_RECORD_SIZE;}if(covered!=s.indices.size())throw std::runtime_error("ORPM groups do not cover indices");
    o = size_t(mo);
    for (size_t i = 0; i < s.materials.size(); ++i) {
        const MaterialSlot& material = s.materials[i];
        const MRef& ref = refs[i];
        d[o + MATERIAL_EXTRUDER_ID_OFFSET] = material.extruder_id;
        std::copy(material.rgba.begin(), material.rgba.end(), d.begin() + o + MATERIAL_RGBA_OFFSET);
        put_le(d, o + MATERIAL_PRESET_OFFSET, so + ref.preset.off);
        put_le(d, o + MATERIAL_PRESET_LENGTH_OFFSET, ref.preset.len);
        put_le(d, o + MATERIAL_NAME_LENGTH_OFFSET, ref.name.len);
        put_le(d, o + MATERIAL_NAME_OFFSET, so + ref.name.off);
        o += MATERIAL_RECORD_SIZE;
    }
    auto write_points=[&](const std::vector<Point>&pts){for(const auto&p:pts){finite(p.x,"plate x");finite(p.y,"plate y");put_float(d,o,p.x);put_float(d,o+4,p.y);o+=8;}};write_points(s.printable_area);write_points(s.bed_excluded_area);write_points(s.wrapping_excluded_area);std::copy(strings.begin(),strings.end(),d.begin()+size_t(so));validate(d);return d;
}

Header validate(const uint8_t* data, size_t size, uint64_t max_file_size)
{
    if (data == nullptr || size < HEADER_BYTE_SIZE || size > max_file_size || std::memcmp(data, FORMAT_NAME, 4) != 0)
        throw std::runtime_error("Invalid ORPM header");

    Header header;
    header.major_version = get_le<uint16_t>(data, size, 4);
    header.minor_version = get_le<uint16_t>(data, size, 6);
    header.header_size = get_le<uint32_t>(data, size, 8);
    header.vertex_record_size = get_le<uint32_t>(data, size, 12);
    header.group_record_size = get_le<uint32_t>(data, size, 16);
    header.point_record_size = get_le<uint32_t>(data, size, 20);
    if (header.major_version != FORMAT_MAJOR || header.header_size < HEADER_BYTE_SIZE || header.header_size > size ||
        header.vertex_record_size < VERTEX_RECORD_SIZE || header.group_record_size < GROUP_RECORD_SIZE ||
        header.point_record_size < POINT_RECORD_SIZE)
        throw std::runtime_error("Unsupported ORPM layout");

    header.scene_id = get_le<uint64_t>(data, size, 24);
    header.plate_index = get_le<int32_t>(data, size, 32);
    header.flags = get_le<uint32_t>(data, size, 36);
    header.vertex_count = get_le<uint64_t>(data, size, 40);
    header.index_count = get_le<uint64_t>(data, size, 48);
    header.group_count = get_le<uint64_t>(data, size, 56);
    header.material_slot_count = get_le<uint32_t>(data, size, 64);
    header.printable_area_count = get_le<uint32_t>(data, size, 68);
    header.excluded_area_count = get_le<uint32_t>(data, size, 72);
    header.bed_excluded_area_count = get_le<uint32_t>(data, size, 76);
    header.vertices_offset = get_le<uint64_t>(data, size, 80);
    header.indices_offset = get_le<uint64_t>(data, size, 88);
    header.groups_offset = get_le<uint64_t>(data, size, 96);
    header.materials_offset = get_le<uint64_t>(data, size, 104);
    header.printable_area_offset = get_le<uint64_t>(data, size, 112);
    header.excluded_area_offset = get_le<uint64_t>(data, size, 120);
    header.strings_offset = get_le<uint64_t>(data, size, 128);
    header.file_size = get_le<uint64_t>(data, size, 136);
    header.z_offset = get_float(data, size, 144);
    header.printable_height = get_float(data, size, 148);
    for (size_t i = 0; i < 3; ++i) {
        header.bounds_min[i] = get_float(data, size, 152 + 4 * i);
        header.bounds_max[i] = get_float(data, size, 164 + 4 * i);
        finite(header.bounds_min[i], "bounds");
        finite(header.bounds_max[i], "bounds");
        if (header.bounds_min[i] > header.bounds_max[i])
            throw std::runtime_error("ORPM inverted bounds");
    }
    finite(header.z_offset, "z offset");
    finite(header.printable_height, "height");
    require_reserved_zero(data, size, 176, HEADER_BYTE_SIZE - 176, "header");

    if (header.file_size != size || header.vertex_count == 0 || header.index_count == 0 || header.group_count == 0 ||
        header.material_slot_count == 0 || header.material_slot_count > uint32_t(std::numeric_limits<uint16_t>::max()) + 1 ||
        header.bed_excluded_area_count > header.excluded_area_count || (header.flags & FLAG_INDEXED) == 0)
        throw std::runtime_error("ORPM invalid count or flags");

    const uint64_t vertices_end = region_end(header.vertices_offset, header.vertex_count, header.vertex_record_size, size);
    const uint64_t indices_end = region_end(header.indices_offset, header.index_count, sizeof(uint32_t), size);
    const uint64_t groups_end = region_end(header.groups_offset, header.group_count, header.group_record_size, size);
    const uint64_t materials_end = region_end(header.materials_offset, header.material_slot_count, MATERIAL_RECORD_SIZE, size);
    const uint64_t printable_area_end =
        region_end(header.printable_area_offset, header.printable_area_count, header.point_record_size, size);
    const uint64_t excluded_area_end =
        region_end(header.excluded_area_offset, header.excluded_area_count, header.point_record_size, size);
    if (header.vertices_offset < header.header_size || header.indices_offset < vertices_end ||
        header.groups_offset < indices_end || header.materials_offset < groups_end ||
        header.printable_area_offset < materials_end || header.excluded_area_offset < printable_area_end ||
        header.strings_offset < excluded_area_end || header.strings_offset > size)
        throw std::runtime_error("ORPM overlapping sections");

    for (uint64_t i = 0; i < header.vertex_count; ++i) {
        const uint64_t offset = header.vertices_offset + i * header.vertex_record_size;
        float normal_length_squared = 0;
        for (int component = 0; component < 8; ++component) {
            const float value = get_float(data, size, offset + 4 * component);
            finite(value, "vertex");
            if (component >= 3 && component < 6)
                normal_length_squared += value * value;
        }
        if (normal_length_squared < 0.25f || normal_length_squared > 2.25f)
            throw std::runtime_error("ORPM invalid normal");
    }

    for (uint64_t i = 0; i < header.index_count; ++i)
        if (get_le<uint32_t>(data, size, header.indices_offset + sizeof(uint32_t) * i) >= header.vertex_count)
            throw std::runtime_error("ORPM index out of range");

    uint64_t covered_indices = 0;
    for (uint64_t i = 0; i < header.group_count; ++i) {
        const uint64_t offset = header.groups_offset + i * header.group_record_size;
        const uint32_t first_index = get_le<uint32_t>(data, size, offset);
        const uint32_t index_count = get_le<uint32_t>(data, size, offset + 4);
        const uint16_t material_slot = get_le<uint16_t>(data, size, offset + 8);
        if (first_index != covered_indices || index_count == 0 || index_count % 3 != 0 ||
            uint64_t(first_index) + index_count > header.index_count || material_slot >= header.material_slot_count)
            throw std::runtime_error("ORPM invalid group");
        require_reserved_zero(data, size, offset + 13, 3, "group");
        require_reserved_zero(data, size, offset + 20, 4, "group");
        covered_indices += index_count;
    }
    if (covered_indices != header.index_count)
        throw std::runtime_error("ORPM group coverage mismatch");

    const auto validate_points = [&](uint64_t offset, uint32_t count) {
        for (uint64_t i = 0; i < count; ++i) {
            const uint64_t point_offset = offset + i * header.point_record_size;
            finite(get_float(data, size, point_offset), "plate x");
            finite(get_float(data, size, point_offset + sizeof(float)), "plate y");
        }
    };
    validate_points(header.printable_area_offset, header.printable_area_count);
    validate_points(header.excluded_area_offset, header.excluded_area_count);

    const auto validate_string = [&](uint64_t offset, uint32_t length) {
        if (offset < header.strings_offset || offset > size || length > size - offset)
            throw std::runtime_error("ORPM material string is outside the string section");
    };
    for (uint64_t i = 0; i < header.material_slot_count; ++i) {
        const uint64_t offset = header.materials_offset + i * MATERIAL_RECORD_SIZE;
        require_reserved_zero(data, size, offset + MATERIAL_PREFIX_RESERVED_OFFSET, MATERIAL_PREFIX_RESERVED_SIZE, "material");
        require_reserved_zero(data, size, offset + MATERIAL_SUFFIX_RESERVED_OFFSET, MATERIAL_SUFFIX_RESERVED_SIZE, "material");
        validate_string(get_le<uint64_t>(data, size, offset + MATERIAL_PRESET_OFFSET),
                        get_le<uint32_t>(data, size, offset + MATERIAL_PRESET_LENGTH_OFFSET));
        validate_string(get_le<uint64_t>(data, size, offset + MATERIAL_NAME_OFFSET),
                        get_le<uint32_t>(data, size, offset + MATERIAL_NAME_LENGTH_OFFSET));
    }

    return header;
}

Header validate(const std::vector<uint8_t>& data, uint64_t max_file_size)
{
    return validate(data.data(), data.size(), max_file_size);
}

void write_atomic(const Snapshot& snapshot, const boost::filesystem::path& target)
{
    namespace fs = boost::filesystem;
    const std::vector<uint8_t> bytes = serialize(snapshot);
    if (!target.parent_path().empty())
        fs::create_directories(target.parent_path());

    fs::path temp = target;
    temp += ".tmp-" + fs::unique_path("%%%%-%%%%-%%%%").string();
    try {
        boost::nowide::ofstream output(temp.string(), std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        output.flush();
        output.close();
        if (!output)
            throw std::runtime_error("Failed to write ORPM snapshot");

#ifdef _WIN32
        const std::error_code error = rename_file(temp.string(), target.string());
        if (error)
            throw std::runtime_error("Failed to publish ORPM snapshot: " + error.message());
#else
        boost::system::error_code error;
        fs::rename(temp, target, error);
        if (error)
            throw std::runtime_error("Failed to publish ORPM snapshot: " + error.message());
#endif
    } catch (...) {
        boost::system::error_code ignored;
        fs::remove(temp, ignored);
        throw;
    }
}

} // namespace Slic3r::PreviewGeometrySnapshot
