#include <catch2/catch_all.hpp>

#include <slic3r/GUI/ToolpathMeshBuilder.hpp>
#include <slic3r/plugin/host/PreviewGeometrySnapshot.hpp>
#include <libslic3r/GCode/GCodeProcessor.hpp>
#include <libvgcode/include/GCodeInputData.hpp>
#include <libvgcode/include/Viewer.hpp>
#include <nlohmann/json.hpp>

#include "test_utils.hpp"

#include <boost/filesystem.hpp>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>
#include <vector>

using namespace Slic3r;
namespace fs = boost::filesystem;
namespace Preview = PreviewGeometrySnapshot;

namespace {
Preview::Snapshot sample_snapshot()
{
    Preview::Snapshot snapshot;
    snapshot.scene_id         = 42;
    snapshot.plate_index      = 3;
    snapshot.printable_height = 250;
    snapshot.bounds_min       = {{0, 0, 0}};
    snapshot.bounds_max       = {{1, 1, 1}};
    snapshot.vertices         = {{{0, 0, 0}, {0, 0, 1}, {0, 0}},
                                 {{1, 0, 0}, {0, 0, 1}, {1, 0}},
                                 {{1, 1, 0}, {0, 0, 1}, {1, 1}},
                                 {{0, 1, 0}, {0, 0, 1}, {0, 1}}};
    snapshot.indices          = {0, 1, 2, 0, 2, 3};
    snapshot.groups           = {{0, 6, 0, 1, 0, 0, 7}};
    snapshot.materials        = {{0, {{10, 20, 30, 255}}, "preset", "Preset"}};
    snapshot.printable_area   = {{0, 0}, {200, 0}, {200, 200}, {0, 200}};
    return snapshot;
}

template<class T> T get_le(const std::vector<uint8_t>& data, size_t offset)
{
    using U = std::make_unsigned_t<T>;
    U value{};
    for (size_t i = 0; i < sizeof(T); ++i)
        value |= U(data[offset + i]) << (8 * i);
    return static_cast<T>(value);
}

nlohmann::json glb_json(const std::vector<uint8_t>& data)
{
    const uint32_t size = get_le<uint32_t>(data, 12);
    return nlohmann::json::parse(data.begin() + 20, data.begin() + 20 + size);
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
} // namespace

TEST_CASE("preview scene serializes as standard GLB 2.0", "[PreviewGeometrySnapshot]")
{
    const auto bytes = Preview::serialize(sample_snapshot());
    const auto info = Preview::validate(bytes);
    const auto document = glb_json(bytes);

    CHECK(get_le<uint32_t>(bytes, 0) == 0x46546c67);
    CHECK(info.version == 2);
    CHECK(info.file_size == bytes.size());
    CHECK(info.scene_id == 42);
    CHECK(info.vertex_count == 4);
    CHECK(info.index_count == 6);
    CHECK(info.primitive_count == 1);
    CHECK(info.material_count == 1);
    CHECK(document["asset"]["version"] == "2.0");
    CHECK(document["nodes"][0]["rotation"].size() == 4);
    CHECK(document["meshes"][0]["primitives"][0]["extras"]["orca"]["layerId"] == 7);
    CHECK(document["meshes"][0]["primitives"][1]["mode"] == 2);
    CHECK(document["meshes"][0]["primitives"][1]["extras"]["orca"]["areaKind"] == "printable");
}

TEST_CASE("preview capture preserves shared mesh and result metadata", "[PreviewGeometrySnapshot]")
{
    GUI::PreviewTriangleMesh mesh;
    mesh.vertices = {{{0, 0, 0}, {0, 0, 1}, {0, 0}},
                     {{1, 0, 0}, {0, 0, 1}, {0, 0}},
                     {{0, 1, 0}, {0, 0, 1}, {0, 0}}};
    mesh.indices = {0, 1, 2};
    mesh.groups  = {{0, 3, 0, 1, 0, 0, 4, {255, 0, 0}}};
    for (const auto& vertex : mesh.vertices)
        mesh.bounds.merge(vertex.position.cast<double>());

    GCodeProcessorResult result;
    result.id                    = 77;
    result.printable_height      = 256;
    result.filaments_count       = 1;
    result.extruder_colors       = {"#123456"};
    result.settings_ids.filament = {"filament"};

    const Pointfs plate = {{0, 0}, {200, 0}, {200, 200}, {0, 200}};
    const auto snapshot = Preview::capture(mesh, result, 2, 77, plate);

    CHECK(snapshot.scene_id == 77);
    CHECK(snapshot.vertices.size() == 3);
    CHECK(snapshot.indices == mesh.indices);
    CHECK(snapshot.groups.size() == 1);
    CHECK(snapshot.materials.size() == 1);
    CHECK(snapshot.printable_area.size() == plate.size());
    CHECK_THROWS(Preview::capture(mesh, result, 2, 76));
}

TEST_CASE("shared libvgcode mesh builder emits Orca caps and no travel", "[PreviewGeometrySnapshot]")
{
    libvgcode::GCodeInputData data;
    data.tools_colors.push_back({255, 0, 0});
    libvgcode::PathVertex start;
    start.position = {0, 0, 0.2f};
    start.width = 0.45f;
    start.height = 0.2f;
    start.type = libvgcode::EMoveType::Extrude;
    start.role = libvgcode::EGCodeExtrusionRole::Perimeter;
    start.gcode_id = 5;
    auto duplicate = start;
    auto end = start;
    end.position = {10, 0, 0.2f};
    auto travel = end;
    travel.position = {20, 0, 0.2f};
    travel.type = libvgcode::EMoveType::Travel;
    travel.gcode_id = 6;
    data.vertices = {start, duplicate, end, travel};

    const auto mesh = GUI::build_preview_triangle_mesh(data);
    CHECK(mesh.vertices.size() == 10);
    CHECK(mesh.indices.size() == 48);
    CHECK(mesh.groups.size() >= 1);
    CHECK(mesh.bounds.min.x() < 0);
    CHECK(mesh.bounds.max.x() > 10);
}

TEST_CASE("GLB previews replace an existing target atomically", "[PreviewGeometrySnapshot]")
{
    ScopedTemporaryDir temp("glb-atomic");
    const fs::path target = temp.path() / "scene.glb";
    const fs::path legacy_temp = target.string() + ".tmp";
    {
        std::ofstream sentinel(legacy_temp.string());
        sentinel << "keep";
    }

    Preview::write_atomic(sample_snapshot(), target);
    REQUIRE(fs::exists(target));
    CHECK(Preview::validate(read_file(target)).scene_id == 42);
    Preview::Snapshot replacement = sample_snapshot();
    replacement.scene_id = 43;
    Preview::write_atomic(replacement, target);
    CHECK(Preview::validate(read_file(target)).scene_id == 43);
    CHECK(fs::exists(legacy_temp));
    CHECK_FALSE(has_snapshot_temp(temp.path(), target.filename().string()));
}

TEST_CASE("GLB preview publication cleans a unique temporary file after failure", "[PreviewGeometrySnapshot]")
{
    ScopedTemporaryDir temp("glb-atomic-failure");
    const fs::path target = temp.path() / "scene.glb";
    fs::create_directory(target);

    CHECK_THROWS(Preview::write_atomic(sample_snapshot(), target));
    CHECK(fs::is_directory(target));
    CHECK_FALSE(has_snapshot_temp(temp.path(), target.filename().string()));
}
