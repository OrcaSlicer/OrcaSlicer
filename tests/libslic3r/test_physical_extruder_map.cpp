#include <catch2/catch_test_macros.hpp>

#include <utility>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/PhysicalExtruderMap.hpp"

using namespace Slic3r;

static ConfigOptionInts make_pem(std::vector<int> v) {
    ConfigOptionInts o;
    o.values = std::move(v);
    return o;
}

TEST_CASE("effective_physical_extruder_map - authored map wins", "[PhysicalExtruderMap]") {
    // An AFC manifold on a 7-extruder machine: logical 0-3 all feed physical 0. Not derivable,
    // so it must be honoured verbatim.
    auto explicit_pem = make_pem({0, 0, 0, 0, 1, 2, 3});
    auto out = effective_physical_extruder_map(&explicit_pem, 7);
    REQUIRE(out.values == std::vector<int>{0, 0, 0, 0, 1, 2, 3});
}

TEST_CASE("effective_physical_extruder_map - a permutation is honoured", "[PhysicalExtruderMap]") {
    // Shipping BBL dual-nozzle profiles author {1,0}: a slicer/firmware numbering swap, not a
    // sharing map. Deriving over it would silently renumber both extruders.
    auto explicit_pem = make_pem({1, 0});
    auto out = effective_physical_extruder_map(&explicit_pem, 2);
    REQUIRE(out.values == std::vector<int>{1, 0});
}

TEST_CASE("effective_physical_extruder_map - unauthored derives the identity", "[PhysicalExtruderMap]") {
    // The PrintConfig default is a single element, which is not an authored map on a 2-extruder
    // machine. Identity is what upstream itself falls back to.
    auto default_pem = make_pem({0});
    auto out = effective_physical_extruder_map(&default_pem, 2);
    REQUIRE(out.values == std::vector<int>{0, 1});
}

TEST_CASE("effective_physical_extruder_map - result is always one entry per extruder", "[PhysicalExtruderMap]") {
    // The defining property: consumers index this map by logical extruder and size their own
    // arrays from nozzle_diameter, so a shorter map is an out-of-bounds read in several of them.
    for (int n : { 1, 2, 4, 7 }) {
        DYNAMIC_SECTION("nozzle count " << n) {
            REQUIRE((int) effective_physical_extruder_map(nullptr, n).values.size() == n);
            auto stale = make_pem({0});   // wrong length: must not be mistaken for authored
            REQUIRE((int) effective_physical_extruder_map(&stale, n).values.size() == n);
        }
    }
}

TEST_CASE("effective_physical_extruder_map - degenerate nozzle count still yields a usable map", "[PhysicalExtruderMap]") {
    // Never hand back an empty map: several consumers index it as a raw vector.
    REQUIRE(effective_physical_extruder_map(nullptr, 0).values == std::vector<int>{0});
}
