#include <catch2/catch_all.hpp>

#include <string>
#include <vector>

#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"

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

// Model.cpp CONST_FILAMENTS writes these hex strings into 3MF; get/set_triangle_as_string
// must emit and recover exactly those states (same path as colored mesh import).
TEST_CASE("Extruder states match the CONST_FILAMENTS hex encoding", "[TriangleSelector]")
{
    auto hex_for_state = [](int state) -> std::string {
        Model model;
        ModelObject *obj = model.add_object("const_filaments", "", make_cube(20., 20., 20.));
        obj->add_instance();
        ModelVolume *vol = obj->volumes.front();
        TriangleSelector sel(vol->mesh());
        sel.set_facet(0, EnforcerBlockerType(state));
        REQUIRE(vol->mmu_segmentation_facets.set(sel));
        return vol->mmu_segmentation_facets.get_triangle_as_string(0);
    };
    const std::string hex2  = hex_for_state(2);
    const std::string hex3  = hex_for_state(3);
    const std::string hex16 = hex_for_state(16);
    const std::string hex17 = hex_for_state(17);
    const std::string hex18 = hex_for_state(18);
    const std::string hex32 = hex_for_state(32);
    CHECK(hex2 == "8");
    CHECK(hex3 == "0C");
    CHECK(hex16 == "DC");
    CHECK(hex17 == "EC");
    CHECK(hex18 == "0FC");
    CHECK(hex32 == "EFC");

    auto state_used_from_hex = [](const std::string &hex, int state) {
        Model model;
        ModelObject *obj = model.add_object("const_filaments_load", "", make_cube(20., 20., 20.));
        obj->add_instance();
        ModelVolume *vol = obj->volumes.front();
        vol->mmu_segmentation_facets.set_triangle_from_string(0, hex);
        const auto &used = vol->mmu_segmentation_facets.get_data().used_states;
        return state < int(used.size()) && used[size_t(state)];
    };
    const bool load2  = state_used_from_hex("8", 2);
    const bool load3  = state_used_from_hex("0C", 3);
    const bool load16 = state_used_from_hex("DC", 16);
    const bool load17 = state_used_from_hex("EC", 17);
    const bool load18 = state_used_from_hex("0FC", 18);
    const bool load32 = state_used_from_hex("EFC", 32);
    CHECK(load2);
    CHECK(load3);
    CHECK(load16);
    CHECK(load17);
    CHECK(load18);
    CHECK(load32);
}
