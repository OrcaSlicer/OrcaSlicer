#include <catch2/catch_all.hpp>

#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"

using namespace Slic3r;
namespace persistence = Slic3r::AI::SurfaceSelectionPersistence;

namespace {
indexed_triangle_set square()
{
    indexed_triangle_set mesh;
    mesh.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    mesh.indices = {{0, 1, 2}, {1, 3, 2}};
    return mesh;
}
persistence::SelectionState zero_state(size_t faces)
{
    return {std::vector<uint8_t>(faces), std::vector<uint8_t>(faces),
            std::vector<uint8_t>(faces), std::vector<uint8_t>(faces)};
}
void require_same(const persistence::SelectionState& a, const persistence::SelectionState& b)
{
    REQUIRE(a.selected == b.selected);
    REQUIRE(a.protected_faces == b.protected_faces);
    REQUIRE(a.foreground == b.foreground);
    REQUIRE(a.domain == b.domain);
}
const std::string square_fingerprint = "0537362e094bab981ceddf4d6341b427c97c1412526c38eccc3bfd18724af2ab";
}

TEST_CASE("Geometry identity uses a canonical portable face corner encoding", "[SurfaceSelectionState]")
{
    // Golden digest produced independently with Python hashlib and struct's
    // explicit little-endian float32/uint64 formats.
    REQUIRE(persistence::geometry_fingerprint(square()) == square_fingerprint);
}

TEST_CASE("Duplicating coincident material boundary vertices preserves selection identity", "[SurfaceSelectionState]")
{
    const auto original = square();
    indexed_triangle_set split;
    for (const auto& face : original.indices) {
        const int first = int(split.vertices.size());
        for (int corner = 0; corner < 3; ++corner) split.vertices.push_back(original.vertices[face[corner]]);
        split.indices.emplace_back(first, first + 1, first + 2);
    }
    REQUIRE(split.vertices.size() > original.vertices.size());
    REQUIRE(persistence::geometry_fingerprint(split) == persistence::geometry_fingerprint(original));
}

TEST_CASE("Face reorder and geometry changes invalidate selection identity", "[SurfaceSelectionState]")
{
    auto reordered = square();
    std::swap(reordered.indices[0], reordered.indices[1]);
    REQUIRE(persistence::geometry_fingerprint(reordered) != square_fingerprint);
    auto moved = square();
    moved.vertices[0].z() += 0.001f;
    REQUIRE(persistence::geometry_fingerprint(moved) != square_fingerprint);
    auto flipped = square();
    std::swap(flipped.indices[0][0], flipped.indices[0][1]);
    REQUIRE(persistence::geometry_fingerprint(flipped) != square_fingerprint);
}

TEST_CASE("Unused vertices and signed zero do not alter surface identity", "[SurfaceSelectionState]")
{
    auto mesh = square();
    mesh.vertices[0].x() = -0.0f;
    mesh.vertices.emplace_back(99, 99, 99);
    REQUIRE(persistence::geometry_fingerprint(mesh) == square_fingerprint);
}

TEST_CASE("Geometry hashing processes more than one buffered block consistently", "[SurfaceSelectionState]")
{
    const auto original = square();
    indexed_triangle_set shared = original, split;
    shared.indices.clear();
    for (size_t i = 0; i < 1030; ++i) {
        const auto face = original.indices[i % 2];
        shared.indices.push_back(face);
        const int first = int(split.vertices.size());
        for (int corner = 0; corner < 3; ++corner) split.vertices.push_back(original.vertices[face[corner]]);
        split.indices.emplace_back(first, first + 1, first + 2);
    }
    REQUIRE_FALSE(persistence::geometry_fingerprint(shared).empty());
    REQUIRE(persistence::geometry_fingerprint(shared) == persistence::geometry_fingerprint(split));
}

TEST_CASE("Sparse selection runs restore all four masks without inferring their meaning", "[SurfaceSelectionState]")
{
    auto state = zero_state(128);
    for (size_t i = 20; i < 30; ++i) { state.selected[i] = 1; state.domain[i] = 1; }
    state.protected_faces[40] = 1;
    state.foreground[21] = 1;
    const auto doc = persistence::encode(state, 128, square_fingerprint);
    REQUIRE(doc["encoding"] == "rle4");
    persistence::SelectionState restored;
    std::string error;
    REQUIRE(persistence::decode(doc, 128, square_fingerprint, restored, error));
    REQUIRE(error.empty());
    require_same(restored, state);
}

TEST_CASE("Dense mask flags use compact storage and round trip every flag combination", "[SurfaceSelectionState]")
{
    auto state = zero_state(16);
    for (size_t i = 0; i < 16; ++i) {
        state.selected[i] = uint8_t(i & 1);
        state.protected_faces[i] = uint8_t((i >> 1) & 1);
        state.foreground[i] = uint8_t((i >> 2) & 1);
        state.domain[i] = uint8_t((i >> 3) & 1);
    }
    const auto doc = persistence::encode(state, 16, square_fingerprint);
    REQUIRE(doc["encoding"] == "hex4");
    REQUIRE(doc["data"] == "0123456789abcdef");
    persistence::SelectionState restored;
    std::string error;
    REQUIRE(persistence::decode(doc, 16, square_fingerprint, restored, error));
    require_same(restored, state);
}

TEST_CASE("Absent masks restore as zero masks of the actual mesh size", "[SurfaceSelectionState]")
{
    persistence::SelectionState state;
    const auto doc = persistence::encode(state, 2, square_fingerprint);
    persistence::SelectionState restored;
    std::string error;
    REQUIRE(persistence::decode(doc, 2, square_fingerprint, restored, error));
    require_same(restored, zero_state(2));
}

TEST_CASE("Mismatched identities and claimed counts leave an existing selection unchanged", "[SurfaceSelectionState]")
{
    auto state = zero_state(2);
    state.selected[0] = 1;
    const auto doc = persistence::encode(state, 2, square_fingerprint);
    auto restored = state;
    std::string error;
    REQUIRE_FALSE(persistence::decode(doc, 2, std::string(64, '0'), restored, error));
    REQUIRE_FALSE(error.empty());
    require_same(restored, state);
    auto oversized = doc;
    oversized["face_count"] = std::numeric_limits<uint64_t>::max();
    REQUIRE_FALSE(persistence::decode(oversized, 2, square_fingerprint, restored, error));
    require_same(restored, state);
    auto floating = doc;
    floating["face_count"] = 2.0;
    REQUIRE_FALSE(persistence::decode(floating, 2, square_fingerprint, restored, error));
    require_same(restored, state);
}

TEST_CASE("Malformed sparse runs are rejected before replacing existing masks", "[SurfaceSelectionState]")
{
    const std::vector<nlohmann::json> invalid_runs {
        nlohmann::json::array({{0, 3, 1}}),                         // Out of bounds.
        nlohmann::json::array({{0, 2, 1}, {1, 1, 2}}),             // Overlap.
        nlohmann::json::array({{0, 1, 16}}),                       // Unknown bits.
        nlohmann::json::array({{0, 0, 1}}),                        // Empty interval.
        nlohmann::json::array({{-1, 1, 1}}),                       // Negative start.
        nlohmann::json::array({{0, 1.5, 1}}),                      // Noninteger length.
        nlohmann::json::array({{0, std::numeric_limits<uint64_t>::max(), 1}}),
        nlohmann::json::array({{0, 1, 1, 7}}),                     // Wrong structure.
        nlohmann::json::array({{0, 1, 0}})                         // Zero run is noncanonical.
    };
    for (size_t i = 0; i < invalid_runs.size(); ++i) {
        DYNAMIC_SECTION("Malformed run " << i) {
            auto state = zero_state(2);
            state.selected[1] = 1;
            auto doc = persistence::encode({}, 2, square_fingerprint);
            doc["runs"] = invalid_runs[i];
            auto restored = state;
            std::string error;
            REQUIRE_FALSE(persistence::decode(doc, 2, square_fingerprint, restored, error));
            REQUIRE_FALSE(error.empty());
            require_same(restored, state);
        }
    }
}

TEST_CASE("Unknown schemas and malformed dense payloads cannot restore a mask", "[SurfaceSelectionState]")
{
    auto state = zero_state(2);
    state.selected[0] = 1;
    auto doc = persistence::encode(state, 2, square_fingerprint);
    persistence::SelectionState restored;
    std::string error;
    doc["schema"] = "orca.surface-selection/v999";
    REQUIRE_FALSE(persistence::decode(doc, 2, square_fingerprint, restored, error));
    doc["schema"] = "orca.surface-selection/v1";
    doc["data"] = "g0";
    REQUIRE_FALSE(persistence::decode(doc, 2, square_fingerprint, restored, error));
    doc["data"] = "0";
    REQUIRE_FALSE(persistence::decode(doc, 2, square_fingerprint, restored, error));
    REQUIRE_FALSE(persistence::decode(nlohmann::json::array(), 2, square_fingerprint, restored, error));
    REQUIRE(restored.selected.empty());
}

TEST_CASE("Encoding rejects nonbinary masks wrong sizes and missing geometry identity", "[SurfaceSelectionState]")
{
    auto state = zero_state(2);
    state.selected[0] = 2;
    REQUIRE_THROWS_AS(persistence::encode(state, 2, square_fingerprint), std::invalid_argument);
    state = zero_state(3);
    REQUIRE_THROWS_AS(persistence::encode(state, 2, square_fingerprint), std::invalid_argument);
    REQUIRE_THROWS_AS(persistence::encode({}, 2, ""), std::invalid_argument);
}

TEST_CASE("Invalid geometry has no reusable surface identity", "[SurfaceSelectionState]")
{
    auto mesh = square();
    mesh.indices[0][0] = -1;
    REQUIRE(persistence::geometry_fingerprint(mesh).empty());
    mesh = square();
    mesh.vertices[0].x() = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(persistence::geometry_fingerprint(mesh).empty());
}

TEST_CASE("Explicit local face colors round trip independently of selection masks", "[SurfaceSelectionState]")
{
    const persistence::FaceColorOverrides colors {{1, {0.1f, 0.2f, 0.9f}}, {0, {1, 0, 0}}};
    const auto doc = persistence::encode_colors(colors, 2, square_fingerprint);
    REQUIRE(doc["schema"] == "orca.local-face-colors/v1");
    persistence::FaceColorOverrides restored;
    std::string error;
    // Exercise actual JSON text storage as used by artifact metadata.
    REQUIRE(persistence::decode_colors(nlohmann::json::parse(doc.dump()), 2, square_fingerprint, restored, error));
    REQUIRE(error.empty());
    auto sorted = colors;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    REQUIRE(restored == sorted);
}

TEST_CASE("Empty local color intent is a valid persisted state", "[SurfaceSelectionState]")
{
    const auto doc = persistence::encode_colors({}, 2, square_fingerprint);
    persistence::FaceColorOverrides restored {{0, {1, 0, 0}}};
    std::string error;
    REQUIRE(persistence::decode_colors(doc, 2, square_fingerprint, restored, error));
    REQUIRE(restored.empty());
    REQUIRE(error.empty());
}

TEST_CASE("Local color geometry and claimed counts are validated before restoring entries", "[SurfaceSelectionState]")
{
    const persistence::FaceColorOverrides colors {{0, {1, 0, 0}}};
    auto doc = persistence::encode_colors(colors, 2, square_fingerprint);
    auto restored = colors;
    std::string error;
    REQUIRE_FALSE(persistence::decode_colors(doc, 2, std::string(64, '0'), restored, error));
    REQUIRE(restored == colors);
    doc["face_count"] = std::numeric_limits<uint64_t>::max();
    REQUIRE_FALSE(persistence::decode_colors(doc, 2, square_fingerprint, restored, error));
    REQUIRE(restored == colors);
    doc["face_count"] = 2;
    doc["schema"] = "orca.local-face-colors/v2";
    REQUIRE_FALSE(persistence::decode_colors(doc, 2, square_fingerprint, restored, error));
    REQUIRE(restored == colors);
    REQUIRE_FALSE(persistence::decode_colors(nlohmann::json::array(), 2, square_fingerprint, restored, error));
    REQUIRE(restored == colors);
}

TEST_CASE("Invalid local color entries cannot partially replace saved overrides", "[SurfaceSelectionState]")
{
    const std::vector<nlohmann::json> bad_entries {
        nlohmann::json::array({0, 1, 0, 0}),       // Duplicate after valid face 0.
        nlohmann::json::array({2, 1, 0, 0}),       // Outside the loaded mesh.
        nlohmann::json::array({-1, 1, 0, 0}),
        nlohmann::json::array({1.0, 1, 0, 0}),     // Face identifiers are integers.
        nlohmann::json::array({1, -0.1, 0, 0}),
        nlohmann::json::array({1, 1.1, 0, 0}),
        nlohmann::json::array({1, "0.5", 0, 0}),
        nlohmann::json::array({1, true, 0, 0}),
        nlohmann::json::array({1, std::numeric_limits<double>::infinity(), 0, 0}),
        nlohmann::json::array({1, std::numeric_limits<double>::quiet_NaN(), 0, 0}),
        nlohmann::json::array({1, 1, 0}),
        nlohmann::json::array({1, 1, 0, 0, 1})
    };
    for (size_t i = 0; i < bad_entries.size(); ++i) {
        DYNAMIC_SECTION("Invalid color entry " << i) {
            const persistence::FaceColorOverrides colors {{1, {0, 0, 1}}};
            auto restored = colors;
            auto doc = persistence::encode_colors({{0, {1, 0, 0}}}, 2, square_fingerprint);
            doc["colors"].push_back(bad_entries[i]);
            std::string error;
            REQUIRE_FALSE(persistence::decode_colors(doc, 2, square_fingerprint, restored, error));
            REQUIRE_FALSE(error.empty());
            REQUIRE(restored == colors);
        }
    }
}

TEST_CASE("Excess local color entries and invalid outgoing colors are rejected", "[SurfaceSelectionState]")
{
    const persistence::FaceColorOverrides duplicate {{0, {1, 0, 0}}, {0, {0, 0, 1}}};
    REQUIRE_THROWS_AS(persistence::encode_colors(duplicate, 2, square_fingerprint), std::invalid_argument);
    REQUIRE_THROWS_AS(persistence::encode_colors({{2, {1, 0, 0}}}, 2, square_fingerprint), std::invalid_argument);
    REQUIRE_THROWS_AS(persistence::encode_colors({{0, {1.1f, 0, 0}}}, 2, square_fingerprint), std::invalid_argument);
    REQUIRE_THROWS_AS(persistence::encode_colors({{0, {std::numeric_limits<float>::quiet_NaN(), 0, 0}}}, 2, square_fingerprint), std::invalid_argument);
    REQUIRE_THROWS_AS(persistence::encode_colors({}, 2, ""), std::invalid_argument);
    auto doc = persistence::encode_colors({}, 2, square_fingerprint);
    doc["colors"] = nlohmann::json::array({{0, 1, 0, 0}, {1, 0, 0, 1}, {0, 0, 1, 0}});
    persistence::FaceColorOverrides restored;
    std::string error;
    REQUIRE_FALSE(persistence::decode_colors(doc, 2, square_fingerprint, restored, error));
    REQUIRE(restored.empty());
}

TEST_CASE("A uniformly painted surface stores a compact color run instead of one JSON row per face", "[SurfaceSelectionState]")
{
    persistence::FaceColorOverrides colors;
    for (size_t i = 0; i < 10000; ++i) colors.push_back({i, {0, 0, 1}});
    const auto doc = persistence::encode_colors(colors, 10000, square_fingerprint);
    REQUIRE(doc["encoding"] == "runs");
    REQUIRE(doc["colors"].size() == 1);
    REQUIRE(doc.dump().size() < 512);
    persistence::FaceColorOverrides restored;
    std::string error;
    REQUIRE(persistence::decode_colors(doc, 10000, square_fingerprint, restored, error));
    REQUIRE(restored == colors);
}

TEST_CASE("Color runs preserve gaps and boundaries between different target colors", "[SurfaceSelectionState]")
{
    const persistence::FaceColorOverrides colors {
        {0, {0, 0, 1}}, {1, {0, 0, 1}}, {2, {0, 0, 1}},
        {4, {0, 0, 1}}, {5, {0, 0, 1}}, {6, {0, 0, 1}},
        {7, {1, 0, 0}}, {8, {1, 0, 0}}, {9, {1, 0, 0}}
    };
    const auto doc = persistence::encode_colors(colors, 10, square_fingerprint);
    REQUIRE(doc["encoding"] == "runs");
    REQUIRE(doc["colors"].size() == 3);
    persistence::FaceColorOverrides restored;
    std::string error;
    REQUIRE(persistence::decode_colors(doc, 10, square_fingerprint, restored, error));
    REQUIRE(restored == colors);
}

TEST_CASE("Overlapping overflowing and malformed color runs cannot change existing colors", "[SurfaceSelectionState]")
{
    const std::vector<nlohmann::json> runs {
        nlohmann::json::array({{0, 2, 1, 0, 0}, {1, 1, 0, 0, 1}}),
        nlohmann::json::array({{0, 3, 1, 0, 0}}),
        nlohmann::json::array({{0, std::numeric_limits<uint64_t>::max(), 1, 0, 0}}),
        nlohmann::json::array({{0, 0, 1, 0, 0}}),
        nlohmann::json::array({{0, -1, 1, 0, 0}}),
        nlohmann::json::array({{0, 1.5, 1, 0, 0}}),
        nlohmann::json::array({{0, 1, 1, 0}}),
        nlohmann::json::array({{0, 1, 2, 0, 0}})
    };
    for (size_t i = 0; i < runs.size(); ++i) {
        DYNAMIC_SECTION("Invalid color run " << i) {
            const persistence::FaceColorOverrides colors {{0, {0, 0, 1}}};
            auto doc = persistence::encode_colors(colors, 2, square_fingerprint);
            doc["encoding"] = "runs";
            doc["colors"] = runs[i];
            auto restored = colors;
            std::string error;
            REQUIRE_FALSE(persistence::decode_colors(doc, 2, square_fingerprint, restored, error));
            REQUIRE(restored == colors);
        }
    }
}

TEST_CASE("Legacy explicit color entries restore while unknown encodings are rejected", "[SurfaceSelectionState]")
{
    const persistence::FaceColorOverrides colors {{0, {0, 0, 1}}};
    auto doc = persistence::encode_colors(colors, 2, square_fingerprint);
    doc.erase("encoding");
    persistence::FaceColorOverrides restored;
    std::string error;
    REQUIRE(persistence::decode_colors(doc, 2, square_fingerprint, restored, error));
    REQUIRE(restored == colors);
    doc["encoding"] = "future";
    REQUIRE_FALSE(persistence::decode_colors(doc, 2, square_fingerprint, restored, error));
    REQUIRE(restored == colors);
}
