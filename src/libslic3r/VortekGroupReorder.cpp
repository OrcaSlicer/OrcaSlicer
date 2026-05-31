// ============================================================================
// VortekGroupReorder.cpp
//
// Extracted from ToolOrdering.cpp — H2C-specific GroupReorder logic.
// BBL ref: BambuStudio/src/libslic3r/GCode/ToolOrdering.cpp:1270-1789
// ============================================================================

#include "Print.hpp"
#include "GCode/ToolOrdering.hpp"
#include "GCode/ToolOrderUtils.hpp"
#include "FilamentGroupUtils.hpp"
#include "I18N.hpp"

#include <algorithm>
#include <cassert>
#include <numeric>
#include <unordered_map>

#include <libslic3r.h>

namespace Slic3r {

//! macro used at localization, return same string
#define _L(s) Slic3r::I18N::translate(s)

// ============================================================================
// GroupReorder namespace functions
// Declarations stay in ToolOrdering.hpp — only bodies moved here.
// ============================================================================

// H2C port: Build a complete FilamentGroupContext from print data.
// Assembles flush matrices, nozzle groups, unprintable limits, AMS info,
// filament metadata, and speed info into a single context struct.
// BBL ref: BambuStudio/src/libslic3r/GCode/ToolOrdering.cpp:1270-1460
FilamentGroupContext GroupReorder::build_filament_group_context(
    Print                                           *print,
    const std::vector<std::vector<unsigned int>>    &layer_filaments,
    const std::vector<std::set<int>>                &physical_unprintables,
    const std::vector<std::set<int>>                &geometric_unprintables,
    const std::map<int, std::set<NozzleVolumeType>> &unprintable_volumes,
    FilamentMapMode                                  mode,
    const std::unordered_map<int, int>              &nozzle_status)
{
    using namespace FilamentGroupUtils;
    using namespace MultiNozzleUtils;

    FilamentGroupContext context;

    const auto& print_config = print->config();
    const size_t filament_nums = print_config.filament_colour.values.size();
    const size_t extruder_nums = print_config.nozzle_diameter.values.size();

    // Flush matrices
    auto nozzle_flush_mtx = GroupReorder::prepare_flush_matrices(print_config);

    // Nozzle groups
    auto nozzle_groups = GroupReorder::build_nozzle_groups(print_config, extruder_nums);

    // Unprintable limits
    std::vector<std::set<int>> ext_unprintable_filaments(2);
    collect_unprintable_limits(physical_unprintables, geometric_unprintables, ext_unprintable_filaments);

    // AMS info
    bool ignore_ext_filament = false;
    std::vector<std::string> extruder_ams_count_str = print_config.extruder_ams_count.values;
    auto extruder_ams_counts = get_extruder_ams_count(extruder_ams_count_str);
    std::vector<int> group_size = calc_max_group_size(extruder_ams_counts, ignore_ext_filament);

    std::vector<bool> prefer_non_model_filament(extruder_nums, false);
    auto machine_filament_info = build_machine_filaments(print->get_extruder_filament_info(), extruder_ams_counts, ignore_ext_filament);

    std::vector<std::string> filament_types = print_config.filament_type.values;
    std::vector<std::string> filament_colours = print_config.filament_colour.values;
    std::vector<unsigned char> filament_is_support = print_config.filament_is_support.values;
    std::vector<std::string> filament_ids = print_config.filament_ids.values;
    std::vector<FilamentUsageType> filament_usage_types = build_filament_usage_type_list(print_config, print->objects().vector());

    int master_extruder_id = print_config.master_extruder_id.value - 1;
    FGMode fg_mode = mode == FilamentMapMode::fmmAutoForMatch ? FGMode::MatchMode : FGMode::FlushMode;

    context.model_info.flush_matrix = std::move(nozzle_flush_mtx);
    context.model_info.unprintable_filaments = ext_unprintable_filaments;
    context.model_info.layer_filaments = layer_filaments;
    context.model_info.filament_ids = filament_ids;
    context.model_info.unprintable_volumes = unprintable_volumes;

    for (size_t idx = 0; idx < filament_types.size(); ++idx) {
        FilamentGroupUtils::FilamentInfo info;
        info.color = filament_colours[idx];
        info.type = filament_types[idx];
        info.is_support = filament_is_support[idx];
        info.usage_type = filament_usage_types[idx];
        context.model_info.filament_info.emplace_back(std::move(info));
    }

    context.speed_info.filament_print_time = print->get_filament_print_time();
    context.speed_info.group_with_time = print->config().group_algo_with_time;
    context.speed_info.filament_change_time = print->config().machine_load_filament_time + print->config().machine_unload_filament_time;
    context.speed_info.extruder_change_time = print->config().machine_switch_extruder_time;

    context.machine_info.machine_filament_info = machine_filament_info;
    context.machine_info.max_group_size = std::move(group_size);
    context.machine_info.master_extruder_id = master_extruder_id;
    context.machine_info.prefer_non_model_filament = prefer_non_model_filament;

    context.group_info.total_filament_num = (int)(filament_nums);
    context.group_info.max_gap_threshold = 0.01;
    context.group_info.strategy = FGStrategy::BestCost;
    context.group_info.mode = fg_mode;
    context.group_info.ignore_ext_filament = ignore_ext_filament;

    if (mode == FilamentMapMode::fmmManual)
        context.group_info.filament_volume_map = print_config.filament_volume_map.values;
    else
        context.group_info.filament_volume_map = std::vector<int>(filament_nums, (int)(NozzleVolumeType::nvtHybrid));

    context.nozzle_info.nozzle_list = build_nozzle_list(nozzle_groups);
    context.nozzle_info.extruder_nozzle_list = build_extruder_nozzle_list(context.nozzle_info.nozzle_list);

    if (context.nozzle_info.nozzle_list.empty())
        throw Slic3r::RuntimeError(_L("No valid nozzle found. Please check nozzle count."));

    if (!nozzle_status.empty())
        context.nozzle_info.nozzle_status = nozzle_status;

    return context;
}

// H2C port: Extract per-extruder flush matrices from config and apply flush multipliers.
// BBL ref: BambuStudio/src/libslic3r/GCode/ToolOrdering.cpp:1467-1493
std::vector<FlushMatrix> GroupReorder::prepare_flush_matrices(const PrintConfig& print_config)
{
    size_t extruder_nums = print_config.nozzle_diameter.values.size();
    size_t filament_nums = print_config.filament_colour.values.size();
    std::vector<FlushMatrix> nozzle_flush_mtx;
    for (size_t nozzle_id = 0; nozzle_id < extruder_nums; ++nozzle_id) {
        std::vector<float> flush_matrix(cast<float>(get_flush_volumes_matrix(print_config.flush_volumes_matrix.values, nozzle_id, extruder_nums)));
        std::vector<std::vector<float>> wipe_volumes;
        for (unsigned int i = 0; i < filament_nums; ++i)
            wipe_volumes.push_back(std::vector<float>(flush_matrix.begin() + i * filament_nums, flush_matrix.begin() + (i + 1) * filament_nums));
        nozzle_flush_mtx.emplace_back(wipe_volumes);
    }

    auto flush_multiplies = print_config.flush_multiplier.values;
    flush_multiplies.resize(extruder_nums, 1);
    for (size_t nozzle_id = 0; nozzle_id < extruder_nums; ++nozzle_id) {
        for (auto& vec : nozzle_flush_mtx[nozzle_id]) {
            for (auto& v : vec)
                v *= flush_multiplies[nozzle_id];
        }
    }
    return nozzle_flush_mtx;
}

// H2C port: Build nozzle group descriptors from extruder_nozzle_stats.
// Falls back to extruder_max_nozzle_count when stats are unavailable.
// BBL ref: BambuStudio/src/libslic3r/GCode/ToolOrdering.cpp (inline in build_filament_group_context)
std::vector<MultiNozzleUtils::NozzleGroupInfo> GroupReorder::build_nozzle_groups(const PrintConfig &config, size_t extruder_nums)
{
    using namespace MultiNozzleUtils;
    std::vector<NozzleGroupInfo> nozzle_groups;
    auto extruder_nozzle_counts = get_extruder_nozzle_stats(config.extruder_nozzle_stats.values);
    auto nozzle_volume_types = config.nozzle_volume_type.values;

    for (size_t idx = 0; idx < extruder_nums; ++idx) {
        bool stats_unusable = idx >= extruder_nozzle_counts.size() || extruder_nozzle_counts[idx].empty();
        if (!stats_unusable) {
            stats_unusable = true;
            for (auto& kv : extruder_nozzle_counts[idx]) {
                if (kv.second > 0) { stats_unusable = false; break; }
            }
        }
        if (stats_unusable) {
            int fallback_count = (idx < config.extruder_max_nozzle_count.values.size())
                ? config.extruder_max_nozzle_count.values[idx] : 1;
            if (fallback_count <= 0) fallback_count = 1;
            nozzle_groups.emplace_back(format_diameter_to_str(config.nozzle_diameter.values[idx]),
                NozzleVolumeType(config.nozzle_volume_type.values[idx]), idx, fallback_count);
        } else {
            NozzleVolumeType type = NozzleVolumeType(nozzle_volume_types[idx]);
            if (type == nvtHybrid) {
                for (auto [volume_type, count] : extruder_nozzle_counts[idx]) {
                    if (count <= 0) continue;
                    nozzle_groups.emplace_back(format_diameter_to_str(config.nozzle_diameter.values[idx]),
                        volume_type, idx, count);
                }
            } else {
                nozzle_groups.emplace_back(format_diameter_to_str(config.nozzle_diameter.values[idx]),
                    type, idx, extruder_nozzle_counts[idx][type]);
            }
        }
    }
    return nozzle_groups;
}

// H2C port: Convenience wrapper — build nozzle groups then flatten to NozzleInfo list.
// BBL ref: BambuStudio/src/libslic3r/GCode/ToolOrdering.hpp:143
std::vector<MultiNozzleUtils::NozzleInfo> GroupReorder::build_default_nozzle_list(const PrintConfig &config, size_t extruder_nums)
{
    auto groups = build_nozzle_groups(config, extruder_nums);
    return MultiNozzleUtils::build_nozzle_list(groups);
}


// ============================================================================
// Formerly-static helper functions
// Forward-declared in VortekGroupReorder.hpp
// ============================================================================

// H2C port: Refine filament-to-nozzle assignment using min-cost flow.
// BBL ref: BambuStudio/src/libslic3r/GCode/ToolOrdering.cpp:1495-1631
MultiNozzleUtils::LayeredNozzleGroupResult refine_groups_by_Nozzle_State(
    const FilamentGroupContext& ctx,
    const MultiNozzleUtils::LayeredNozzleGroupResult& group,
    const std::unordered_map<int, int> &nozzles_state)
{
    std::vector<std::vector<int>> nozzle_fils(ctx.nozzle_info.nozzle_list.size());
    auto fils        = group.get_used_filaments(0);
    auto fil_noz_map = group.get_layer_filament_nozzle_map(0);

    for (auto fil : fils)
        nozzle_fils[fil_noz_map[fil]].emplace_back(fil);

    // 1. Collect unprintable nozzles for each filament
    std::map<int, std::set<int>> fil_unplaceable_nozs;
    for (auto fil : fils) {
        std::set<NozzleVolumeType> unprintable_volumes;
        if (ctx.model_info.unprintable_volumes.count(fil))
            unprintable_volumes = ctx.model_info.unprintable_volumes.at(fil);
        auto expected_volume = ctx.group_info.filament_volume_map[fil];

        for (int noz = 0; noz < (int)ctx.nozzle_info.nozzle_list.size(); noz++) {
            auto noz_info             = ctx.nozzle_info.nozzle_list[noz];
            int  ext_id               = noz_info.extruder_id;
            auto ext_unprintable_fils = ctx.model_info.unprintable_filaments[ext_id];
            if (ext_unprintable_fils.count(fil) > 0 ||
                (expected_volume != nvtHybrid && expected_volume != (int)noz_info.volume_type) ||
                (unprintable_volumes.count(noz_info.volume_type) != 0))
                fil_unplaceable_nozs[fil].insert(noz);
        }
    }

    // 2. Store global nozzle match result
    std::unordered_map<int, int> global_uv_match;

    // 3. Process per-extruder groups with min-cost flow
    for (const auto& [ext_id, ext_nozzles] : ctx.nozzle_info.extruder_nozzle_list) {
        if (ext_nozzles.empty()) continue;

        std::vector<int> u_nodes = ext_nozzles;
        std::vector<int> v_nodes = ext_nozzles;

        std::unordered_map<int, int> global_to_local;
        for (size_t i = 0; i < ext_nozzles.size(); ++i)
            global_to_local[ext_nozzles[i]] = static_cast<int>(i);

        std::vector<std::vector<float>> cost_matrix(u_nodes.size(), std::vector<float>(v_nodes.size(), std::numeric_limits<float>::max()));
        std::unordered_map<int, std::vector<int>> uv_unlink_limits;

        for (size_t local_u = 0; local_u < u_nodes.size(); ++local_u) {
            int u_node = u_nodes[local_u];
            std::set<int> unlink_v_local;
            auto u_fils = nozzle_fils[u_node];

            for (auto fil : u_fils) {
                for (auto unplaceable_noz : fil_unplaceable_nozs[fil]) {
                    if (global_to_local.count(unplaceable_noz))
                        unlink_v_local.insert(global_to_local[unplaceable_noz]);
                }
            }
            uv_unlink_limits[static_cast<int>(local_u)].assign(unlink_v_local.begin(), unlink_v_local.end());

            for (size_t local_v = 0; local_v < v_nodes.size(); ++local_v) {
                int v_node = v_nodes[local_v];
                float cost = 0;
                if (unlink_v_local.count(static_cast<int>(local_v))) continue;

                std::optional<unsigned int> v_fil_opt = std::nullopt;
                if (nozzles_state.count(v_node))
                    v_fil_opt = nozzles_state.at(v_node);

                if (!v_fil_opt.has_value() || v_fil_opt.value() >= ctx.model_info.filament_info.size()) {
                    cost = 0;
                } else {
                    int v_fil = v_fil_opt.value();
                    if (std::find(u_fils.begin(), u_fils.end(), v_fil) != u_fils.end())
                        cost = -1;
                    else {
                        for (auto u_fil : u_fils)
                            cost += ctx.model_info.flush_matrix[ext_id][u_fil][v_fil];
                        if (u_fils.size() > 0)
                            cost /= u_fils.size();
                    }
                }
                cost_matrix[local_u][local_v] = cost;
            }
        }

        std::vector<int> local_u_nodes(u_nodes.size());
        std::vector<int> local_v_nodes(v_nodes.size());
        std::iota(local_u_nodes.begin(), local_u_nodes.end(), 0);
        std::iota(local_v_nodes.begin(), local_v_nodes.end(), 0);

        MinFlushFlowSolver solver(cost_matrix, local_u_nodes, local_v_nodes, {}, uv_unlink_limits);
        auto local_match = solver.solve();

        for (size_t local_u = 0; local_u < u_nodes.size(); ++local_u) {
            int global_u = u_nodes[local_u];
            int local_v  = local_match[static_cast<int>(local_u)];
            if (local_v == MaxFlowGraph::INVALID_ID || local_v < 0 || local_v >= static_cast<int>(v_nodes.size()))
                continue;
            int global_v = v_nodes[local_v];
            global_uv_match[global_u] = global_v;
        }
    }

    // 4. Build new group_result
    std::vector<int> new_default_filament_nozzle_maps = group.get_layer_filament_nozzle_map(-1);
    for (auto fil : fils) {
        int ori_noz = new_default_filament_nozzle_maps[fil];
        if (global_uv_match.count(ori_noz))
            new_default_filament_nozzle_maps[fil] = global_uv_match[ori_noz];
    }

    auto new_group = MultiNozzleUtils::LayeredNozzleGroupResult::create(new_default_filament_nozzle_maps, ctx.nozzle_info.nozzle_list, fils);
    if (!new_group.has_value()) new_group = group;
    return *new_group;
}


// H2C port: Hash functor for vector<unsigned int>, used as key in combo-range map.
// BBL ref: BambuStudio/src/libslic3r/GCode/ToolOrdering.cpp (inline helper)
struct VectorHash {
    size_t operator()(const std::vector<unsigned int>& v) const {
        size_t seed = v.size();
        for (auto& elem : v)
            seed ^= std::hash<unsigned int>()(elem) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }
};


// H2C port: Core dynamic GroupReorder — plan filament mapping per combo range.
// BBL ref: BambuStudio/src/libslic3r/GCode/ToolOrdering.cpp:1633-1789
std::vector<FilamentPlanRes> plan_filament_mapping_and_order_by_combo_ranges(
    Print*                                             print,
    const FilamentGroupContext&                        ctx,
    const ToolOrdering::OrderingContext&               order_ctx,
    const FilamentMapMode                              mode,
    const std::vector<std::set<int>>&                  physical_unprintables,
    const std::vector<std::set<int>>&                  geometric_unprintables,
    const std::map<int, std::set<NozzleVolumeType>>&   unprintable_volumes,
    MultiNozzleUtils::NozzleStatusRecorder*            io_nozzle_status)
{
    std::vector<FilamentPlanRes> results;
    const auto& layer_fils = ctx.model_info.layer_filaments;
    if (layer_fils.empty()) return results;

    results.resize(layer_fils.size());

    // Group layers by filament combo -> contiguous ranges
    std::unordered_map<std::vector<unsigned int>, std::vector<std::pair<int, int>>, VectorHash> filament_combo_ranges;
    for (int layer_idx = 0; layer_idx < static_cast<int>(layer_fils.size()); ++layer_idx) {
        std::vector<unsigned int> cur_combo = layer_fils[layer_idx];
        std::sort(cur_combo.begin(), cur_combo.end());
        cur_combo.erase(std::unique(cur_combo.begin(), cur_combo.end()), cur_combo.end());
        if (cur_combo.empty()) continue;

        auto& ranges = filament_combo_ranges[cur_combo];
        if (ranges.empty() || ranges.back().second != layer_idx - 1)
            ranges.emplace_back(layer_idx, layer_idx);
        else
            ranges.back().second = layer_idx;
    }

    std::map<std::pair<int,int>, std::vector<unsigned int>> range_filas_map;
    for (auto& [combo, ranges] : filament_combo_ranges) {
        for (auto& range : ranges)
            range_filas_map[range] = combo;
    }

    std::set<int> used_filaments;
    MultiNozzleUtils::NozzleStatusRecorder tool_status;
    if (io_nozzle_status) tool_status = *io_nozzle_status;

    std::vector<int> fil_noz_map(ctx.group_info.total_filament_num, -1);
    std::unordered_map<int, int> fil_first_nozzle_map;

    for (auto &[range, combo] : range_filas_map) {
        auto [start_layer, end_layer] = range;

        // 1. Build range layer_filaments
        std::vector<std::vector<unsigned int>> range_layer_fils;
        range_layer_fils.reserve(end_layer - start_layer + 1);
        for (int layer_idx = start_layer; layer_idx <= end_layer; ++layer_idx)
            range_layer_fils.push_back(layer_fils[layer_idx]);
        used_filaments.insert(combo.begin(), combo.end());

        // 2. Get group result for this range
        auto nozzle_filament_map = tool_status.get_nozzle_filament_map();
        auto group_result = ToolOrdering::get_recommended_filament_maps(
            print, range_layer_fils, mode, physical_unprintables,
            geometric_unprintables, unprintable_volumes, nozzle_filament_map);

        // 3. Refine by nozzle state
        auto new_group_result = refine_groups_by_Nozzle_State(ctx, group_result, nozzle_filament_map);

        auto range_seq_function = [&order_ctx, start_layer_ = start_layer, end_layer_ = end_layer](int layer_idx, std::vector<int> &out_seq) -> bool {
            if (layer_idx <= end_layer_ - start_layer_) {
                int global_idx = start_layer_ + layer_idx;
                return order_ctx.get_custom_seq(global_idx, out_seq);
            }
            return false;
        };

        // 4. Reorder filaments for this range
        std::vector<std::vector<unsigned int>> fils_sequences;
        reorder_filaments_for_multi_nozzle_extruder(
            range_layer_fils.front(), new_group_result, range_layer_fils,
            ctx.model_info.flush_matrix, range_seq_function, &fils_sequences, tool_status);

        // 5. Store range results
        for (auto fil_id : fils_sequences.back()) {
            auto noz = new_group_result.get_nozzle_for_filament(fil_id);
            if (noz.has_value()) {
                int noz_id = noz->group_id;
                int ext_id = noz->extruder_id;
                fil_noz_map[fil_id] = noz_id;
                fil_first_nozzle_map.emplace(static_cast<int>(fil_id), noz_id);
                tool_status.set_current_extruder_id(ext_id);
                tool_status.set_nozzle_status(noz_id, fil_id, ext_id);
            }
        }

        assert(fils_sequences.size() == range_layer_fils.size());
        for (size_t layer_id = 0; layer_id < fils_sequences.size(); ++layer_id) {
            int g_layer_id                       = start_layer + static_cast<int>(layer_id);
            results[g_layer_id].fil_nozzle_match = fil_noz_map;
            results[g_layer_id].fil_order        = std::vector<int>(fils_sequences[layer_id].begin(), fils_sequences[layer_id].end());
        }
    }

    for (auto& res : results) {
        for (int fil_id = 0; fil_id < (int)res.fil_nozzle_match.size(); fil_id++) {
            auto& noz_id = res.fil_nozzle_match[fil_id];
            if (noz_id == -1)
                noz_id = (used_filaments.count(fil_id) && fil_first_nozzle_map.count(fil_id)) ? fil_first_nozzle_map[fil_id] : 0;
        }
    }

    if (io_nozzle_status) *io_nozzle_status = tool_status;
    return results;
}

} // namespace Slic3r
