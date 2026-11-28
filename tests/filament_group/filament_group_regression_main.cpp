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

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include "fg_test_serialization.hpp"
#include "fg_test_evaluator.hpp"
#include "fg_test_utils.hpp"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <fstream>
#include "libslic3r/FilamentGroup.hpp"
#include "libslic3r/FilamentGroupUtils.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <string>
#include <vector>
#include <algorithm>
#include <numeric>
#include "libslic3r/MultiNozzleUtils.hpp"

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
// In flush mode the goldens grade the least-flush search, so those cases run without the printer's
// filaments. With them, the AMS match may choose a grouping up to about five filament changes worse
// that matches the loaded filaments better, which the goldens predate; the [filament_group] tests that
// put filaments where the printer has them loaded cover that choice. Match-mode cases group by the
// printer's filaments and keep them.
//
// A_mode_match_150 is checked for its constraints only. Match mode picks by colour, not flush, and its
// colour comparison is finer than when the goldens were made (two-decimal distances where whole numbers
// used to tie): that case's filament 2 (#37A850) now takes a slot at distance 10.16 on the second
// extruder instead of one at 10.78 on the first, which costs more flush. The "Match-mode grouping" tests
// cover how it picks.
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

        auto search_context = tc.context;
        if (search_context.group_info.mode == FGMode::FlushMode)
            for (auto& filaments : search_context.machine_info.machine_filament_info)
                filaments.clear();

        auto result = run_and_evaluate(search_context, FIXED_SEARCH_BUDGET);
        auto eval = full_evaluate_map(tc.context, result.filament_map);

        auto& base = *tc.base_result;

        INFO("Case: " << tc.metadata.id);
        INFO("Golden score: " << base.full_score);
        INFO("Actual score: " << eval.full_score);
        INFO("Flush cost: " << eval.flush_cost << " (golden " << base.flush_cost << ")");
        INFO("Elapsed: " << result.elapsed_ms << " ms");

        int tolerance = std::max(50, (int)(base.full_score * 0.03));

        REQUIRE(result.constraints_ok);
        if (tc.metadata.id != "A_mode_match_150")
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

// ============ Layer 3: Matching the filaments loaded in the printer ============

// Two extruders with one nozzle each, as on the H2D and X2D, extruder 0 being the master. A change
// between two different filaments flushes `flush`, and a load or an unload takes 29 s, so no
// grouping scores 0.
static FilamentGroupContext two_extruder_context(const std::vector<FilamentGroupUtils::FilamentInfo>& filaments,
                                                 const std::vector<std::vector<unsigned int>>& layer_filaments,
                                                 float flush)
{
    const int n = (int)filaments.size();
    FilamentGroupContext ctx{};
    ctx.nozzle_info.nozzle_list = {{"0.4", NozzleVolumeType::nvtStandard, 0, 0},
                                   {"0.4", NozzleVolumeType::nvtStandard, 1, 1}};
    ctx.nozzle_info.extruder_nozzle_list = {{0, {0}}, {1, {1}}};

    ctx.machine_info.max_group_size = {4, 4};
    ctx.machine_info.prefer_non_model_filament = {false, false};
    ctx.machine_info.master_extruder_id = 0;
    ctx.machine_info.machine_filament_info.resize(2);

    std::vector<std::vector<float>> matrix(n, std::vector<float>(n, flush));
    for (int i = 0; i < n; ++i)
        matrix[i][i] = 0;
    ctx.model_info.flush_matrix = {matrix, matrix};
    ctx.model_info.unprintable_filaments.resize(2);
    ctx.model_info.layer_filaments = layer_filaments;
    ctx.model_info.filament_info = filaments;
    for (int i = 0; i < n; ++i)
        ctx.model_info.filament_ids.push_back("GFL_" + std::to_string(i));

    ctx.group_info.total_filament_num = n;
    ctx.group_info.max_gap_threshold = 0.01;
    ctx.group_info.mode = FGMode::FlushMode;
    ctx.group_info.strategy = FGStrategy::BestCost;
    ctx.group_info.filament_volume_map.assign(n, (int)NozzleVolumeType::nvtHybrid);

    ctx.speed_info.change_time_params = {/*selector_load_time*/ 14.5f, /*selector_unload_time*/ 14.5f,
                                         /*standard_load_time*/ 29.f, /*standard_unload_time*/ 29.f};
    ctx.speed_info.ams_preload_enabled = {false, false};
    return ctx;
}

static FilamentGroupUtils::FilamentInfo filament(const char* color, const char* type)
{
    return {FilamentGroupUtils::Color(color), type, false};
}

static FilamentGroupUtils::MachineFilamentInfo loaded(const char* color, const char* type, int extruder_id)
{
    FilamentGroupUtils::MachineFilamentInfo info;
    info.color = FilamentGroupUtils::Color(color);
    info.type = type;
    info.is_support = false;
    info.extruder_id = extruder_id;
    info.is_extended = false;
    return info;
}

static std::vector<int> grouped_extruders(const FilamentGroupContext& ctx)
{
    FilamentGroup fg(ctx);
    std::vector<int> extruders;
    for (int nozzle : fg.calc_filament_group())
        extruders.push_back(ctx.nozzle_info.nozzle_list[nozzle].extruder_id);
    return extruders;
}

TEST_CASE("Filament-saving grouping puts a lone filament on the extruder that has it loaded", "[filament_group]") {
    auto ctx = two_extruder_context({filament("#F95D73", "PETG")}, std::vector<std::vector<unsigned int>>(10, {0}), 0.f);
    ctx.machine_info.machine_filament_info[0] = {loaded("#FF6A13", "ABS", 0), loaded("#F98C36", "PLA", 0), loaded("#BCBCBC", "PETG", 0)};
    ctx.machine_info.machine_filament_info[1] = {loaded("#F95D73", "PETG", 1)};

    CHECK(grouped_extruders(ctx) == std::vector<int>{1});
}

TEST_CASE("Filament-saving grouping keeps two filaments on the extruder that has both loaded when it costs a few changes", "[filament_group]") {
    // Split, the two print without a change; on one extruder, two changes, within the budget of
    // about five.
    auto ctx = two_extruder_context({filament("#FFFFFF", "PLA"), filament("#000000", "PLA")}, {{0}, {1}, {0}}, 100.f);
    ctx.machine_info.machine_filament_info[0] = {loaded("#FF0000", "PETG", 0)};
    ctx.machine_info.machine_filament_info[1] = {loaded("#FFFFFF", "PLA", 1), loaded("#000000", "PLA", 1)};

    CHECK(grouped_extruders(ctx) == std::vector<int>{1, 1});
}

TEST_CASE("Filament-saving grouping splits two filaments when keeping them together costs many changes", "[filament_group]") {
    auto ctx = two_extruder_context({filament("#FFFFFF", "PLA"), filament("#000000", "PLA")}, {{0}, {1}, {0}, {1}, {0}, {1}, {0}}, 100.f);
    ctx.machine_info.machine_filament_info[0] = {loaded("#FF0000", "PETG", 0)};
    ctx.machine_info.machine_filament_info[1] = {loaded("#FFFFFF", "PLA", 1), loaded("#000000", "PLA", 1)};

    auto extruders = grouped_extruders(ctx);
    REQUIRE(extruders.size() == 2);
    CHECK(extruders[0] != extruders[1]);
}

TEST_CASE("Filament-saving grouping keeps the least-flush split when the printer reports no filaments", "[filament_group]") {
    auto ctx = two_extruder_context({filament("#FFFFFF", "PLA"), filament("#000000", "PLA")}, {{0}, {1}, {0}}, 100.f);

    auto extruders = grouped_extruders(ctx);
    REQUIRE(extruders.size() == 2);
    CHECK(extruders[0] != extruders[1]);
}

TEST_CASE("Filament-saving grouping keeps a lone filament on the master extruder when neither extruder has it loaded", "[filament_group]") {
    // The master reports nothing; the other extruder holds only a filament of another type.
    auto ctx = two_extruder_context({filament("#FFFFFF", "PLA")}, std::vector<std::vector<unsigned int>>(10, {0}), 0.f);
    ctx.machine_info.machine_filament_info[1] = {loaded("#FFFFFF", "PETG", 1)};

    CHECK(grouped_extruders(ctx) == std::vector<int>{0});
}

TEST_CASE("Near-optimal groupings are kept within five filament changes, 60 points or 1 % of the best", "[filament_group]") {
    using namespace FilamentGroupUtils;
    auto kept = [](double best, double candidate, double one_change_budget) {
        MemoryedGroupHeap heap;
        update_memoryed_groups(MemoryedGroup({0}, best, 1), one_change_budget, 0.01, heap);
        update_memoryed_groups(MemoryedGroup({1}, candidate, 1), one_change_budget, 0.01, heap);
        return heap.size() == 2;
    };

    CHECK(kept(100, 160, 0));
    CHECK_FALSE(kept(100, 161, 0));
    CHECK(kept(100, 200, 20));
    CHECK_FALSE(kept(100, 201, 20));
    CHECK(kept(20000, 20200, 0));
    CHECK_FALSE(kept(20000, 20201, 0));
}

TEST_CASE("Match-mode grouping picks the slot of the same filament sub-type among slots of one colour", "[filament_group]") {
    // Both extruders hold white PLA; only the second one's is the project filament's sub-type.
    auto ctx = two_extruder_context({filament("#FFFFFF", "PLA")}, std::vector<std::vector<unsigned int>>(10, {0}), 0.f);
    ctx.group_info.mode = FGMode::MatchMode;
    ctx.model_info.filament_ids = {"GFA00"};
    auto generic = loaded("#FFFFFF", "PLA", 0);
    generic.filament_id = "GFL99";
    auto same_sub_type = loaded("#FFFFFF", "PLA", 1);
    same_sub_type.filament_id = "GFA00";
    ctx.machine_info.machine_filament_info[0] = {generic};
    ctx.machine_info.machine_filament_info[1] = {same_sub_type};

    CHECK(grouped_extruders(ctx) == std::vector<int>{1});
}

TEST_CASE("Match-mode grouping ranks colour above filament sub-type", "[filament_group]") {
    // The second extruder has the project filament's sub-type in a different shade; the first has
    // the exact colour in another sub-type.
    auto ctx = two_extruder_context({filament("#FFFFFF", "PLA")}, std::vector<std::vector<unsigned int>>(10, {0}), 0.f);
    ctx.group_info.mode = FGMode::MatchMode;
    ctx.model_info.filament_ids = {"GFA00"};
    auto exact_colour = loaded("#FFFFFF", "PLA", 0);
    exact_colour.filament_id = "GFL99";
    auto same_sub_type = loaded("#D0D0D0", "PLA", 1);
    same_sub_type.filament_id = "GFA00";
    ctx.machine_info.machine_filament_info[0] = {exact_colour};
    ctx.machine_info.machine_filament_info[1] = {same_sub_type};

    CHECK(grouped_extruders(ctx) == std::vector<int>{0});
}

TEST_CASE("Match-mode grouping picks the closer colour when two slots differ by under one unit of distance", "[filament_group]") {
    // Colour distances about 10.78 (first extruder) and 10.16 (second): equal in whole units.
    auto ctx = two_extruder_context({filament("#37A850", "PLA")}, std::vector<std::vector<unsigned int>>(10, {0}), 0.f);
    ctx.group_info.mode = FGMode::MatchMode;
    ctx.machine_info.machine_filament_info[0] = {loaded("#40C111", "PLA", 0)};
    ctx.machine_info.machine_filament_info[1] = {loaded("#3F8A30", "PLA", 1)};

    CHECK(grouped_extruders(ctx) == std::vector<int>{1});
}

TEST_CASE("Match-mode grouping ranks one step of colour above a filament sub-type", "[filament_group]") {
    // Against white, #F0F1F2 is at 2.9364 and #F0F1F1 at 2.9266: one step of 0.01 apart once rounded.
    // The farther slot has the project filament's sub-type, the closer one another.
    auto ctx = two_extruder_context({filament("#FFFFFF", "PLA")}, std::vector<std::vector<unsigned int>>(10, {0}), 0.f);
    ctx.group_info.mode = FGMode::MatchMode;
    ctx.model_info.filament_ids = {"GFA00"};
    auto same_sub_type = loaded("#F0F1F2", "PLA", 0);
    same_sub_type.filament_id = "GFA00";
    auto closer_colour = loaded("#F0F1F1", "PLA", 1);
    closer_colour.filament_id = "GFL99";
    ctx.machine_info.machine_filament_info[0] = {same_sub_type};
    ctx.machine_info.machine_filament_info[1] = {closer_colour};

    CHECK(grouped_extruders(ctx) == std::vector<int>{1});
}
