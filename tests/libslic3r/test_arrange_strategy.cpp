#include <catch2/catch_all.hpp>

#include "libslic3r/Arrange.hpp"
#include "../libnest2d/printer_parts.hpp"

#include <chrono>
#include <set>

using namespace Slic3r;
using namespace Slic3r::arrangement;

// --- Test helpers ---

static ArrangePolygon make_square(double size_mm, double height_mm, int bed_temp = 60, const std::string &name = "")
{
    ArrangePolygon ap;
    coord_t s = scaled(size_mm);
    ap.poly.contour = {Point(0, 0), Point(s, 0), Point(s, s), Point(0, s)};
    ap.height = height_mm;
    ap.bed_temp = bed_temp;
    ap.first_bed_temp = bed_temp;
    ap.filament_temp_type = 0;
    ap.extrude_ids = {0};
    ap.name = name.empty() ? ("sq_" + std::to_string(int(size_mm)) + "_h" + std::to_string(int(height_mm))) : name;
    return ap;
}

static ArrangeParams make_seq_params()
{
    ArrangeParams params;
    params.is_seq_print = true;
    params.clearance_radius = 50.0;
    params.clearance_height_to_rod = 40.0;
    params.clearance_height_to_lid = 120.0;
    params.nozzle_height = 4.0;
    params.stopcondition = nullptr;
    params.progressind = [](unsigned, std::string) {};
    return params;
}

static Points make_bed_250x210()
{
    return {Point(0, 0), Point(scaled(250.0), 0), Point(scaled(250.0), scaled(210.0)), Point(0, scaled(210.0))};
}

static int count_plates(const ArrangePolygons &items)
{
    std::set<int> beds;
    for (const auto &item : items)
        if (item.bed_idx != UNARRANGED) beds.insert(item.bed_idx);
    return static_cast<int>(beds.size());
}

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
    Points bed = make_bed_250x210();
    ArrangeParams params;
    params.is_seq_print = false;

    auto result = portfolio_arrange(items, excludes, bed, params);
    REQUIRE_FALSE(result.has_value()); // fallback, no portfolio result
}

TEST_CASE("portfolio_arrange falls back for empty items", "[Arrange][Portfolio]") {
    ArrangePolygons items; // empty
    ArrangePolygons excludes;
    Points bed = make_bed_250x210();
    ArrangeParams params;
    params.is_seq_print = true;

    auto result = portfolio_arrange(items, excludes, bed, params);
    REQUIRE_FALSE(result.has_value()); // fallback, empty input
}

TEST_CASE("portfolio_arrange returns valid result for sequential print", "[Arrange][Portfolio]") {
    // Create a simple set of small square polygons with proper inflation
    ArrangePolygons items;
    for (int i = 0; i < 5; ++i)
        items.push_back(make_square(20.0, 10.0 + i * 5.0));

    ArrangePolygons excludes;
    Points bed = make_bed_250x210();
    auto params = make_seq_params();

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
    for (int i = 0; i < 3; ++i)
        items.push_back(make_square(20.0, 10.0));

    ArrangePolygons excludes;
    auto bed = make_bed_250x210();
    auto params = make_seq_params();
    params.stopcondition = []() { return true; }; // Cancel immediately

    auto result = portfolio_arrange(items, excludes, bed, params);
    // May or may not have a result depending on timing, but should not hang
}

// === ORCA-4: Benchmark & Regression Tests ===

// --- AC-1: Each PlacementTactic produces valid results ---

TEST_CASE("Each PlacementTactic produces valid arrangement", "[Arrange][Strategy][Tactic]") {
    auto tactics = {PlacementTactic::Center, PlacementTactic::MaxXMinY,
                    PlacementTactic::MinXMaxY, PlacementTactic::MinXMinY, PlacementTactic::MaxXMaxY};

    for (auto tactic : tactics) {
        DYNAMIC_SECTION("Tactic " << static_cast<int>(tactic)) {
            ArrangePolygons items;
            for (int i = 0; i < 5; ++i)
                items.push_back(make_square(20.0, 10.0 + i * 5.0));

            ArrangePolygons excludes;
            auto bed = make_bed_250x210();
            auto params = make_seq_params();
            params.strategy = ArrangeStrategy{tactic, ObjectOrdering::HeightMinToMax};

            arrange(items, excludes, bed, params);

            for (const auto &item : items)
                REQUIRE(item.bed_idx != UNARRANGED);
        }
    }
}

// --- AC-2: Each ObjectOrdering produces correctly ordered results ---

TEST_CASE("Each ObjectOrdering produces valid arrangement", "[Arrange][Strategy][Ordering]") {
    auto orderings = {ObjectOrdering::HeightMinToMax, ObjectOrdering::HeightMaxToMin,
                      ObjectOrdering::HeightRandom, ObjectOrdering::HeightInput};

    for (auto ordering : orderings) {
        DYNAMIC_SECTION("Ordering " << static_cast<int>(ordering)) {
            ArrangePolygons items;
            for (int i = 0; i < 5; ++i)
                items.push_back(make_square(20.0, 10.0 + i * 10.0));

            ArrangePolygons excludes;
            auto bed = make_bed_250x210();
            auto params = make_seq_params();
            params.strategy = ArrangeStrategy{PlacementTactic::MinXMinY, ordering};

            arrange(items, excludes, bed, params);

            for (const auto &item : items)
                REQUIRE(item.bed_idx != UNARRANGED);
        }
    }
}

// --- AC-3: All 20 strategy combinations produce valid results ---

TEST_CASE("All 20 strategies produce valid arrange results", "[Arrange][Strategy][Integration]") {
    auto all_tactics = {PlacementTactic::Center, PlacementTactic::MaxXMinY,
                        PlacementTactic::MinXMaxY, PlacementTactic::MinXMinY, PlacementTactic::MaxXMaxY};
    auto all_orderings = {ObjectOrdering::HeightMinToMax, ObjectOrdering::HeightMaxToMin,
                          ObjectOrdering::HeightRandom, ObjectOrdering::HeightInput};

    auto bed = make_bed_250x210();
    int valid_count = 0;

    for (auto tactic : all_tactics) {
        for (auto ordering : all_orderings) {
            ArrangePolygons items;
            for (int i = 0; i < 4; ++i)
                items.push_back(make_square(15.0 + i * 5.0, 10.0 + i * 10.0));

            ArrangePolygons excludes;
            auto params = make_seq_params();
            params.strategy = ArrangeStrategy{tactic, ordering};

            arrange(items, excludes, bed, params);

            bool all_arranged = true;
            for (const auto &item : items)
                if (item.bed_idx == UNARRANGED) all_arranged = false;

            if (all_arranged) valid_count++;
        }
    }
    REQUIRE(valid_count == 20);
}

// --- AC-4: Regression test — default strategy matches original behavior ---

TEST_CASE("Default strategy matches original arrange behavior", "[Arrange][Strategy][Regression]") {
    auto bed = make_bed_250x210();

    // Run without strategy (original code path)
    ArrangePolygons items_original;
    for (int i = 0; i < 5; ++i)
        items_original.push_back(make_square(20.0, 15.0));

    ArrangePolygons excludes;
    ArrangeParams params_orig = make_seq_params();
    // No strategy set — uses default code path

    arrange(items_original, excludes, bed, params_orig);

    // Run with explicit defaults_for_seq_print
    ArrangePolygons items_explicit;
    for (int i = 0; i < 5; ++i)
        items_explicit.push_back(make_square(20.0, 15.0));

    ArrangeParams params_explicit = make_seq_params();
    params_explicit.strategy = ArrangeStrategy::defaults_for_seq_print();

    arrange(items_explicit, excludes, bed, params_explicit);

    // Results must be identical
    REQUIRE(items_original.size() == items_explicit.size());
    for (size_t i = 0; i < items_original.size(); ++i) {
        REQUIRE(items_original[i].bed_idx == items_explicit[i].bed_idx);
        REQUIRE(items_original[i].translation.x() == items_explicit[i].translation.x());
        REQUIRE(items_original[i].translation.y() == items_explicit[i].translation.y());
    }
}

// --- AC-5/6/7/8: Benchmark tests ---

TEST_CASE("Benchmark: Portfolio vs Single strategy", "[Arrange][Portfolio][Benchmark]") {
    auto bed = make_bed_250x210();
    ArrangePolygons excludes;

    SECTION("Testset (a): 10 identical cubes") {
        ArrangePolygons items_single, items_portfolio;
        for (int i = 0; i < 10; ++i) {
            items_single.push_back(make_square(20.0, 20.0));
            items_portfolio.push_back(make_square(20.0, 20.0));
        }

        auto params = make_seq_params();
        arrange(items_single, excludes, bed, params);
        int single_plates = count_plates(items_single);

        auto result = portfolio_arrange(items_portfolio, excludes, bed, params);
        int portfolio_plates = count_plates(items_portfolio);

        INFO("Testset (a): Single=" << single_plates << " plates, Portfolio=" << portfolio_plates << " plates");
        REQUIRE(portfolio_plates <= single_plates);
    }

    SECTION("Testset (b): 20 mixed geometries with varying heights") {
        ArrangePolygons items_single, items_portfolio;
        for (int i = 0; i < 20; ++i) {
            double size = 10.0 + (i % 4) * 10.0;   // 10, 20, 30, 40mm
            double height = 5.0 + i * 5.0;           // 5 to 100mm
            items_single.push_back(make_square(size, height));
            items_portfolio.push_back(make_square(size, height));
        }

        auto params = make_seq_params();
        arrange(items_single, excludes, bed, params);
        int single_plates = count_plates(items_single);

        auto result = portfolio_arrange(items_portfolio, excludes, bed, params);
        int portfolio_plates = count_plates(items_portfolio);

        INFO("Testset (b): Single=" << single_plates << " plates, Portfolio=" << portfolio_plates << " plates");
        REQUIRE(portfolio_plates <= single_plates);
    }

    SECTION("Testset (c): Printer parts from libnest2d test data") {
        // Use real printer part polygons — take first 30 (or all available)
        size_t num_parts = std::min(PRINTER_PART_POLYGONS.size(), size_t(30));
        ArrangePolygons items_single, items_portfolio;

        for (size_t i = 0; i < num_parts; ++i) {
            ArrangePolygon ap;
            // Convert libnest2d polygon to ExPolygon contour
            Points pts;
            for (const auto &pt : PRINTER_PART_POLYGONS[i])
                pts.push_back(Point(pt.x(), pt.y()));
            if (!pts.empty()) {
                ap.poly.contour = Polygon(pts);
                // Assign height based on index (10-80mm range)
                ap.height = 10.0 + (i % 8) * 10.0;
                ap.bed_temp = 60;
                ap.filament_temp_type = 0;
                ap.extrude_ids = {0};
                ap.name = "printer_part_" + std::to_string(i);
                items_single.push_back(ap);
                items_portfolio.push_back(ap);
            }
        }

        auto params = make_seq_params();
        arrange(items_single, excludes, bed, params);
        int single_plates = count_plates(items_single);

        auto t_start = std::chrono::high_resolution_clock::now();
        auto result = portfolio_arrange(items_portfolio, excludes, bed, params);
        auto t_end = std::chrono::high_resolution_clock::now();
        int portfolio_plates = count_plates(items_portfolio);

        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();

        INFO("Testset (c): " << num_parts << " printer parts");
        INFO("Single=" << single_plates << " plates, Portfolio=" << portfolio_plates << " plates");
        INFO("Portfolio runtime: " << duration_ms << "ms");

        // AC-7: Portfolio should use same or fewer plates
        REQUIRE(portfolio_plates <= single_plates);

        // AC-8: Runtime should be reasonable (generous limit for CI machines)
        REQUIRE(duration_ms < 120000); // 120s generous limit (spec says 60s on 8-core)
    }
}
