// H2C/A2L FilamentGroup golden regression harness.
//
// Notes:
//   * Orca: links Catch2::Catch2WithMain and uses the v3 convenience include <catch2/catch_all.hpp>.
//   * All three golden families (config_a one-nozzle-per-extruder, config_b/config_c nozzle-centric)
//     are evaluated against the goldens. The nozzle-centric FilamentGroup engine and solver layer run
//     the same algorithm the goldens were generated with, scored via the nozzle-aware reorder
//     (fg_test_evaluator.hpp) at a 3% one-directional tolerance.
//   * The hidden [update-golden] utility is intentionally omitted: the goldens are the reference
//     and must not be rewritten from Orca output.

#include <catch2/catch_all.hpp>

#include "fg_test_serialization.hpp"
#include "fg_test_evaluator.hpp"
#include "fg_test_utils.hpp"

#include <filesystem>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <numeric>

namespace fs = std::filesystem;
using namespace Slic3r;
using namespace Slic3r::FGTest;

// ============ Helpers ============

static std::vector<std::string> collect_test_files(const std::string& dir) {
    std::vector<std::string> files;
    if (!fs::exists(dir)) return files;
    for (auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.path().extension() == ".json" &&
            entry.path().string().find(".result.") == std::string::npos) {
            files.push_back(entry.path().string());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

static std::vector<std::string> get_golden_files() {
    static std::vector<std::string> files = collect_test_files(FG_TEST_GOLDEN_DIR);
    return files;
}

static bool is_constraint_feasible(const FilamentGroupContext& ctx,
                                    const std::vector<unsigned int>& used_filaments) {
    int total_capacity = 0;
    for (auto sz : ctx.machine_info.max_group_size)
        total_capacity += sz;
    if (total_capacity < (int)used_filaments.size())
        return false;

    // Check that every filament has at least one valid nozzle
    for (auto fil : used_filaments) {
        bool has_valid_nozzle = false;
        for (size_t nid = 0; nid < ctx.nozzle_info.nozzle_list.size(); ++nid) {
            auto& nozzle = ctx.nozzle_info.nozzle_list[nid];
            // Check unprintable_filaments
            if (nozzle.extruder_id >= 0 && nozzle.extruder_id < (int)ctx.model_info.unprintable_filaments.size()) {
                if (ctx.model_info.unprintable_filaments[nozzle.extruder_id].count(fil))
                    continue;
            }
            // Check unprintable_volumes
            if (ctx.model_info.unprintable_volumes.count(fil)) {
                if (ctx.model_info.unprintable_volumes.at(fil).count(nozzle.volume_type))
                    continue;
            }
            has_valid_nozzle = true;
            break;
        }
        if (!has_valid_nozzle)
            return false;
    }
    return true;
}

// ============ Property Check Specs ============

struct PropertySpec {
    std::string id;
    std::string config;
    int seed;
    int num_filaments;
    int num_layers;
    bool chaotic;
    bool with_constraints;
    FGMode mode;
    FGStrategy strategy;
    bool group_with_time;
};

static std::vector<PropertySpec> build_property_specs() {
    std::vector<PropertySpec> specs;

    // Config A: 20 cases
    for (int i = 0; i < 6; ++i) {
        int seed = 90000 + i;
        TestRng rng(seed);
        specs.push_back({"prop_a_basic_" + std::to_string(i), "A", seed,
                         rng.rand_int(2, 6), rng.rand_int(100, 400),
                         false, false, FGMode::FlushMode, FGStrategy::BestCost, false});
    }
    for (int i = 0; i < 4; ++i) {
        int seed = 90100 + i;
        TestRng rng(seed);
        specs.push_back({"prop_a_stress_" + std::to_string(i), "A", seed,
                         rng.rand_int(7, 10), rng.rand_int(500, 1000),
                         false, false, FGMode::FlushMode, FGStrategy::BestCost, false});
    }
    for (int i = 0; i < 4; ++i) {
        int seed = 90200 + i;
        TestRng rng(seed);
        specs.push_back({"prop_a_constraint_" + std::to_string(i), "A", seed,
                         rng.rand_int(3, 8), rng.rand_int(100, 400),
                         false, true, FGMode::FlushMode, FGStrategy::BestCost, false});
    }
    for (int i = 0; i < 3; ++i) {
        int seed = 90300 + i;
        TestRng rng(seed);
        specs.push_back({"prop_a_edge_" + std::to_string(i), "A", seed,
                         rng.rand_int(2, 3), rng.rand_int(10, 50),
                         true, false, FGMode::FlushMode, FGStrategy::BestCost, false});
    }
    specs.push_back({"prop_a_mode_match", "A", 90400,
                     5, 200, false, false, FGMode::MatchMode, FGStrategy::BestCost, false});
    specs.push_back({"prop_a_mode_bestfit", "A", 90401,
                     5, 200, false, false, FGMode::FlushMode, FGStrategy::BestFit, false});
    specs.push_back({"prop_a_mode_time", "A", 90402,
                     5, 200, false, false, FGMode::FlushMode, FGStrategy::BestCost, true});

    // Config B: 25 cases
    for (int i = 0; i < 6; ++i) {
        int seed = 91000 + i;
        TestRng rng(seed);
        specs.push_back({"prop_b_basic_" + std::to_string(i), "B", seed,
                         rng.rand_int(3, 8), rng.rand_int(100, 400),
                         false, false, FGMode::FlushMode, FGStrategy::BestCost, false});
    }
    for (int i = 0; i < 6; ++i) {
        int seed = 91100 + i;
        TestRng rng(seed);
        specs.push_back({"prop_b_stress_" + std::to_string(i), "B", seed,
                         rng.rand_int(9, 12), rng.rand_int(500, 1000),
                         false, false, FGMode::FlushMode, FGStrategy::BestCost, false});
    }
    for (int i = 0; i < 7; ++i) {
        int seed = 91200 + i;
        TestRng rng(seed);
        specs.push_back({"prop_b_constraint_" + std::to_string(i), "B", seed,
                         rng.rand_int(4, 10), rng.rand_int(100, 400),
                         false, true, FGMode::FlushMode, FGStrategy::BestCost, false});
    }
    for (int i = 0; i < 3; ++i) {
        int seed = 91300 + i;
        TestRng rng(seed);
        specs.push_back({"prop_b_edge_" + std::to_string(i), "B", seed,
                         rng.rand_int(2, 4), rng.rand_int(10, 50),
                         true, false, FGMode::FlushMode, FGStrategy::BestCost, false});
    }
    specs.push_back({"prop_b_mode_match", "B", 91400,
                     6, 200, false, false, FGMode::MatchMode, FGStrategy::BestCost, false});
    specs.push_back({"prop_b_mode_bestfit", "B", 91401,
                     6, 200, false, false, FGMode::FlushMode, FGStrategy::BestFit, false});
    specs.push_back({"prop_b_mode_time", "B", 91402,
                     6, 200, false, false, FGMode::FlushMode, FGStrategy::BestCost, true});

    // Config C: 15 cases
    for (int i = 0; i < 5; ++i) {
        int seed = 92000 + i;
        TestRng rng(seed);
        specs.push_back({"prop_c_basic_" + std::to_string(i), "C", seed,
                         rng.rand_int(3, 9), rng.rand_int(100, 400),
                         false, false, FGMode::FlushMode, FGStrategy::BestCost, false});
    }
    for (int i = 0; i < 3; ++i) {
        int seed = 92100 + i;
        TestRng rng(seed);
        specs.push_back({"prop_c_stress_" + std::to_string(i), "C", seed,
                         rng.rand_int(10, 15), rng.rand_int(500, 1000),
                         false, false, FGMode::FlushMode, FGStrategy::BestCost, false});
    }
    for (int i = 0; i < 3; ++i) {
        int seed = 92200 + i;
        TestRng rng(seed);
        specs.push_back({"prop_c_constraint_" + std::to_string(i), "C", seed,
                         rng.rand_int(4, 9), rng.rand_int(100, 400),
                         false, true, FGMode::FlushMode, FGStrategy::BestCost, false});
    }
    for (int i = 0; i < 2; ++i) {
        int seed = 92300 + i;
        TestRng rng(seed);
        specs.push_back({"prop_c_edge_" + std::to_string(i), "C", seed,
                         rng.rand_int(2, 4), rng.rand_int(10, 50),
                         true, false, FGMode::FlushMode, FGStrategy::BestCost, false});
    }
    specs.push_back({"prop_c_mode_match", "C", 92400,
                     6, 200, false, false, FGMode::MatchMode, FGStrategy::BestCost, false});
    specs.push_back({"prop_c_mode_bestfit", "C", 92401,
                     6, 200, false, false, FGMode::FlushMode, FGStrategy::BestFit, false});

    return specs;
}

static std::vector<PropertySpec>& get_property_specs() {
    static std::vector<PropertySpec> specs = build_property_specs();
    return specs;
}

// Under the default wall clock the result depends on how fast the machine is (see ClusteringBudget),
// so the goldens are graded under a fixed budget instead. Two restarts is the fewest that reaches
// parity with the reference on every golden, stress_79 being the last to get there. Four leaves
// margin, since the search follows a different path on each standard library (see below).
static constexpr ClusteringBudget FIXED_SEARCH_BUDGET{
    /*timeout_ms*/ 0, // no wall clock
    /*max_restarts*/ 4};

// ============ Layer 1: Golden Regression (all configs) ============

// Graded against the BambuStudio golden the harness was ported from, one-directional at 3%.
//
// The tolerance is a parity allowance, and it also covers a small spread across standard libraries.
// The k-medoids search seeds each restart with std::shuffle, whose algorithm the C++ standard leaves
// unspecified, so libstdc++, libc++ and the MSVC STL permute the same seed differently, start from
// different medoids, and settle on slightly different groupings, about 3e-4 apart on either side of
// the reference, and only on the goldens heavy enough to reach the k-medoids search.
TEST_CASE("FilamentGroup golden regression", "[filament_group][golden]") {
    auto files = get_golden_files();
    if (files.empty()) {
        WARN("No golden files found in " FG_TEST_GOLDEN_DIR);
        REQUIRE(!files.empty());
        return;
    }

    auto file_path = GENERATE_REF(from_range(files));

    DYNAMIC_SECTION("Golden: " << fs::path(file_path).stem().string()) {
        auto tc = load_test_case(file_path);
        REQUIRE(tc.base_result.has_value());

        auto result = run_and_evaluate(tc.context, FIXED_SEARCH_BUDGET);
        auto eval = full_evaluate_map(tc.context, result.filament_map);

        auto& base = *tc.base_result;

        INFO("Case: " << tc.metadata.id);
        INFO("Golden score: " << base.full_score);
        INFO("Actual score: " << eval.full_score);
        INFO("Flush cost: " << eval.flush_cost << " (golden " << base.flush_cost << ")");
        INFO("Elapsed: " << result.elapsed_ms << " ms");

        int tolerance = std::max(50, (int)(base.full_score * 0.03));

        REQUIRE(result.constraints_ok);
        REQUIRE(eval.full_score <= base.full_score + tolerance);

        // A slower search still scores the same above, since it searches just as far, but in slicing
        // it would mean fewer restarts fit in the wall clock and so worse groupings. Loose on
        // purpose, so it never becomes a proxy for how loaded the runner is.
        const double throughput_ceiling_ms = 10.0 * ClusteringBudget{}.timeout_ms;
        REQUIRE(result.elapsed_ms < throughput_ceiling_ms);
    }
}

// Covers the path real slicing takes, under the default wall clock. The score there depends on the
// runner rather than on the code (see FIXED_SEARCH_BUDGET), so the only things worth asserting are
// that the grouping comes back valid and that the search terminates.
TEST_CASE("FilamentGroup returns a valid grouping under the default budget", "[filament_group][budget]") {
    auto files = get_golden_files();
    REQUIRE(!files.empty());

    auto file_path = GENERATE_REF(from_range(files));

    DYNAMIC_SECTION("Golden: " << fs::path(file_path).stem().string()) {
        auto tc = load_test_case(file_path);

        auto result = run_and_evaluate(tc.context); // the default budget, as real slicing runs it

        INFO("Case: " << tc.metadata.id);
        INFO("Elapsed: " << result.elapsed_ms << " ms");

        REQUIRE(result.constraints_ok);
        // A hang guard. The clock is only checked between swaps, so a sweep can overshoot.
        REQUIRE(result.elapsed_ms < 40000.0);
    }
}

// ============ Layer 2: Property Checks (all configs) ============

TEST_CASE("FilamentGroup property checks", "[filament_group][property]") {
    auto& specs = get_property_specs();
    auto spec = GENERATE_REF(from_range(specs));

    DYNAMIC_SECTION("Property: " << spec.id) {
        auto tc = build_test_case(spec.id, spec.config, spec.seed,
                                   spec.num_filaments, spec.num_layers,
                                   spec.chaotic, spec.with_constraints,
                                   spec.mode, spec.strategy, spec.group_with_time);

        auto result = run_and_evaluate(tc.context);

        INFO("Case: " << spec.id);
        INFO("Config: " << spec.config);
        INFO("Flush cost: " << result.flush_cost);
        INFO("Elapsed: " << result.elapsed_ms << " ms");

        // RelWithDebInfo runaway guard; the Release-calibrated 10 s limit is raised for the slower
        // build (config_b/config_c cases evaluate the full per-layer nozzle-aware reorder for every
        // candidate grouping; this is a guard against hangs, not a micro-perf gate).
        REQUIRE(result.elapsed_ms < 40000.0);
        REQUIRE(result.flush_cost >= 0);

        auto used_filaments = collect_sorted_used_filaments(tc.context.model_info.layer_filaments);
        if (is_constraint_feasible(tc.context, used_filaments)) {
            if (!result.constraints_ok) {
                for (auto& v : result.violations)
                    WARN("Violation: " << v);
            }
            REQUIRE(result.constraints_ok);
        } else {
            if (!result.constraints_ok) {
                WARN("Constraint violation (infeasible case, soft): " << spec.id);
            }
        }
    }
}
