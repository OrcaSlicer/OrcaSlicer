#include <catch2/catch_all.hpp>

#include "libslic3r/Arrange.hpp"

using namespace Slic3r;
using namespace Slic3r::arrangement;

// --- PlacementTactic enum ---

TEST_CASE("PlacementTactic has 5 values", "[Arrange][Strategy]") {
    // Verify all 5 enum values are distinct and compilable
    std::vector<PlacementTactic> tactics = {
        PlacementTactic::Center,
        PlacementTactic::MaxXMinY,
        PlacementTactic::MinXMaxY,
        PlacementTactic::MinXMinY,
        PlacementTactic::MaxXMaxY,
    };
    REQUIRE(tactics.size() == 5);

    // All values must be distinct
    std::set<int> values;
    for (auto t : tactics) values.insert(static_cast<int>(t));
    REQUIRE(values.size() == 5);
}

// --- ObjectOrdering enum ---

TEST_CASE("ObjectOrdering has 4 values", "[Arrange][Strategy]") {
    std::vector<ObjectOrdering> orderings = {
        ObjectOrdering::HeightMinToMax,
        ObjectOrdering::HeightMaxToMin,
        ObjectOrdering::HeightRandom,
        ObjectOrdering::HeightInput,
    };
    REQUIRE(orderings.size() == 4);

    std::set<int> values;
    for (auto o : orderings) values.insert(static_cast<int>(o));
    REQUIRE(values.size() == 4);
}

// --- ArrangeStrategy struct ---

TEST_CASE("ArrangeStrategy composition", "[Arrange][Strategy]") {
    SECTION("Manual construction") {
        ArrangeStrategy s{PlacementTactic::Center, ObjectOrdering::HeightMaxToMin};
        REQUIRE(s.tactic == PlacementTactic::Center);
        REQUIRE(s.ordering == ObjectOrdering::HeightMaxToMin);
    }

    SECTION("defaults_for_seq_print matches current behavior") {
        auto s = ArrangeStrategy::defaults_for_seq_print();
        REQUIRE(s.tactic == PlacementTactic::MinXMinY);
        REQUIRE(s.ordering == ObjectOrdering::HeightMinToMax);
    }
}

// --- ArrangeParams integration ---

TEST_CASE("ArrangeParams strategy field", "[Arrange][Strategy]") {
    SECTION("Default is nullopt") {
        ArrangeParams params;
        REQUIRE_FALSE(params.strategy.has_value());
    }

    SECTION("Can be set to a strategy") {
        ArrangeParams params;
        params.strategy = ArrangeStrategy{PlacementTactic::MaxXMaxY, ObjectOrdering::HeightRandom};
        REQUIRE(params.strategy.has_value());
        REQUIRE(params.strategy->tactic == PlacementTactic::MaxXMaxY);
        REQUIRE(params.strategy->ordering == ObjectOrdering::HeightRandom);
    }

    SECTION("Can be reset to nullopt") {
        ArrangeParams params;
        params.strategy = ArrangeStrategy::defaults_for_seq_print();
        REQUIRE(params.strategy.has_value());
        params.strategy = std::nullopt;
        REQUIRE_FALSE(params.strategy.has_value());
    }
}

// --- All 20 strategy combinations are constructible ---

TEST_CASE("All 20 strategy combinations are valid", "[Arrange][Strategy]") {
    std::vector<PlacementTactic> tactics = {
        PlacementTactic::Center,
        PlacementTactic::MaxXMinY,
        PlacementTactic::MinXMaxY,
        PlacementTactic::MinXMinY,
        PlacementTactic::MaxXMaxY,
    };
    std::vector<ObjectOrdering> orderings = {
        ObjectOrdering::HeightMinToMax,
        ObjectOrdering::HeightMaxToMin,
        ObjectOrdering::HeightRandom,
        ObjectOrdering::HeightInput,
    };

    int count = 0;
    for (auto t : tactics) {
        for (auto o : orderings) {
            ArrangeStrategy s{t, o};
            ArrangeParams params;
            params.is_seq_print = true;
            params.strategy = s;
            REQUIRE(params.strategy.has_value());
            REQUIRE(params.strategy->tactic == t);
            REQUIRE(params.strategy->ordering == o);
            count++;
        }
    }
    REQUIRE(count == 20);
}

// --- Backward compatibility: default ArrangeParams unchanged ---

TEST_CASE("Default ArrangeParams backward compatible", "[Arrange][Strategy][Regression]") {
    ArrangeParams params;

    // strategy must be nullopt by default
    REQUIRE_FALSE(params.strategy.has_value());

    // All existing defaults must be preserved
    REQUIRE(params.min_obj_distance == 0);
    REQUIRE(params.is_seq_print == false);
    REQUIRE(params.parallel == true);
    REQUIRE(params.allow_rotations == false);
    REQUIRE(params.do_final_align == true);
    REQUIRE(params.allow_multi_materials_on_same_plate == true);
    REQUIRE(params.avoid_extrusion_cali_region == true);
}
