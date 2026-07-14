#ifndef FG_TEST_EVALUATOR_HPP
#define FG_TEST_EVALUATOR_HPP

#include "fg_test_serialization.hpp"
#include <libslic3r/FilamentGroup.hpp>
#include <libslic3r/GCode/ToolOrderUtils.hpp>
#include <libslic3r/MultiNozzleUtils.hpp>

#include <chrono>
#include <sstream>
#include <unordered_set>

namespace Slic3r {
namespace FGTest {

inline bool check_constraints(const FilamentGroupContext& ctx,
                              const std::vector<int>& filament_map,
                              std::vector<std::string>& violations) {
    violations.clear();
    auto used_filaments = collect_sorted_used_filaments(ctx.model_info.layer_filaments);

    // 1. unprintable_filaments check
    for (size_t ext = 0; ext < ctx.model_info.unprintable_filaments.size(); ++ext) {
        for (int fil : ctx.model_info.unprintable_filaments[ext]) {
            if (fil < 0 || fil >= (int)filament_map.size())
                continue;
            int assigned_nozzle = filament_map[fil];
            if (assigned_nozzle < 0 || assigned_nozzle >= (int)ctx.nozzle_info.nozzle_list.size())
                continue;
            if (ctx.nozzle_info.nozzle_list[assigned_nozzle].extruder_id == (int)ext) {
                std::ostringstream ss;
                ss << "filament " << fil << " assigned to nozzle " << assigned_nozzle
                   << " (extruder " << ext << ") but is unprintable there";
                violations.push_back(ss.str());
            }
        }
    }

    // 2. unprintable_volumes check
    for (auto& [fil, volume_types] : ctx.model_info.unprintable_volumes) {
        if (fil < 0 || fil >= (int)filament_map.size())
            continue;
        int assigned_nozzle = filament_map[fil];
        if (assigned_nozzle < 0 || assigned_nozzle >= (int)ctx.nozzle_info.nozzle_list.size())
            continue;
        if (volume_types.count(ctx.nozzle_info.nozzle_list[assigned_nozzle].volume_type)) {
            std::ostringstream ss;
            ss << "filament " << fil << " assigned to nozzle " << assigned_nozzle
               << " with volume_type " << (int)ctx.nozzle_info.nozzle_list[assigned_nozzle].volume_type
               << " but that type is unprintable for this filament";
            violations.push_back(ss.str());
        }
    }

    // 3. max_group_size per extruder. This cap is an invariant of the flush-partition
    //    modes only: those solvers partition the filaments across extruders subject to
    //    each extruder's capacity. MatchMode instead maps every filament to the extruder
    //    holding the nearest-color loaded AMS filament and does not partition by capacity
    //    (its solver capacity is the filament count, not max_group_size), so a legitimate
    //    match may place more than max_group_size filaments on one extruder. Enforce the
    //    cap only for the partition modes, and only when the instance is feasible.
    int total_capacity = 0;
    for (auto sz : ctx.machine_info.max_group_size)
        total_capacity += sz;

    if (ctx.group_info.mode != FGMode::MatchMode &&
        total_capacity >= (int)used_filaments.size()) {
        std::map<int, int> extruder_count;
        for (auto fil : used_filaments) {
            if (fil >= filament_map.size()) continue;
            int nozzle_id = filament_map[fil];
            if (nozzle_id < 0 || nozzle_id >= (int)ctx.nozzle_info.nozzle_list.size())
                continue;
            extruder_count[ctx.nozzle_info.nozzle_list[nozzle_id].extruder_id]++;
        }
        for (auto& [ext, count] : extruder_count) {
            if (ext >= 0 && ext < (int)ctx.machine_info.max_group_size.size()) {
                if (count > ctx.machine_info.max_group_size[ext]) {
                    std::ostringstream ss;
                    ss << "extruder " << ext << " has " << count << " filaments but max is "
                       << ctx.machine_info.max_group_size[ext];
                    violations.push_back(ss.str());
                }
            }
        }
    }

    return violations.empty();
}

inline int compute_flush_cost(const FilamentGroupContext& ctx,
                              const std::vector<int>& filament_map) {
    auto used_filaments = collect_sorted_used_filaments(ctx.model_info.layer_filaments);
    if (used_filaments.empty())
        return 0;

    auto nozzle_group_result = MultiNozzleUtils::LayeredNozzleGroupResult::create(
        filament_map, ctx.nozzle_info.nozzle_list, used_filaments);

    if (!nozzle_group_result)
        return -1;

    std::vector<std::vector<unsigned int>> filament_sequences;
    auto get_custom_seq_null = [](int, std::vector<int>&) -> bool { return false; };

    int cost = reorder_filaments_for_multi_nozzle_extruder(
        used_filaments,
        *nozzle_group_result,
        ctx.model_info.layer_filaments,
        ctx.model_info.flush_matrix,
        get_custom_seq_null,
        &filament_sequences,
        MultiNozzleUtils::NozzleStatusRecorder{});

    return cost;
}

struct FullEvalResult {
    int flush_cost = 0;
    double change_time = 0.0;
    double full_score = 0.0;
    bool constraints_ok = true;
    std::vector<std::string> violations;
};

inline double evaluate_score(double flush, double time) {
    double approx_density = 1.26;
    double approx_flush_speed = 180;
    double correction_factor = 2;
    double flush_score = flush * approx_density * approx_flush_speed * correction_factor / 1000;
    return flush_score + time;
}

inline double calc_change_time_for_group_eval(
    const std::vector<int>& filament_change_seq,
    const std::vector<int>& nozzle_change_seq,
    const std::vector<int>& logical_filaments,
    const std::vector<MultiNozzleUtils::NozzleInfo>& nozzle_list,
    const MultiNozzleUtils::FilamentChangeTimeParams& time_params,
    const std::vector<bool>& ams_preload_enabled,
    const std::vector<int>& group_of_filament)
{
    auto r = MultiNozzleUtils::simulate_filament_change_time(
        logical_filaments, nozzle_list, filament_change_seq,
        nozzle_change_seq, group_of_filament, time_params,
        ams_preload_enabled);
    return r.actual_time;
}

inline FullEvalResult full_evaluate_map(const FilamentGroupContext& ctx,
                                         const std::vector<int>& filament_map) {
    FullEvalResult result;
    auto used_filaments = collect_sorted_used_filaments(ctx.model_info.layer_filaments);
    if (used_filaments.empty()) return result;

    auto nozzle_group_result = MultiNozzleUtils::LayeredNozzleGroupResult::create(
        filament_map, ctx.nozzle_info.nozzle_list, used_filaments);
    if (!nozzle_group_result) return result;

    MultiNozzleUtils::NozzleStatusRecorder initial_status;
    for (auto& [nozzle_id, filament_id] : ctx.nozzle_info.nozzle_status) {
        if (filament_id >= 0) {
            int extruder_id = 0;
            for (const auto& nozzle : ctx.nozzle_info.nozzle_list) {
                if (nozzle.group_id == nozzle_id) { extruder_id = nozzle.extruder_id; break; }
            }
            initial_status.set_nozzle_status(nozzle_id, filament_id, extruder_id);
        }
    }

    std::vector<std::vector<unsigned int>> filament_sequences;
    auto get_custom_seq_null = [](int, std::vector<int>&) -> bool { return false; };

    result.flush_cost = reorder_filaments_for_multi_nozzle_extruder(
        used_filaments, *nozzle_group_result, ctx.model_info.layer_filaments,
        ctx.model_info.flush_matrix, get_custom_seq_null, &filament_sequences, initial_status);

    if (!filament_sequences.empty()) {
        std::vector<int> filament_change_seq;
        std::vector<int> nozzle_change_seq;
        int prev_fil = -1, prev_nozzle = -1;
        for (const auto& layer_seq : filament_sequences) {
            for (unsigned int fil : layer_seq) {
                auto nozzle_info = nozzle_group_result->get_first_nozzle_for_filament(fil);
                if (!nozzle_info) continue;
                int nid = nozzle_info->group_id;
                if ((int)fil == prev_fil && nid == prev_nozzle) continue;
                filament_change_seq.push_back((int)fil);
                nozzle_change_seq.push_back(nid);
                prev_fil = (int)fil;
                prev_nozzle = nid;
            }
        }

        std::vector<int> logical_filaments(used_filaments.begin(), used_filaments.end());
        std::vector<int> group_of_filament(used_filaments.size(), 0);
        for (size_t fi = 0; fi < used_filaments.size(); ++fi) {
            int nid = filament_map[used_filaments[fi]];
            if (nid >= 0 && nid < (int)ctx.nozzle_info.nozzle_list.size())
                group_of_filament[fi] = ctx.nozzle_info.nozzle_list[nid].extruder_id;
        }
        result.change_time = calc_change_time_for_group_eval(
            filament_change_seq, nozzle_change_seq, logical_filaments,
            ctx.nozzle_info.nozzle_list, ctx.speed_info.change_time_params,
            ctx.speed_info.ams_preload_enabled, group_of_filament);
    }

    result.full_score = evaluate_score(result.flush_cost, result.change_time);
    result.constraints_ok = check_constraints(ctx, filament_map, result.violations);
    return result;
}

inline TestResult run_and_evaluate(const FilamentGroupContext& ctx) {
    TestResult result;

    auto start = std::chrono::high_resolution_clock::now();
    int algo_cost = 0;

    FilamentGroup fg(ctx);
    result.filament_map = fg.calc_filament_group(&algo_cost);

    auto end = std::chrono::high_resolution_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();

    result.flush_cost = compute_flush_cost(ctx, result.filament_map);

    result.constraints_ok = check_constraints(ctx, result.filament_map, result.violations);

    return result;
}

} // namespace FGTest
} // namespace Slic3r

#endif // FG_TEST_EVALUATOR_HPP
