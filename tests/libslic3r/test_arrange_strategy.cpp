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

// --- Portfolio Runner (ORCA-2) ---

TEST_CASE("PortfolioResult struct is constructible", "[Arrange][Portfolio]") {
    PortfolioResult r{
        ArrangeStrategy{PlacementTactic::Center, ObjectOrdering::HeightMinToMax},
        3,  // num_plates
        20  // strategies_evaluated
    };
    REQUIRE(r.best_strategy.tactic == PlacementTactic::Center);
    REQUIRE(r.best_strategy.ordering == ObjectOrdering::HeightMinToMax);
    REQUIRE(r.num_plates == 3);
    REQUIRE(r.strategies_evaluated == 20);
}

TEST_CASE("portfolio_arrange falls back for non-sequential print", "[Arrange][Portfolio]") {
    ArrangePolygons items;
    ArrangePolygons excludes;
    Points bed = {Point(0, 0), Point(scaled(250), 0), Point(scaled(250), scaled(210)), Point(0, scaled(210))};
    ArrangeParams params;
    params.is_seq_print = false;

    auto result = portfolio_arrange(items, excludes, bed, params);
    REQUIRE_FALSE(result.has_value()); // fallback, no portfolio result
}

TEST_CASE("portfolio_arrange falls back for empty items", "[Arrange][Portfolio]") {
    ArrangePolygons items; // empty
    ArrangePolygons excludes;
    Points bed = {Point(0, 0), Point(scaled(250), 0), Point(scaled(250), scaled(210)), Point(0, scaled(210))};
    ArrangeParams params;
    params.is_seq_print = true;

    auto result = portfolio_arrange(items, excludes, bed, params);
    REQUIRE_FALSE(result.has_value()); // fallback, empty input
}

TEST_CASE("portfolio_arrange returns valid result for sequential print", "[Arrange][Portfolio]") {
    // Create a simple set of small square polygons
    ArrangePolygons items;
    for (int i = 0; i < 5; ++i) {
        ArrangePolygon ap;
        coord_t size = scaled(20.0); // 20mm squares
        ap.poly.contour = {Point(0, 0), Point(size, 0), Point(size, size), Point(0, size)};
        ap.height = 10.0 + i * 5.0; // varying heights: 10, 15, 20, 25, 30
        ap.bed_temp = 60;
        ap.filament_temp_type = 0;
        ap.extrude_ids = {0};
        ap.name = "obj_" + std::to_string(i);
        items.push_back(ap);
    }

    ArrangePolygons excludes;
    // 250x210mm bed (Prusa MK3S size)
    Points bed = {Point(0, 0), Point(scaled(250), 0), Point(scaled(250), scaled(210)), Point(0, scaled(210))};

    ArrangeParams params;
    params.is_seq_print = true;
    params.clearance_radius = 50.0; // 50mm clearance
    params.clearance_height_to_rod = 40.0;
    params.clearance_height_to_lid = 120.0;
    params.nozzle_height = 4.0;

    auto result = portfolio_arrange(items, excludes, bed, params);

    // Portfolio should have run and returned a result
    REQUIRE(result.has_value());
    REQUIRE(result->strategies_evaluated > 0);
    REQUIRE(result->strategies_evaluated <= 20);
    REQUIRE(result->num_plates >= 1);

    // All items should be arranged
    for (const auto &item : items) {
        REQUIRE(item.bed_idx != UNARRANGED);
    }
}

TEST_CASE("portfolio_arrange respects stopcondition", "[Arrange][Portfolio]") {
    ArrangePolygons items;
    for (int i = 0; i < 3; ++i) {
        ArrangePolygon ap;
        coord_t size = scaled(20.0);
        ap.poly.contour = {Point(0, 0), Point(size, 0), Point(size, size), Point(0, size)};
        ap.height = 10.0;
        ap.bed_temp = 60;
        ap.filament_temp_type = 0;
        ap.extrude_ids = {0};
        ap.name = "obj_" + std::to_string(i);
        items.push_back(ap);
    }

    ArrangePolygons excludes;
    Points bed = {Point(0, 0), Point(scaled(250), 0), Point(scaled(250), scaled(210)), Point(0, scaled(210))};

    ArrangeParams params;
    params.is_seq_print = true;
    params.clearance_radius = 50.0;
    params.clearance_height_to_rod = 40.0;
    params.clearance_height_to_lid = 120.0;
    params.nozzle_height = 4.0;
    // Cancel immediately
    params.stopcondition = []() { return true; };

    auto result = portfolio_arrange(items, excludes, bed, params);
    // May or may not have a result depending on timing, but should not hang
    // (this test verifies no deadlock on immediate cancel)
}
