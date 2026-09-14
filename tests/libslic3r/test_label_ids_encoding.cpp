// Regression test for GCode::encode_label_ids_to_base64.
//
// Background: a feature branch added an option that re-routes top/bottom external
// solid surfaces to a non-primary extruder. Under print_sequence=ByLayer +
// exclude-object enabled, the per-extruder pre-pass at GCode.cpp:5095-5097 builds
// a vector of label_object_ids for the instances using that extruder, then encodes
// it for an M624 line. With the new routing an instance can have zero entries on
// the primary extruder for a given layer, leaving the vector empty. The encoder's
// previous "bitset == 0 -> throw LogicError" arm fired on that empty input,
// killing slicing with the opaque message "Label object id error!" and no other
// diagnostic.
//
// Fix: encode_label_ids_to_base64({}, ...) returns "" — the M624 emission path
// already gates on the encoded string being non-empty, so an empty result just
// suppresses that line for the layer (matching the M624-bracketing semantics:
// no instances => no bracket). This test pins that contract and a few adjacent
// invariants so the empty-input crash can't sneak back in.

#include <catch2/catch_all.hpp>

#include "libslic3r/GCode.hpp"
#include "libslic3r/Exception.hpp"

using namespace Slic3r;

TEST_CASE("encode_label_ids_to_base64 returns empty string on empty input",
          "[GCode][LabelObjects]")
{
    // Was previously: throw LogicError("Label object id error!").
    // Reachable in production via surface_wall_override_filament_target ∈ {Surfaces, Both}
    // routing top/bottom infill to a non-primary extruder, leaving the primary's
    // instance list empty for the layer.
    REQUIRE(GCode::encode_label_ids_to_base64({}, {}) == std::string());
    REQUIRE(GCode::encode_label_ids_to_base64({}, {0, 1, 2, 3}) == std::string());
}

TEST_CASE("encode_label_ids_to_base64 returns a non-empty string when ids are known",
          "[GCode][LabelObjects]")
{
    const std::vector<size_t> known{0, 1, 2, 3};

    SECTION("Single id maps to a non-empty encoded value")
    {
        REQUIRE_FALSE(GCode::encode_label_ids_to_base64({0}, known).empty());
        REQUIRE_FALSE(GCode::encode_label_ids_to_base64({3}, known).empty());
    }

    SECTION("Same id repeated is idempotent (bitset is a set, not a multiset)")
    {
        const std::string a = GCode::encode_label_ids_to_base64({1},       known);
        const std::string b = GCode::encode_label_ids_to_base64({1, 1, 1}, known);
        REQUIRE(a == b);
    }

    SECTION("Different id sets produce different encodings")
    {
        const std::string only_0     = GCode::encode_label_ids_to_base64({0},    known);
        const std::string only_1     = GCode::encode_label_ids_to_base64({1},    known);
        const std::string both_0_and_1 = GCode::encode_label_ids_to_base64({0, 1}, known);
        REQUIRE(only_0 != only_1);
        REQUIRE(only_0 != both_0_and_1);
        REQUIRE(only_1 != both_0_and_1);
    }
}

TEST_CASE("encode_label_ids_to_base64 throws on an unknown id (programming error, not empty input)",
          "[GCode][LabelObjects]")
{
    // The remaining throw path: a non-empty `ids` containing an entry that is
    // not in `known_ids`. That's a callsite mistake — we do NOT want to absorb
    // it silently because it would emit a stale/wrong label downstream.
    const std::vector<size_t> known{0, 1, 2};
    REQUIRE_THROWS_AS(GCode::encode_label_ids_to_base64({99},   known), Slic3r::LogicError);
    REQUIRE_THROWS_AS(GCode::encode_label_ids_to_base64({0, 5}, known), Slic3r::LogicError);
}
