#include <catch2/catch_all.hpp>

#include <slic3r/plugin/host/PreviewGeometrySnapshot.hpp>
#include <libslic3r/GCode/GCodeProcessor.hpp>

#include "test_utils.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <type_traits>
#include <vector>

using namespace Slic3r;
namespace fs = boost::filesystem;
namespace ORPV = PreviewGeometrySnapshot;

namespace {

ORPV::Snapshot sample_snapshot()
{
    ORPV::Snapshot snapshot;
    snapshot.scene_id = 42;
    snapshot.plate_index = 3;
    snapshot.printable_height = 250.0f;
    snapshot.vertices = {
        {1.0f, 2.0f, 0.2f, 0.4f, 0.2f, 10, 1, static_cast<uint8_t>(EMoveType::Extrude),
         static_cast<uint8_t>(erPerimeter), 0, 7, ORPV::PathStart},
        {2.0f, 2.0f, 0.2f, 0.4f, 0.2f, 11, 1, static_cast<uint8_t>(EMoveType::Extrude),
         static_cast<uint8_t>(erPerimeter), 0, 7, ORPV::PathEnd},
    };
    snapshot.materials = {{0, {{10, 20, 30, 255}}, "preset-id", "Preset Name"}};
    snapshot.printable_area = {{0.0f, 0.0f}, {200.0f, 0.0f}, {200.0f, 200.0f}, {0.0f, 200.0f}};
    snapshot.bed_excluded_area = {{5.0f, 5.0f}};
    snapshot.wrapping_excluded_area = {{6.0f, 6.0f}};
    return snapshot;
}

template<typename T> void put_le(std::vector<uint8_t>& data, size_t offset, T value)
{
    using U = std::make_unsigned_t<T>;
    const U bits = static_cast<U>(value);
    for (size_t i = 0; i < sizeof(T); ++i)
        data[offset + i] = static_cast<uint8_t>(bits >> (8 * i));
}

void put_float(std::vector<uint8_t>& data, size_t offset, float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    put_le(data, offset, bits);
}

} // namespace

TEST_CASE("ORPV v1 serializes a bounded versioned snapshot", "[PreviewGeometrySnapshot]")
{
    const std::vector<uint8_t> data = ORPV::serialize(sample_snapshot());
    const ORPV::Header header = ORPV::validate(data);

    CHECK(header.major_version == ORPV::FORMAT_MAJOR);
    CHECK(header.minor_version == ORPV::FORMAT_MINOR);
    CHECK(header.scene_id == 42);
    CHECK(header.plate_index == 3);
    CHECK(header.vertex_count == 2);
    CHECK(header.material_slot_count == 1);
    CHECK(header.printable_area_count == 4);
    CHECK(header.excluded_area_count == 2);
    CHECK(header.bed_excluded_area_count == 1);
    CHECK(header.file_size == data.size());
}

TEST_CASE("ORPV validation rejects malformed and unbounded input", "[PreviewGeometrySnapshot]")
{
    const std::vector<uint8_t> valid = ORPV::serialize(sample_snapshot());

    SECTION("truncation") {
        std::vector<uint8_t> data(valid.begin(), valid.end() - 1);
        CHECK_THROWS(ORPV::validate(data));
    }
    SECTION("magic") {
        auto data = valid;
        data[0] = 'X';
        CHECK_THROWS(ORPV::validate(data));
    }
    SECTION("unknown major") {
        auto data = valid;
        put_le<uint16_t>(data, 4, ORPV::FORMAT_MAJOR + 1);
        CHECK_THROWS(ORPV::validate(data));
    }
    SECTION("unknown additive minor") {
        auto data = valid;
        put_le<uint16_t>(data, 6, ORPV::FORMAT_MINOR + 1);
        CHECK_NOTHROW(ORPV::validate(data));
    }
    SECTION("count bound") {
        auto data = valid;
        put_le<uint64_t>(data, 40, std::numeric_limits<uint64_t>::max());
        CHECK_THROWS(ORPV::validate(data));
    }
    SECTION("non-finite extrusion width") {
        auto data = valid;
        put_float(data, ORPV::HEADER_BYTE_SIZE + 12, std::numeric_limits<float>::quiet_NaN());
        CHECK_THROWS(ORPV::validate(data));
    }
    SECTION("overlapping sections") {
        auto data = valid;
        put_le<uint64_t>(data, 72, ORPV::HEADER_BYTE_SIZE);
        CHECK_THROWS(ORPV::validate(data));
    }
    SECTION("configurable file bound") {
        CHECK_THROWS(ORPV::validate(valid, ORPV::DEFAULT_MAX_VERTICES, valid.size() - 1));
    }
}

TEST_CASE("ORPV capture preserves final move identity and path boundaries", "[PreviewGeometrySnapshot]")
{
    GCodeProcessorResult result;
    result.id = 77;
    result.printable_height = 256.0f;
    result.filaments_count = 1;
    result.extruder_colors = {"#123456"};
    result.settings_ids.filament = {"filament-preset"};

    GCodeProcessorResult::MoveVertex travel;
    travel.type = EMoveType::Travel;
    travel.position = Vec3f(0.0f, 2.0f, 0.2f);

    GCodeProcessorResult::MoveVertex first;
    first.type = EMoveType::Extrude;
    first.extrusion_role = erPerimeter;
    first.extruder_id = 0;
    first.cp_color_id = 9;
    first.position = Vec3f(1.0f, 2.0f, 0.2f);
    first.width = 0.45f;
    first.height = 0.2f;
    first.gcode_id = 100;
    first.layer_id = 5;
    GCodeProcessorResult::MoveVertex second = first;
    second.position.x() = 2.0f;
    second.gcode_id = 101;
    second.acceleration = first.acceleration + 100.0f;
    GCodeProcessorResult::MoveVertex distant_travel = travel;
    distant_travel.position.x() = 100.0f;
    GCodeProcessorResult::MoveVertex third = first;
    third.position.x() = 101.0f;
    third.gcode_id = 102;
    result.moves = {travel, first, second, distant_travel, third};

    const ORPV::Snapshot snapshot = ORPV::capture(result, 4, 77);
    REQUIRE(snapshot.vertices.size() == 5);
    CHECK(snapshot.scene_id == 77);
    CHECK(snapshot.plate_index == 4);
    CHECK(snapshot.vertices.front().gcode_id == 100);
    CHECK(snapshot.vertices.front().colour_id == 9);
    CHECK(snapshot.vertices.front().x == 0.0f);
    CHECK(snapshot.vertices[2].x == 2.0f);
    CHECK(snapshot.vertices[3].x == 100.0f);
    CHECK(snapshot.vertices.back().x == 101.0f);
    CHECK(std::all_of(snapshot.vertices.begin(), snapshot.vertices.end(), [](const ORPV::Vertex& vertex) {
        return vertex.move_type == static_cast<uint8_t>(EMoveType::Extrude);
    }));
    CHECK((snapshot.vertices.front().path_flags & ORPV::PathStart) != 0);
    CHECK((snapshot.vertices[2].path_flags & ORPV::PathEnd) != 0);
    CHECK((snapshot.vertices[3].path_flags & ORPV::PathStart) != 0);
    CHECK((snapshot.vertices.back().path_flags & ORPV::PathEnd) != 0);
    REQUIRE(snapshot.materials.size() == 1);
    CHECK(snapshot.materials.front().preset_id == "filament-preset");
    CHECK_THROWS(ORPV::capture(result, 4, 76));
}

TEST_CASE("ORPV snapshots publish atomically", "[PreviewGeometrySnapshot]")
{
    ScopedTemporaryDir temp("orpv-atomic");
    const fs::path target = temp.path() / "plate-3-scene-42.orpv";

    ORPV::write_atomic(sample_snapshot(), target);
    REQUIRE(fs::exists(target));

    std::ifstream input(target.string(), std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK(ORPV::validate(data).scene_id == 42);

    const fs::path parent = target.parent_path();
    for (fs::directory_iterator it(parent), end; it != end; ++it)
        CHECK(it->path().extension() == ".orpv");
}
