#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Point.hpp"

#include <algorithm>
#include "libslic3r/libslic3r.h"
#include <vector>
#include <cstddef>
#include <string>
#include <functional>
#include <memory>
#include <utility>

using namespace Slic3r;

// A sphere gives well over ExtruderMax original facets, so every extruder state can be assigned
// to a facet of its own without any splitting getting in the way.
static TriangleMesh test_mesh() { return make_sphere(5., 2 * PI / 24); }

// Read the nibble_idx-th 4-bit group of a serialized bitstream, least significant bit first.
static int nibble_at(const std::vector<bool> &bitstream, size_t nibble_idx)
{
    int n = 0;
    for (size_t bit = 0; bit < 4; ++bit)
        n |= int(bitstream[nibble_idx * 4 + bit]) << bit;
    return n;
}

TEST_CASE("Every extruder state survives a serialize/deserialize round trip", "[TriangleSelector]")
{
    const TriangleMesh mesh      = test_mesh();
    const int          max_state = int(EnforcerBlockerType::ExtruderMax);
    REQUIRE(int(mesh.its.indices.size()) >= max_state);

    TriangleSelector selector(mesh);
    for (int state = 1; state <= max_state; ++state)
        selector.set_facet(state - 1, EnforcerBlockerType(state));

    TriangleSelector restored(mesh);
    restored.deserialize(selector.serialize());

    for (int state = 1; state <= max_state; ++state) {
        INFO("Extruder " << state);
        REQUIRE(restored.has_facets(EnforcerBlockerType(state)));
        REQUIRE(restored.num_facets(EnforcerBlockerType(state)) == 1);
    }
}

TEST_CASE("Serialized data reports the extruder states it uses", "[TriangleSelector]")
{
    const TriangleMesh mesh = test_mesh();
    TriangleSelector   selector(mesh);
    selector.set_facet(0, EnforcerBlockerType::Extruder16);
    selector.set_facet(1, EnforcerBlockerType::Extruder32);

    const TriangleSelector::TriangleSplittingData data = selector.serialize();

    REQUIRE(data.used_states.size() == size_t(EnforcerBlockerType::ExtruderMax) + 1);
    REQUIRE(data.used_states[size_t(EnforcerBlockerType::Extruder16)]);
    REQUIRE(data.used_states[size_t(EnforcerBlockerType::Extruder32)]);
    REQUIRE_FALSE(data.used_states[size_t(EnforcerBlockerType::Extruder17)]);

    SECTION("used_states recomputed from the bitstream agrees") {
        TriangleSelector::TriangleSplittingData recomputed = data;
        recomputed.reset_used_states();
        recomputed.update_used_states(0);
        REQUIRE(recomputed.used_states == data.used_states);
    }

    SECTION("has_facets on the raw data agrees") {
        REQUIRE(TriangleSelector::has_facets(data, EnforcerBlockerType::Extruder32));
        REQUIRE_FALSE(TriangleSelector::has_facets(data, EnforcerBlockerType::Extruder17));
    }
}

// States 3..17 must keep the pre-existing encoding ("11" prefix plus one nibble of state-3) so
// projects written by older builds stay readable and newly written ones stay readable by them.
TEST_CASE("Extruder states up to 17 keep the single-nibble encoding", "[TriangleSelector]")
{
    const int state = GENERATE(3, 8, 16, 17);

    TriangleSelector selector(test_mesh());
    selector.set_facet(0, EnforcerBlockerType(state));
    const std::vector<bool> bitstream = selector.serialize().bitstream;

    INFO("Extruder " << state);
    // Two nibbles: the "11"-prefixed leaf code, then the state itself.
    REQUIRE(bitstream.size() == 8);
    REQUIRE(nibble_at(bitstream, 0) == 0b1100);
    REQUIRE(nibble_at(bitstream, 1) == state - 3);
}

// States 18 and above set the state nibble to 0b1111 and carry (state-18) in one more nibble.
TEST_CASE("Extruder states above 17 are encoded in a second nibble", "[TriangleSelector]")
{
    const int state = GENERATE(18, 25, 32);

    TriangleSelector selector(test_mesh());
    selector.set_facet(0, EnforcerBlockerType(state));
    const std::vector<bool> bitstream = selector.serialize().bitstream;

    INFO("Extruder " << state);
    REQUIRE(bitstream.size() == 12);
    REQUIRE(nibble_at(bitstream, 0) == 0b1100);
    REQUIRE(nibble_at(bitstream, 1) == 0b1111);
    REQUIRE(nibble_at(bitstream, 2) == state - 18);
}

// Model.cpp writes these hex strings into the 3MF for colored mesh imports; the selector must
// decode exactly the states CONST_FILAMENTS assigns to them.
TEST_CASE("Extruder states match the CONST_FILAMENTS hex encoding", "[TriangleSelector]")
{
    struct Case { const char *hex; int state; };
    const auto c = GENERATE(values<Case>({
        {"8", 2}, {"0C", 3}, {"DC", 16}, {"EC", 17}, {"0FC", 18}, {"EFC", 32},
    }));

    // get_triangle_as_string emits the nibbles most significant first, so read the hex backwards.
    const std::string hex = c.hex;
    std::vector<bool> bitstream;
    for (auto it = hex.rbegin(); it != hex.rend(); ++it) {
        const int nibble = *it >= 'A' ? (*it - 'A' + 10) : (*it - '0');
        for (int bit = 0; bit < 4; ++bit)
            bitstream.push_back((nibble >> bit) & 1);
    }

    TriangleSelector::TriangleSplittingData data;
    data.triangles_to_split.emplace_back(0, 0);
    data.bitstream = bitstream;

    INFO("Hex " << c.hex << " -> extruder " << c.state);
    REQUIRE(TriangleSelector::has_facets(data, EnforcerBlockerType(c.state)));
}

// Pack 4-bit codes into a bitstream, least significant bit first, in the order the decoder reads them.
static std::vector<bool> pack_nibbles(const std::vector<int> &nibbles)
{
    std::vector<bool> bitstream;
    for (const int nibble : nibbles)
        for (int bit = 0; bit < 4; ++bit)
            bitstream.push_back((nibble >> bit) & 1);
    return bitstream;
}

TEST_CASE("A valid paint stream with nested splits round-trips bit for bit", "[TriangleSelector]")
{
    const TriangleMesh mesh = test_mesh();

    TriangleSelector::TriangleSplittingData data;
    data.triangles_to_split.emplace_back(0, 0);
    // A three-side split whose children, in stream order, are: a one-side split (side 2) into two
    // leaves, a two-side split (side 1) into leaves of states 20, 0 and 8, then two plain leaves.
    const std::vector<int> triangle_0 = {0b0011,
                                         0b1001, 0b1000, 0b0100,
                                         0b0110, 0b1100, 0b1111, 20 - 18, 0b0000, 0b1100, 8 - 3,
                                         0b1000,
                                         0b0100};
    data.bitstream = pack_nibbles(triangle_0);
    data.triangles_to_split.emplace_back(5, int(data.bitstream.size()));
    const std::vector<bool> triangle_5 = pack_nibbles({0b1100, 3 - 3});
    data.bitstream.insert(data.bitstream.end(), triangle_5.begin(), triangle_5.end());
    data.reset_used_states();
    REQUIRE(data.update_used_states(0));

    TriangleSelector restored(mesh);
    restored.deserialize(data);

    REQUIRE(restored.num_facets(EnforcerBlockerType::Extruder20) == 1);
    REQUIRE(restored.num_facets(EnforcerBlockerType::Extruder3) == 1);
    REQUIRE(restored.serialize() == data);
}

TEST_CASE("A truncated or malformed paint stream drops only the damaged triangle", "[TriangleSelector][Regression]")
{
    struct Case { const char *name; std::vector<int> nibbles; };
    const auto c = GENERATE(values<Case>({
        {"three-side split missing two children", {0b0011, 0b1000, 0b1000}},
        {"leaf missing its state nibble",         {0b1100}},
        {"leaf missing its second state nibble",  {0b1100, 0b1111}},
        {"splits nested past the end",            {0b0011, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF,
                                                    0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF}},
        {"one-side split of the nonexistent side 3", {0b1101, 0b1000, 0b1000}},
    }));
    INFO(c.name);

    const TriangleMesh mesh = test_mesh();
    TriangleSelector   intact(mesh);
    intact.set_facet(0, EnforcerBlockerType::Extruder2);

    // Triangle 0 stays intact, triangle 1 carries the damaged stream.
    TriangleSelector::TriangleSplittingData data = intact.serialize();
    data.triangles_to_split.emplace_back(1, int(data.bitstream.size()));
    const std::vector<bool> damaged = pack_nibbles(c.nibbles);
    data.bitstream.insert(data.bitstream.end(), damaged.begin(), damaged.end());

    TriangleSelector restored(mesh);
    REQUIRE_NOTHROW(restored.deserialize(data));
    // Triangle 1 unwinds completely, so the selector holds exactly the intact paint.
    REQUIRE(restored.serialize() == intact.serialize());

    REQUIRE_NOTHROW(TriangleSelector::has_facets(data, EnforcerBlockerType::Extruder3));

    TriangleSelector::TriangleSplittingData recomputed = data;
    recomputed.reset_used_states();
    REQUIRE_FALSE(recomputed.update_used_states(0));
    REQUIRE(std::none_of(recomputed.used_states.begin(), recomputed.used_states.end(), [](bool used) { return used; }));
}

// Paint state's total area on a mesh and its area-weighted centroid, measured on the leaf triangles of the deserialized paint.
static std::pair<double, Vec3d> painted_area_and_centroid(const TriangleMesh &mesh, const TriangleSelector::TriangleSplittingData &data,
                                                          EnforcerBlockerType state)
{
    TriangleSelector selector(mesh);
    selector.deserialize(data);
    const indexed_triangle_set facets = selector.get_facets(state);
    double area = 0.;
    Vec3d  moment = Vec3d::Zero();
    for (const stl_triangle_vertex_indices &f : facets.indices) {
        const Vec3f &a = facets.vertices[f(0)], &b = facets.vertices[f(1)], &c = facets.vertices[f(2)];
        const double facet_area = 0.5 * double((b - a).cross(c - a).norm());
        area += facet_area;
        moment += facet_area * (a + b + c).cast<double>() / 3.;
    }
    return {area, area > 0. ? Vec3d(moment / area) : Vec3d::Zero()};
}

static double painted_area(const TriangleMesh &mesh, const TriangleSelector::TriangleSplittingData &data, EnforcerBlockerType state)
{
    return painted_area_and_centroid(mesh, data, state).first;
}

// Brush strokes of alternating colors over the sphere, deep enough to subdivide the facets.
static TriangleSelector::TriangleSplittingData brush_paint(const TriangleMesh &mesh, int strokes)
{
    TriangleSelector selector(mesh);
    const int step = std::max(1, int(mesh.its.indices.size()) / strokes);
    for (int facet = 0, i = 0; facet < int(mesh.its.indices.size()); facet += step, ++i) {
        const stl_triangle_vertex_indices &f = mesh.its.indices[facet];
        const Vec3f center = (mesh.its.vertices[f(0)] + mesh.its.vertices[f(1)] + mesh.its.vertices[f(2)]) / 3.f;
        auto cursor = std::make_unique<TriangleSelector::Sphere>(center, 2.f * center, 1.f, Transform3d::Identity(),
                                                                 TriangleSelector::ClippingPlane());
        selector.select_patch(facet, std::move(cursor), EnforcerBlockerType(1 + i % 3), Transform3d::Identity(), true);
    }
    return selector.serialize();
}

TEST_CASE("Remapping paint onto an unchanged mesh copies it exactly", "[TriangleSelector]")
{
    const TriangleMesh mesh  = make_sphere(20., 2 * PI / 48);
    const auto         paint = brush_paint(mesh, 40);
    REQUIRE(!paint.triangles_to_split.empty());

    SECTION("same coordinates") {
        REQUIRE(TriangleSelector::remap_painting(mesh.its, paint, mesh.its, Transform3d::Identity(), {}) == paint);
    }
    SECTION("target shifted, as after recentering a volume") {
        const Vec3d  shift(12.345, -6.789, 3.21);
        TriangleMesh shifted = mesh;
        shifted.translate(-shift.cast<float>());
        REQUIRE(TriangleSelector::remap_painting(mesh.its, paint, shifted.its, Geometry::translation_transform(shift), {}) == paint);
    }
}

TEST_CASE("Remapping paint onto a mesh with rotated triangle vertices keeps the paint", "[TriangleSelector]")
{
    const TriangleMesh mesh  = make_sphere(20., 2 * PI / 48);
    const auto         paint = brush_paint(mesh, 40);

    TriangleMesh rotated = mesh;
    for (size_t i = 0; i < rotated.its.indices.size(); ++i) {
        stl_triangle_vertex_indices &f = rotated.its.indices[i];
        for (size_t r = 0; r < i % 3; ++r)
            f = stl_triangle_vertex_indices(f(1), f(2), f(0));
    }

    const auto remapped = TriangleSelector::remap_painting(mesh.its, paint, rotated.its, Transform3d::Identity(), {});
    for (int state = 1; state <= 3; ++state) {
        const auto [expected_area, expected_centroid] = painted_area_and_centroid(mesh, paint, EnforcerBlockerType(state));
        const auto [area, centroid]                   = painted_area_and_centroid(rotated, remapped, EnforcerBlockerType(state));
        REQUIRE(expected_area > 0.);
        // A midpoint split gives every child the same share of the area whichever side is special,
        // so the area alone would not notice the paint landing on the wrong children. Its centroid does.
        REQUIRE_THAT(area, Catch::Matchers::WithinRel(expected_area, 1e-5));
        REQUIRE((centroid - expected_centroid).norm() < 1e-4);
    }
}

TEST_CASE("Remapping paint onto a changed triangle still paints it", "[TriangleSelector]")
{
    const TriangleMesh mesh = make_sphere(20., 2 * PI / 48);
    TriangleSelector   selector(mesh);
    selector.set_facet(0, EnforcerBlockerType::Extruder2);
    selector.set_facet(1, EnforcerBlockerType::Extruder3);
    const auto paint = selector.serialize();

    // Replace facet 0 by three facets meeting at its centroid, as a repair re-triangulating it would.
    indexed_triangle_set its = mesh.its;
    const stl_triangle_vertex_indices f = its.indices[0];
    its.vertices.emplace_back((its.vertices[f(0)] + its.vertices[f(1)] + its.vertices[f(2)]) / 3.f);
    const int c = int(its.vertices.size()) - 1;
    its.indices[0] = stl_triangle_vertex_indices(f(0), f(1), c);
    its.indices.emplace_back(f(1), f(2), c);
    its.indices.emplace_back(f(2), f(0), c);
    const TriangleMesh changed(its);

    const auto remapped = TriangleSelector::remap_painting(mesh.its, paint, changed.its, Transform3d::Identity(), {});
    // The changed facet is painted again geometrically, which is approximate. Its unchanged neighbor keeps its paint exactly.
    REQUIRE_THAT(painted_area(changed, remapped, EnforcerBlockerType::Extruder2),
                 Catch::Matchers::WithinRel(painted_area(mesh, paint, EnforcerBlockerType::Extruder2), 0.05));
    REQUIRE_THAT(painted_area(changed, remapped, EnforcerBlockerType::Extruder3),
                 Catch::Matchers::WithinRel(painted_area(mesh, paint, EnforcerBlockerType::Extruder3), 1e-5));
}

TEST_CASE("Remapping ignores existing paint whose triangle index is out of range", "[TriangleSelector][Regression]")
{
    const TriangleMesh mesh  = make_sphere(20., 2 * PI / 48);
    const auto         paint = brush_paint(mesh, 40);

    // deserialize() rejects this stream as a whole, so it must not keep any face from being copied either.
    TriangleSelector::TriangleSplittingData existing;
    existing.triangles_to_split.emplace_back(paint.triangles_to_split.front().triangle_idx, 0);
    existing.triangles_to_split.emplace_back(int(mesh.its.indices.size()) + 100, 4);
    existing.bitstream = pack_nibbles({1, 1});

    REQUIRE(TriangleSelector::remap_painting(mesh.its, paint, mesh.its, Transform3d::Identity(), std::cref(existing)) == paint);
}
