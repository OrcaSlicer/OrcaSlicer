#include <catch2/catch_all.hpp>

#include <slic3r/GUI/ToolpathMeshBuilder.hpp>
#include <slic3r/plugin/host/PreviewGeometrySnapshot.hpp>
#include <libslic3r/GCode/GCodeProcessor.hpp>
#include <libvgcode/include/GCodeInputData.hpp>
#include <libvgcode/include/Viewer.hpp>

#include "test_utils.hpp"

#include <boost/filesystem.hpp>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

using namespace Slic3r;
namespace fs = boost::filesystem;
namespace ORPM = PreviewGeometrySnapshot;

namespace {
ORPM::Snapshot sample_snapshot()
{
    ORPM::Snapshot s; s.scene_id=42;s.plate_index=3;s.printable_height=250;s.bounds_min={{0,0,0}};s.bounds_max={{1,1,1}};
    s.vertices={{{0,0,0},{0,0,1},{0,0}},{{1,0,0},{0,0,1},{1,0}},{{1,1,0},{0,0,1},{1,1}},{{0,1,0},{0,0,1},{0,1}}};
    s.indices={0,1,2,0,2,3};s.groups={{0,6,0,1,0,0,0}};s.materials={{0,{{10,20,30,255}},"preset","Preset"}};
    s.printable_area={{0,0},{200,0},{200,200},{0,200}};return s;
}
template<class T> void put_le(std::vector<uint8_t>& data, size_t offset, T value)
{
    using U = std::make_unsigned_t<T>;
    const U encoded = static_cast<U>(value);
    for (size_t i = 0; i < sizeof(T); ++i)
        data[offset + i] = uint8_t(encoded >> (8 * i));
}

template<class T> T get_le(const std::vector<uint8_t>& data, size_t offset)
{
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

std::vector<uint8_t> read_file(const fs::path& path)
{
    std::ifstream input(path.string(), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool has_snapshot_temp(const fs::path& directory, const std::string& target_name)
{
    const std::string prefix = target_name + ".tmp-";
    for (const fs::directory_entry& entry : fs::directory_iterator(directory))
        if (entry.path().filename().string().compare(0, prefix.size(), prefix) == 0)
            return true;
    return false;
}
}

TEST_CASE("ORPM serializes authoritative indexed mesh", "[PreviewGeometrySnapshot]")
{
    const auto bytes=ORPM::serialize(sample_snapshot());const auto h=ORPM::validate(bytes);
    CHECK(h.major_version==1);CHECK(h.header_size==256);CHECK(h.vertex_count==4);CHECK(h.index_count==6);
    CHECK(h.group_count==1);CHECK(h.material_slot_count==1);CHECK(h.file_size==bytes.size());
}

TEST_CASE("ORPM rejects malformed mesh", "[PreviewGeometrySnapshot]")
{
    const std::vector<uint8_t> valid = ORPM::serialize(sample_snapshot());

    SECTION("bad magic")
    {
        auto data = valid;
        data[0] = 'X';
        CHECK_THROWS(ORPM::validate(data));
    }
    SECTION("bad major version")
    {
        auto data = valid;
        put_le<uint16_t>(data, 4, ORPM::FORMAT_MAJOR + 1);
        CHECK_THROWS(ORPM::validate(data));
    }
    SECTION("zero required count")
    {
        auto data = valid;
        put_le<uint64_t>(data, 40, 0);
        CHECK_THROWS(ORPM::validate(data));
    }
    SECTION("out of range section offset")
    {
        auto data = valid;
        put_le<uint64_t>(data, 80, data.size() + 1);
        CHECK_THROWS(ORPM::validate(data));
    }
    SECTION("overlapping sections")
    {
        auto data = valid;
        put_le<uint64_t>(data, 88, get_le<uint64_t>(data, 80));
        CHECK_THROWS(ORPM::validate(data));
    }
    SECTION("index")
    {
        auto data = valid;
        put_le<uint32_t>(data, ORPM::HEADER_BYTE_SIZE + 4 * ORPM::VERTEX_RECORD_SIZE, 99);
        CHECK_THROWS(ORPM::validate(data));
    }
    SECTION("group coverage")
    {
        auto data = valid;
        const uint64_t groups_offset = get_le<uint64_t>(data, 96);
        put_le<uint32_t>(data, groups_offset + 4, 3);
        CHECK_THROWS(ORPM::validate(data));
    }
    SECTION("bad normal")
    {
        auto data = valid;
        put_float(data, ORPM::HEADER_BYTE_SIZE + 20, 0.0f);
        CHECK_THROWS(ORPM::validate(data));
    }
    SECTION("non-finite vertex")
    {
        auto data = valid;
        put_le<uint32_t>(data, ORPM::HEADER_BYTE_SIZE, 0x7fc00000);
        CHECK_THROWS(ORPM::validate(data));
    }
    SECTION("inverted bounds")
    {
        auto data = valid;
        put_float(data, 152, 2.0f);
        CHECK_THROWS(ORPM::validate(data));
    }
    SECTION("material string outside string section")
    {
        auto data = valid;
        const uint64_t materials_offset = get_le<uint64_t>(data, 104);
        const uint64_t strings_offset = get_le<uint64_t>(data, 128);
        put_le<uint64_t>(data, materials_offset + 8, strings_offset - 1);
        CHECK_THROWS(ORPM::validate(data));
    }
    SECTION("missing indexed flag")
    {
        auto data = valid;
        put_le<uint32_t>(data, 36, 0);
        CHECK_THROWS(ORPM::validate(data));
    }
    SECTION("non-zero reserved header bytes")
    {
        auto data = valid;
        data[176] = 1;
        CHECK_THROWS(ORPM::validate(data));
    }
    SECTION("non-zero reserved group bytes")
    {
        auto data = valid;
        const uint64_t groups_offset = get_le<uint64_t>(data, 96);
        data[groups_offset + 13] = 1;
        CHECK_THROWS(ORPM::validate(data));
    }
    SECTION("non-zero reserved material bytes")
    {
        auto data = valid;
        const uint64_t materials_offset = get_le<uint64_t>(data, 104);
        data[materials_offset + 5] = 1;
        CHECK_THROWS(ORPM::validate(data));
    }
    SECTION("non-finite printable point")
    {
        auto data = valid;
        const uint64_t printable_offset = get_le<uint64_t>(data, 112);
        put_le<uint32_t>(data, printable_offset, 0x7fc00000);
        CHECK_THROWS(ORPM::validate(data));
    }
    SECTION("file limit")
    {
        CHECK_THROWS(ORPM::validate(valid, 3));
    }
}

TEST_CASE("ORPM capture preserves shared preview mesh and result metadata", "[PreviewGeometrySnapshot]")
{
    GUI::PreviewTriangleMesh mesh;mesh.vertices={{{0,0,0},{0,0,1},{0,0}},{{1,0,0},{0,0,1},{0,0}},{{0,1,0},{0,0,1},{0,0}}};mesh.indices={0,1,2};mesh.groups={{0,3,0,1,0,0,4,{255,0,0}}};for(const auto&v:mesh.vertices)mesh.bounds.merge(v.position.cast<double>());
    GCodeProcessorResult result;result.id=77;result.printable_height=256;result.filaments_count=1;result.extruder_colors={"#123456"};result.settings_ids.filament={"filament"};
    const Pointfs plate={{0,0},{200,0},{200,200},{0,200}};
    const auto snapshot=ORPM::capture(mesh,result,2,77,plate);CHECK(snapshot.scene_id==77);CHECK(snapshot.vertices.size()==3);CHECK(snapshot.indices==mesh.indices);CHECK(snapshot.groups.size()==1);CHECK(snapshot.materials.size()==1);CHECK(snapshot.printable_area.size()==plate.size());CHECK_THROWS(ORPM::capture(mesh,result,2,76));
}

TEST_CASE("shared libvgcode mesh builder emits Orca caps and no travel", "[PreviewGeometrySnapshot]")
{
    libvgcode::GCodeInputData data;data.tools_colors.push_back({255,0,0});
    libvgcode::PathVertex start;start.position={0,0,0.2f};start.width=0.45f;start.height=0.2f;start.type=libvgcode::EMoveType::Extrude;start.role=libvgcode::EGCodeExtrusionRole::Perimeter;start.gcode_id=5;
    auto duplicate=start;
    auto end=start;end.position={10,0,0.2f};
    auto travel=end;travel.position={20,0,0.2f};travel.type=libvgcode::EMoveType::Travel;travel.gcode_id=6;
    data.vertices={start,duplicate,end,travel};
    const auto mesh=GUI::build_preview_triangle_mesh(data);CHECK(mesh.vertices.size()==10);CHECK(mesh.indices.size()==48);CHECK(mesh.groups.size()>=1);CHECK(mesh.bounds.min.x()<0);CHECK(mesh.bounds.max.x()>10);
}

TEST_CASE("ORPM snapshots replace an existing target atomically", "[PreviewGeometrySnapshot]")
{
    ScopedTemporaryDir temp("orpm-atomic");
    const fs::path target = temp.path() / "scene.orpm";
    const fs::path legacy_temp = target.string() + ".tmp";
    {
        std::ofstream sentinel(legacy_temp.string());
        sentinel << "keep";
    }

    ORPM::write_atomic(sample_snapshot(), target);
    REQUIRE(fs::exists(target));
    CHECK(ORPM::validate(read_file(target)).scene_id == 42);

    ORPM::Snapshot replacement = sample_snapshot();
    replacement.scene_id = 43;
    ORPM::write_atomic(replacement, target);
    CHECK(ORPM::validate(read_file(target)).scene_id == 43);
    CHECK(fs::exists(legacy_temp));
    CHECK_FALSE(has_snapshot_temp(temp.path(), target.filename().string()));
}

TEST_CASE("ORPM snapshot publication cleans a unique temporary file after failure", "[PreviewGeometrySnapshot]")
{
    ScopedTemporaryDir temp("orpm-atomic-failure");
    const fs::path target = temp.path() / "scene.orpm";
    fs::create_directory(target);

    CHECK_THROWS(ORPM::write_atomic(sample_snapshot(), target));
    CHECK(fs::is_directory(target));
    CHECK_FALSE(has_snapshot_temp(temp.path(), target.filename().string()));
}
