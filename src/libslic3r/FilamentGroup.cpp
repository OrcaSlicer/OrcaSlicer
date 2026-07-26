#include "FilamentGroup.hpp"
#include "GCode/ToolOrderUtils.hpp"
#include "FlushVolPredictor.hpp"
#include <queue>
#include <random>
#include <cassert>
#include <sstream>
#include <boost/log/trivial.hpp>

namespace Slic3r
{
    using namespace FilamentGroupUtils;
    static constexpr long long ENUM_THRESHOLD = 10000;
    static constexpr long long ENUM_EARLY_EXIT = 10000000;
    constexpr uint32_t GOLDEN_RATIO_32 = 0x9e3779b9;

    // clear the array and heap,save the groups in heap to the array
    static void change_memoryed_heaps_to_arrays(MemoryedGroupHeap& heap,const int total_filament_num,const std::vector<unsigned int>& used_filaments, std::vector<std::vector<int>>& arrs)
    {
        // switch the label idx
        arrs.clear();
        while (!heap.empty()) {
            auto top = heap.top();
            heap.pop();
            std::vector<int> labels_tmp(total_filament_num, 0);
            for (size_t idx = 0; idx < top.group.size(); ++idx)
                labels_tmp[used_filaments[idx]] = top.group[idx];
            arrs.emplace_back(std::move(labels_tmp));
        }
    }

    static std::unordered_map<int, int> get_merged_filament_map(const std::unordered_map<int, std::vector<int>>& merged_filaments)
    {
        std::unordered_map<int, int> filament_merge_map;
        for (auto elem : merged_filaments) {
            for (auto f : elem.second) {
                //traverse filaments in merged group
                filament_merge_map[f] = elem.first;
            }
        }
        return filament_merge_map;
    }

    static uint64_t fnv_hash_nozzle(int volume_type, int is_right_extruder, int loaded_filament = -1)
    {
        constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
        constexpr uint64_t FNV_PRIME        = 1099511628211ULL;
        constexpr uint64_t SALT_A           = 0xA5A5A5A5A5A5A5A5ULL;
        constexpr uint64_t SALT_B           = 0x5A5A5A5A5A5A5A5AULL;
        constexpr uint64_t SALT_C           = 0x3C3C3C3C3C3C3C3CULL;

        uint64_t h = FNV_OFFSET_BASIS;
        h ^= static_cast<uint64_t>(volume_type) + SALT_A;
        h *= FNV_PRIME;
        h ^= static_cast<uint64_t>(is_right_extruder) + SALT_B;
        h *= FNV_PRIME;
        if (loaded_filament >= 0) {
            h ^= static_cast<uint64_t>(loaded_filament) + SALT_C;
            h *= FNV_PRIME;
        }

        return h;
    }

    static double evaluate_score(const double flush, const double time, const bool with_time = false) {
        if (!with_time) return flush;

        double approx_density = 1.26;    //   g/cm^3
        double approx_flush_speed = 180; //   s/g
        double correction_factor = 2;
        double flush_score = flush * approx_density * approx_flush_speed * correction_factor / 1000;
        return flush_score + time;
    }

    static double calc_change_time_for_group(
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

    static double full_evaluate(
        const std::vector<unsigned int>& used_filaments,
        const std::vector<int>& filament_nozzle_map,
        const FilamentGroupContext& ctx,
        std::optional<std::function<bool(int, std::vector<int>&)>> get_custom_seq = std::nullopt,
        int* out_flush = nullptr)
    {
        auto group_res = MultiNozzleUtils::LayeredNozzleGroupResult::create(filament_nozzle_map, ctx.nozzle_info.nozzle_list, used_filaments);
        if (!group_res) {
            if (out_flush) *out_flush = 0;
            return 0.0;
        }

        MultiNozzleUtils::NozzleStatusRecorder initial_status;
        for (auto& [nozzle_id, filament_id] : ctx.nozzle_info.nozzle_status) {
            if (filament_id >= 0) {
                int extruder_id = 0;
                for (const auto& nozzle : ctx.nozzle_info.nozzle_list) {
                    if (nozzle.group_id == nozzle_id) {
                        extruder_id = nozzle.extruder_id;
                        break;
                    }
                }
                initial_status.set_nozzle_status(nozzle_id, filament_id, extruder_id);
            }
        }

        std::vector<std::vector<unsigned int>> filament_sequences;
        int flush = reorder_filaments_for_multi_nozzle_extruder(
            used_filaments,
            *group_res,
            ctx.model_info.layer_filaments,
            ctx.model_info.flush_matrix,
            get_custom_seq ? *get_custom_seq : std::function<bool(int, std::vector<int>&)>{},
            &filament_sequences,
            initial_status
        );

        if (out_flush) *out_flush = flush;

        double change_time = 0.0;
        if (!filament_sequences.empty()) {
            std::vector<int> filament_change_seq;
            std::vector<int> nozzle_change_seq;
            int prev_fil = -1, prev_nozzle = -1;
            for (const auto& layer_seq : filament_sequences) {
                for (unsigned int fil : layer_seq) {
                    auto nozzle_info = group_res->get_first_nozzle_for_filament(fil);
                    if (!nozzle_info) continue;
                    int nid = nozzle_info->group_id;
                    if ((int)fil == prev_fil && nid == prev_nozzle) continue;
                    filament_change_seq.push_back(static_cast<int>(fil));
                    nozzle_change_seq.push_back(nid);
                    prev_fil = (int)fil;
                    prev_nozzle = nid;
                }
            }

            std::vector<int> logical_filaments(used_filaments.begin(), used_filaments.end());
            std::vector<int> group_of_filament(used_filaments.size(), 0);
            for (size_t fi = 0; fi < used_filaments.size(); ++fi) {
                int nozzle_id = filament_nozzle_map[used_filaments[fi]];
                if (nozzle_id >= 0 && nozzle_id < (int)ctx.nozzle_info.nozzle_list.size())
                    group_of_filament[fi] = ctx.nozzle_info.nozzle_list[nozzle_id].extruder_id;
            }
            change_time = calc_change_time_for_group(
                filament_change_seq,
                nozzle_change_seq,
                logical_filaments,
                ctx.nozzle_info.nozzle_list,
                ctx.speed_info.change_time_params,
                ctx.speed_info.ams_preload_enabled,
                group_of_filament
            );
        }

        double print_time = 0.0;
        if (ctx.speed_info.group_with_time) {
            TimeEvaluator time_evaluator(ctx.speed_info);
            print_time = time_evaluator.get_estimated_time(filament_nozzle_map);
        }

        return evaluate_score(flush, change_time + print_time, true);
    }

    static long long estimate_dedup_enum_count(int k, int n, const FilamentGroupContext& ctx)
    {
        if (n <= 0 || k <= 0) return 0;

        long long total = 1;
        for (int i = 0; i < n; ++i) {
            total *= k;
            if (total > ENUM_EARLY_EXIT) return total;
        }

        if (k <= 1) return total;

        int dedup_factor = 1;
        std::map<uint64_t, int> nozzle_type_count;
        for (const auto& nozzle : ctx.nozzle_info.nozzle_list) {
            auto it = ctx.nozzle_info.nozzle_status.find(nozzle.group_id);
            int loaded_filament = (it != ctx.nozzle_info.nozzle_status.end()) ? it->second : -1;
            uint64_t hash = fnv_hash_nozzle(nozzle.volume_type, nozzle.group_id > 0, loaded_filament);
            nozzle_type_count[hash]++;
        }

        for (auto& [hash, count] : nozzle_type_count) {
            int factorial = 1;
            for (int i = 2; i <= count; ++i) factorial *= i;
            dedup_factor *= factorial;
        }

        return total / std::max(dedup_factor, 1);
    }

    std::vector<int> FilamentGroup::calc_group_by_enum(
        int k,
        const std::vector<unsigned int>& used_filaments,
        const std::unordered_map<int, std::vector<int>>& unplaceable_limits,
        int* cost)
    {
        static constexpr int UNPLACEABLE_LIMIT_REWARD = 10000;
        static constexpr int MAX_SIZE_LIMIT_REWARD = 5000;
        static constexpr int SUPPORT_PREFER_REWARD = 100;
        static constexpr int BEST_FIT_LIMIT_REWARD = 10;

        int n = (int)used_filaments.size();

        std::vector<std::vector<int>> candidates(n);
        for (int i = 0; i < n; i++) {
            std::unordered_set<int> group_set;
            if (auto it = unplaceable_limits.find(i); it != unplaceable_limits.end()) {
                for (int g = 0; g < k; g++) group_set.insert(g);
                for (int g : it->second) group_set.erase(g);
            } else {
                for (int g = 0; g < k; g++) group_set.insert(g);
            }
            candidates[i].assign(group_set.begin(), group_set.end());
        }

        auto vector_equal = [](const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) -> bool {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); i++) {
                if (a[i] != b[i]) return false;
            }
            return true;
        };
        auto vector_hash = [](const std::vector<uint64_t>& v) -> size_t {
            size_t h = 0;
            for (auto val : v) { h ^= val + GOLDEN_RATIO_32 + (h << 6) + (h >> 2); }
            return h;
        };
        std::unordered_set<std::vector<uint64_t>, decltype(vector_hash), decltype(vector_equal)> group_set(0, vector_hash, vector_equal);
        std::vector<uint64_t> group_hashs;

        std::vector<size_t> nozzles_hash(k);
        for (const auto& nozzle : ctx.nozzle_info.nozzle_list) {
            if (nozzle.group_id < k) {
                auto it = ctx.nozzle_info.nozzle_status.find(nozzle.group_id);
                int loaded_filament = (it != ctx.nozzle_info.nozzle_status.end()) ? it->second : -1;
                nozzles_hash[nozzle.group_id] = fnv_hash_nozzle(nozzle.volume_type, nozzle.group_id > 0, loaded_filament);
            }
        }

        std::vector<int> best_full_map(ctx.group_info.total_filament_num, ctx.machine_info.master_extruder_id);
        double best_score = std::numeric_limits<double>::max();
        int best_prefer_level = 0;
        int best_flush = 0;

        const long long total = (long long)std::pow(k, n);
        for (long long mask = 0; mask < total; mask++) {
            long long num = mask;
            std::unordered_map<int, std::vector<int>> nozzles_filaments;
            std::vector<int> groups_count(k, 0);
            std::vector<int> used_labels(n, 0);

            for (int i = 0; i < n; i++) {
                int g_id = num % k;
                num /= k;
                used_labels[i] = g_id;
                nozzles_filaments[g_id].emplace_back(i);
                groups_count[g_id]++;
            }

            // Hash dedup
            group_hashs.clear();
            for (auto& nf : nozzles_filaments) {
                uint64_t filament_mask = 0;
                for (int filament : nf.second) filament_mask |= (1ULL << filament);
                size_t gh = nozzles_hash[nf.first];
                gh ^= (std::hash<uint64_t>{}(filament_mask) + GOLDEN_RATIO_32 + (gh << 6) + (gh >> 2));
                group_hashs.emplace_back(gh);
            }
            std::sort(group_hashs.begin(), group_hashs.end());
            if (group_set.find(group_hashs) != group_set.end()) continue;
            group_set.insert(group_hashs);

            // Prefer level
            int prefer_level = 0;
            int placeable_count = 0;
            for (int i = 0; i < n; i++) {
                if (std::find(candidates[i].begin(), candidates[i].end(), used_labels[i]) != candidates[i].end())
                    placeable_count++;
            }
            prefer_level += placeable_count * UNPLACEABLE_LIMIT_REWARD;

            bool size_ok = true;
            for (int g = 0; g < k; g++) {
                if (g < (int)ctx.machine_info.max_group_size.size() && groups_count[g] > ctx.machine_info.max_group_size[g])
                    size_ok = false;
            }
            if (size_ok)
                prefer_level += MAX_SIZE_LIMIT_REWARD;

            if (ctx.group_info.strategy == FGStrategy::BestFit) {
                bool all_full = true;
                for (int g = 0; g < k; g++) {
                    if (g < (int)ctx.machine_info.max_group_size.size() && groups_count[g] < ctx.machine_info.max_group_size[g])
                        all_full = false;
                }
                if (all_full)
                    prefer_level += BEST_FIT_LIMIT_REWARD;
            }

            for (int g = 0; g < k; g++) {
                if (g < (int)ctx.machine_info.prefer_non_model_filament.size() && ctx.machine_info.prefer_non_model_filament[g]) {
                    for (int fidx : nozzles_filaments[g]) {
                        if (ctx.model_info.filament_info[used_filaments[fidx]].usage_type == SupportOnly)
                            prefer_level += SUPPORT_PREFER_REWARD;
                    }
                }
            }

            // Build full map and evaluate
            std::vector<int> full_map(ctx.group_info.total_filament_num, ctx.machine_info.master_extruder_id);
            for (int i = 0; i < n; ++i)
                full_map[used_filaments[i]] = used_labels[i];

            int flush_vol = 0;
            double score = full_evaluate(used_filaments, full_map, ctx, get_custom_seq, &flush_vol);

            int master_ex_id = ctx.machine_info.master_extruder_id;
            if (master_ex_id < k && groups_count[master_ex_id] < (int)(used_filaments.size() + 1) / 2)
                score += ABSOLUTE_FLUSH_GAP_TOLERANCE;

            if (prefer_level > best_prefer_level || (prefer_level == best_prefer_level && score < best_score)) {
                best_score = score;
                best_prefer_level = prefer_level;
                best_full_map = full_map;
                best_flush = flush_vol;
            }

            MemoryedGroup mg(used_labels, score, prefer_level);
            update_memoryed_groups(mg, ctx.group_info.max_gap_threshold, m_memoryed_heap);
        }

        if (cost) *cost = best_flush;
        return best_full_map;
    }

    std::vector<int> FilamentGroup::calc_group_by_kmedoids(
        int k,
        const std::vector<unsigned int>& used_filaments,
        const std::unordered_map<int, std::vector<int>>& unplaceable_limits,
        int* cost)
    {
        auto distance_evaluator = std::make_shared<FlushDistanceEvaluator>(ctx.model_info.flush_matrix, used_filaments, ctx.model_info.layer_filaments);
        KMediods PAM(k, (int)used_filaments.size(), distance_evaluator, ctx.machine_info.master_extruder_id);
        PAM.set_unplacable_limits(unplaceable_limits);
        PAM.set_memory_threshold(ctx.group_info.max_gap_threshold);

        std::vector<std::pair<std::set<int>, int>> cluster_size_limit;
        for (auto& [extruder_id, nozzles] : ctx.nozzle_info.extruder_nozzle_list) {
            std::pair<std::set<int>, int> clusters;
            clusters.first = std::set<int>(nozzles.begin(), nozzles.end());
            clusters.second = ctx.machine_info.max_group_size.at(extruder_id);
            cluster_size_limit.emplace_back(clusters);
        }
        PAM.set_cluster_group_size(cluster_size_limit);

        PAM.do_clustering(ctx, m_clustering_budget);

        m_memoryed_heap = PAM.get_memoryed_groups();

        auto labels = PAM.get_cluster_labels();
        std::vector<int> full_map(ctx.group_info.total_filament_num, ctx.machine_info.master_extruder_id);
        for (int i = 0; i < (int)labels.size(); ++i)
            full_map[used_filaments[i]] = labels[i];

        if (cost) {
            int flush_vol = 0;
            full_evaluate(used_filaments, full_map, ctx, get_custom_seq, &flush_vol);
            *cost = flush_vol;
        }

        return full_map;
    }

    /**
     * @brief Select the group that best fit the filaments in AMS
     *
     * Calculate the total color distance between the grouping results and the AMS filaments through
     * minimum cost maximum flow. Only those with a distance difference within the threshold are
     * considered valid.
     *
     * @param map_lists Group list with similar flush count
     * @param nozzle_lists nozzle_id -> extruder_id
     * @param used_filaments Idx of used filaments
     * @param used_filament_info Information of filaments used
     * @param machine_filament_info Information of filaments loaded in printer
     * @param color_threshold Threshold for considering colors to be similar
     * @return The group that best fits the filament distribution in AMS
     */
    std::vector<int> select_best_group_for_ams(const std::vector<std::vector<int>>& filament_to_nozzles,
        const std::vector<MultiNozzleUtils::NozzleInfo>& nozzle_list,
        const std::vector<unsigned int>& used_filaments,
        const std::vector<FilamentGroupUtils::FilamentInfo>& used_filament_info,
        const std::vector<std::vector<MachineFilamentInfo>>& machine_filament_info_,
        const bool has_filament_switcher,
        const double color_threshold)
    {
        using namespace FlushPredict;

        if (has_filament_switcher)
            return filament_to_nozzles.size() ? filament_to_nozzles.front() : std::vector<int>();

        const int fail_cost = 9999;

        // these code is to make we machine filament info size is 2
        std::vector<std::vector<MachineFilamentInfo>> machine_filament_info = machine_filament_info_;
        machine_filament_info.resize(2);

        int best_cost = std::numeric_limits<int>::max();
        std::vector<int>best_map;

        for (auto &filament_to_nozzle : filament_to_nozzles) {
            std::vector<std::vector<int>> group_filaments(2);
            std::vector<std::vector<Color>>group_colors(2);

            for (size_t i = 0; i < used_filaments.size(); ++i) {
                auto &nozzle       = nozzle_list[filament_to_nozzle[used_filaments[i]]];
                int   target_group = nozzle.extruder_id == 0 ? 0 : 1;
                group_colors[target_group].emplace_back(used_filament_info[i].color);
                group_filaments[target_group].emplace_back(i);
            }

            int group_cost = 0;
            for (size_t i = 0; i < 2; ++i) {
                if (group_colors[i].empty())
                    continue;
                if (machine_filament_info[i].empty()) {
                    group_cost += group_colors.size() * fail_cost;
                    continue;
                }
                std::vector<std::vector<float>>distance_matrix(group_colors[i].size(), std::vector<float>(machine_filament_info[i].size()));

                // calculate color distance matrix
                for (size_t src = 0; src < group_colors[i].size(); ++src) {
                    for (size_t dst = 0; dst < machine_filament_info[i].size(); ++dst) {
                        distance_matrix[src][dst] = calc_color_distance(
                            RGBColor(group_colors[i][src].r, group_colors[i][src].g, group_colors[i][src].b),
                            RGBColor(machine_filament_info[i][dst].color.r, machine_filament_info[i][dst].color.g, machine_filament_info[i][dst].color.b)
                        );
                    }
                }

                // get min cost by min cost max flow
                std::vector<int>l_nodes(group_colors[i].size()), r_nodes(machine_filament_info[i].size());
                std::iota(l_nodes.begin(), l_nodes.end(), 0);
                std::iota(r_nodes.begin(), r_nodes.end(), 0);

                std::unordered_map<int, std::vector<int>>unlink_limits;
                for (size_t from = 0; from < group_filaments[i].size(); ++from) {
                    for (size_t to = 0; to < machine_filament_info[i].size(); ++to) {
                        if (used_filament_info[group_filaments[i][from]].type != machine_filament_info[i][to].type ||
                            used_filament_info[group_filaments[i][from]].is_support != machine_filament_info[i][to].is_support) {
                            unlink_limits[from].emplace_back(to);
                        }
                    }
                }

                MatchModeGroupSolver mcmf(distance_matrix, l_nodes, r_nodes, std::vector<int>(r_nodes.size(), l_nodes.size()), unlink_limits);
                auto ams_map = mcmf.solve();

                for (size_t idx = 0; idx < ams_map.size(); ++idx) {
                    if (ams_map[idx] == MaxFlowGraph::INVALID_ID || distance_matrix[idx][ams_map[idx]] > color_threshold) {
                        group_cost += fail_cost;
                    }
                    else {
                        group_cost += distance_matrix[idx][ams_map[idx]];
                    }
                }
            }

            if (best_map.empty() || group_cost < best_cost) {
                best_cost = group_cost;
                best_map  = filament_to_nozzle;
            }
        }

        return best_map;
    }


    void FilamentGroupUtils::update_memoryed_groups(const MemoryedGroup& item, const double gap_threshold, MemoryedGroupHeap& groups)
    {
        auto emplace_if_accepatle = [gap_threshold](MemoryedGroupHeap& heap, const MemoryedGroup& elem, const MemoryedGroup& best) {
            if (best.cost == 0) {
                if (std::abs(elem.cost - best.cost) <= ABSOLUTE_FLUSH_GAP_TOLERANCE)
                    heap.push(elem);
                return;
            }
            double gap_rate = (double)std::abs(elem.cost - best.cost) / (double)best.cost;
            if (gap_rate <= gap_threshold)
                heap.push(elem);
            };

        if (groups.empty()) {
            groups.push(item);
        }
        else {
            auto top = groups.top();
            // we only memory items with the highest prefer level
            if (top.prefer_level > item.prefer_level)
                return;
            else if (top.prefer_level == item.prefer_level) {
                if (top.cost <= item.cost) {
                    emplace_if_accepatle(groups, item, top);
                }
                // find a group with lower cost, rebuild the heap
                else {
                    MemoryedGroupHeap new_heap;
                    new_heap.push(item);
                    while (!groups.empty()) {
                        auto top = groups.top();
                        groups.pop();
                        emplace_if_accepatle(new_heap, top, item);
                    }
                    groups = std::move(new_heap);
                }
            }
            // find a group with the higher prefer level, rebuild the heap
            else {
                groups = MemoryedGroupHeap();
                groups.push(item);
            }
        }
    }

    std::vector<unsigned int> collect_sorted_used_filaments(const std::vector<std::vector<unsigned int>>& layer_filaments)
    {
        std::set<unsigned int>used_filaments_set;
        for (const auto& lf : layer_filaments)
            for (const auto& f : lf)
                used_filaments_set.insert(f);
        std::vector<unsigned int>used_filaments(used_filaments_set.begin(), used_filaments_set.end());
        sort_remove_duplicates(used_filaments);
        return used_filaments;
    }

    FlushDistanceEvaluator::FlushDistanceEvaluator(const std::vector<FlushMatrix>& flush_matrix, const std::vector<unsigned int>& used_filaments, const std::vector<std::vector<unsigned int>>& layer_filaments, double p)
    {
        //calc pair counts
        std::vector<std::vector<int>>count_matrix(used_filaments.size(), std::vector<int>(used_filaments.size()));
        for (const auto& lf : layer_filaments) {
            for (auto iter = lf.begin(); iter != lf.end(); ++iter) {
                auto id_iter1 = std::find(used_filaments.begin(), used_filaments.end(), *iter);
                if (id_iter1 == used_filaments.end())
                    continue;
                auto idx1 = id_iter1 - used_filaments.begin();
                for (auto niter = std::next(iter); niter != lf.end(); ++niter) {
                    auto id_iter2 = std::find(used_filaments.begin(), used_filaments.end(), *niter);
                    if (id_iter2 == used_filaments.end())
                        continue;
                    auto idx2 = id_iter2 - used_filaments.begin();
                    count_matrix[idx1][idx2] += 1;
                    count_matrix[idx2][idx1] += 1;
                }
            }
        }

        m_distance_matrix.resize(flush_matrix.size(), std::vector<std::vector<float>>(used_filaments.size(), std::vector<float>(used_filaments.size())));

        for (size_t i = 0; i < used_filaments.size(); ++i) {
            for (size_t j = 0; j < used_filaments.size(); ++j) {
                for (size_t k = 0; k < flush_matrix.size(); k++) {
                    if (i == j)
                        m_distance_matrix[k][i][j] = 0;
                    else {
                    //TODO: check m_flush_matrix
                        float max_val = std::max(flush_matrix[k][used_filaments[i]][used_filaments[j]], flush_matrix[k][used_filaments[j]][used_filaments[i]]);
                        float min_val = std::min(flush_matrix[k][used_filaments[i]][used_filaments[j]], flush_matrix[k][used_filaments[j]][used_filaments[i]]);
                        m_distance_matrix[k][i][j] = (max_val * p + min_val * (1 - p)) * (std::max(count_matrix[i][j], 1));
                    }
                }
            }
        }
    }

    double FlushDistanceEvaluator::get_distance(int idx_a, int idx_b, int extruder_id) const
    {
        assert(0 <= idx_a && idx_a < m_distance_matrix[extruder_id].size());
        assert(0 <= idx_b && idx_b < m_distance_matrix[extruder_id].size());

        return m_distance_matrix[extruder_id][idx_a][idx_b];
    }

    double TimeEvaluator::get_estimated_time(const std::vector<int>& filament_map) const
    {
        double time = 0;
        for(auto &elem : m_speed_info.filament_print_time){
            int filament_idx = elem.first;
            auto extruder_time = elem.second;
            int filament_extruder_id = filament_map[filament_idx];
            time += extruder_time[filament_extruder_id];
        }
        return time;
    }



    void KMediods::set_cluster_group_size(const std::vector<std::pair<std::set<int>, int>> &cluster_group_size)
    {
        m_cluster_group_size = cluster_group_size;
        m_nozzle_to_extruder.resize(m_k, 0);
        for (int i = 0; i < m_cluster_group_size.size(); i++) {
            for (auto nozzle_id : m_cluster_group_size[i].first) m_nozzle_to_extruder[nozzle_id] = i;
        }
    }

    int KMediods::calc_cost(const std::vector<int>& cluster_labels, const std::vector<int>& cluster_centers,int cluster_id)
    {
        assert(m_evaluator);
        int total_cost = 0;

        std::vector<std::pair<int, double>> nozzle_cost(m_k,{0,0.0});
        std::vector<int> nozzle_filaments(m_k, 0);
        for (int i = 0; i < m_elem_count; ++i) {
            if (cluster_id != -1 && cluster_labels[i] != cluster_id)
                continue;
            if (cluster_centers[cluster_labels[i]] == -1)
                continue;

            nozzle_filaments[cluster_labels[i]]++;
            for (int j = i + 1; j < m_elem_count; ++j) {
                int nozzle_i = cluster_labels[i];
                int nozzle_j = cluster_labels[j];
                if (nozzle_i == nozzle_j) {
                    nozzle_cost[nozzle_i].first++;
                    nozzle_cost[nozzle_j].second += m_evaluator->get_distance(i, j, m_nozzle_to_extruder[nozzle_i]);
                }
            }
        }
        for (size_t i = 0; i < nozzle_cost.size(); ++i) {
            if (nozzle_filaments[i] > 0 && nozzle_cost[i].second > 0)
                total_cost += nozzle_cost[i].second / nozzle_cost[i].first * (nozzle_filaments[i] - 1);
        }

        return total_cost;
    }

    bool KMediods::have_enough_size(const std::vector<int>& cluster_size, const std::vector<std::pair<std::set<int>, int>>& cluster_group_size,int elem_count)
    {
        bool have_enough_size = true;
        std::optional<int>cluster_sum;
        std::optional<int>cluster_group_sum;

        if (!cluster_size.empty())
            cluster_sum = std::accumulate(cluster_size.begin(), cluster_size.end(), 0);
        if (!cluster_group_size.empty())
            cluster_group_sum = std::accumulate(cluster_group_size.begin(), cluster_group_size.end(), 0, [](int a, const std::pair<std::set<int>, int>& p) {return a + p.second; });
        if (cluster_sum.has_value())
            have_enough_size &= (cluster_sum >= elem_count);
        if (cluster_group_sum.has_value())
            have_enough_size &= (cluster_group_sum >= elem_count);
        return have_enough_size;
    }


    // make sure each cluster has at least one element
    std::vector<int> KMediods::init_cluster_center(const std::unordered_map<int, std::vector<int>>& placeable_limits, const std::unordered_map<int, std::vector<int>>& unplaceable_limits,const std::vector<int>& cluster_size,const std::vector<std::pair<std::set<int>,int>>& cluster_group_size, int seed)
    {
        // max flow network
        std::vector<int> l_nodes(m_elem_count); // represent the filament idx, to be shuffled
        std::vector<int> r_nodes(m_k); // represent the group idx
        std::iota(l_nodes.begin(), l_nodes.end(), 0);
        std::iota(r_nodes.begin(), r_nodes.end(), 0);

        std::unordered_map<int, std::vector<int>> shuffled_placeable_limits;
        std::unordered_map<int, std::vector<int>> shuffled_unplaceable_limits;
        // shuffle the filaments and transfer placeable,unplaceable limits
        {
            std::mt19937 rng(seed);
            std::shuffle(l_nodes.begin(), l_nodes.end(), rng);

            std::unordered_map<int, int>idx_transfer;
            for (size_t idx = 0; idx < l_nodes.size(); ++idx){
                int new_idx = std::find(l_nodes.begin(),l_nodes.end(), idx) - l_nodes.begin();
                idx_transfer[idx] = new_idx;
            }
            for (auto& elem : placeable_limits)
                shuffled_placeable_limits[idx_transfer[elem.first]] = elem.second;
            for (auto& elem : unplaceable_limits)
                shuffled_unplaceable_limits[idx_transfer[elem.first]] = elem.second;
        }


        MaxFlowSolver M(l_nodes, r_nodes, shuffled_placeable_limits, shuffled_unplaceable_limits);
        auto ret = M.solve();

        // A remaining -1 means some filaments cannot be placed under the limit. We ignore the -1 here since we
        // are deciding the cluster center; the -1 can be handled in later steps.
        std::vector<int> cluster_center(m_k, -1);
        for (size_t idx = 0; idx < ret.size(); ++idx) {
            if (ret[idx] != -1) {
                cluster_center[ret[idx]] = l_nodes[idx];
            }
        }

        return cluster_center;
    }

    std::vector<int> KMediods::assign_cluster_label(const std::vector<int>& center, const std::unordered_map<int, std::vector<int>>& placeable_limits, const std::unordered_map<int, std::vector<int>>& unplaceable_limits, const std::vector<int>& cluster_size, const std::vector<std::pair<std::set<int>, int>>& cluster_group_size)
    {
        std::vector<int> labels(m_elem_count, -1);
        std::vector<int> l_nodes(m_elem_count);
        std::vector<int> r_nodes(m_k);
        std::iota(l_nodes.begin(), l_nodes.end(), 0);
        std::iota(r_nodes.begin(), r_nodes.end(), 0);

        std::vector<std::vector<float>> distance_matrix(m_elem_count, std::vector<float>(m_k));
        for (int i = 0; i < m_elem_count; ++i) {
            for (int j = 0; j < m_k; ++j) {
                if (center[j] == -1)
                    distance_matrix[i][j] = static_cast<float>(MaxFlowGraph::MCMF_MAX_EDGE_COST);
                else
                    distance_matrix[i][j] = m_evaluator->get_distance(i, center[j], m_nozzle_to_extruder[j]);
            }
        }

        // only consider the size limit if the group can contain all of the filaments
        std::vector<int> r_nodes_capacity = {};
        std::vector<std::pair<std::set<int>, int>> r_nodes_group_capacity = {};
        if (have_enough_size(cluster_size, cluster_group_size, m_elem_count)) {
            r_nodes_capacity = cluster_size;
            r_nodes_group_capacity = cluster_group_size;
        }
        else {
            // TODO: throw exception here?
            // adjust group size to elem count if the group cannot contain all of the filaments
            r_nodes_capacity = std::vector<int>(m_k, m_elem_count);
        }
        std::vector<int> l_nodes_capacity(l_nodes.size(),1);
        //for (size_t idx = 0; idx < center.size(); ++idx)
        //    if (center[idx] != -1)
        //        l_nodes_capacity[center[idx]] = 0;


        // Each group can receive up to m_elem_count materials at most, so the flow from r_nodes to sink is adjusted to m_elem_count.
        MinFlushFlowSolver M(distance_matrix, l_nodes, r_nodes, placeable_limits, unplaceable_limits, l_nodes_capacity, r_nodes_capacity, r_nodes_group_capacity);
        auto ret = M.solve();

        for (size_t idx = 0; idx < ret.size(); ++idx) {
            if (ret[idx] != MaxFlowGraph::INVALID_ID) {
                labels[l_nodes[idx]] = r_nodes[ret[idx]];
            }
        }

        for (size_t idx = 0; idx < center.size(); ++idx)
            if (center[idx] != -1)
                assert(labels[center[idx]] == idx);

        //for (size_t idx = 0; idx < center.size(); ++idx) {
        //    if (center[idx] != -1) {
        //        labels[center[idx]] = idx;
        //    }
        //}

        // If there are materials that have not been grouped in the last step, assign them to a valid group.
        for (size_t idx = 0; idx < labels.size(); ++idx) {
            if (labels[idx] == -1) {
                int fallback = m_default_group_id;
                auto it = unplaceable_limits.find(static_cast<int>(idx));
                if (it != unplaceable_limits.end()) {
                    for (int nid = 0; nid < m_k; ++nid) {
                        if (std::find(it->second.begin(), it->second.end(), nid) == it->second.end()) {
                            fallback = nid;
                            break;
                        }
                    }
                }
                labels[idx] = fallback;
            }
        }

        return labels;
    }

    /*
    1.Select initial medoids randomly
    2.Iterate while the cost decreases:
      2.1 In each cluster, make the point that minimizes the sum of distances within the cluster the medoid
      2.2 Reassign each point to the cluster defined by the closest medoid determined in the previous step
    */
    void KMediods::do_clustering(const FilamentGroupContext& context, const ClusteringBudget& budget)
    {
        FlushTimeMachine T;
        T.time_machine_start();

        const std::vector<unsigned int> used_filaments = collect_sorted_used_filaments(context.model_info.layer_filaments);

        auto build_full_map = [&](const std::vector<int>& labels) -> std::vector<int> {
            std::vector<int> full_map(context.group_info.total_filament_num, m_default_group_id);
            for (int i = 0; i < (int)labels.size(); ++i)
                full_map[used_filaments[i]] = labels[i];
            return full_map;
        };

        auto evaluate_labels = [&](const std::vector<int>& labels) -> double {
            auto full_map = build_full_map(labels);
            return full_evaluate(used_filaments, full_map, context);
        };

        std::vector<int> best_cluster_centers = std::vector<int>(m_k, 0);
        std::vector<int> best_cluster_labels  = std::vector<int>(m_elem_count, m_default_group_id);
        double           best_cluster_cost    = std::numeric_limits<double>::max();
        int              retry_count          = 0;

        // Run at least one restart; otherwise every filament would stay in the default group.
        const int retry = std::max(1, budget.max_restarts);
        auto within_budget = [&]() { return budget.timeout_ms <= 0 || T.time_machine_end() < budget.timeout_ms; };

        while (retry_count < retry && within_budget()) {
            std::vector<int> curr_cluster_centers = init_cluster_center(m_placeable_limits, m_unplaceable_limits, m_max_cluster_size, m_cluster_group_size, retry_count);
            std::vector<int> curr_cluster_labels = assign_cluster_label(curr_cluster_centers, m_placeable_limits, m_unplaceable_limits, m_max_cluster_size, m_cluster_group_size);
            double           curr_cluster_cost   = evaluate_labels(curr_cluster_labels);

            MemoryedGroup g(curr_cluster_labels, curr_cluster_cost, 1);
            update_memoryed_groups(g, memory_threshold, memoryed_groups);

            bool mediods_changed = true;
            while (mediods_changed && within_budget()) {
                mediods_changed        = false;
                double best_swap_cost  = curr_cluster_cost;
                int best_swap_cluster  = -1;
                int best_swap_elem     = -1;

                for (size_t cluster_id = 0; cluster_id < m_k; ++cluster_id) {
                    if (curr_cluster_centers[cluster_id] == -1) continue;
                    for (int elem = 0; elem < m_elem_count; ++elem) {
                        if (std::find(curr_cluster_centers.begin(), curr_cluster_centers.end(), elem) != curr_cluster_centers.end() ||
                            std::find(m_unplaceable_limits[cluster_id].begin(), m_unplaceable_limits[cluster_id].end(), elem) != m_unplaceable_limits[cluster_id].end())
                            continue;
                        std::vector<int> tmp_centers = curr_cluster_centers;
                        tmp_centers[cluster_id]      = elem;
                        std::vector<int> tmp_labels  = assign_cluster_label(tmp_centers, m_placeable_limits, m_unplaceable_limits, m_max_cluster_size, m_cluster_group_size);
                        double           tmp_cost    = evaluate_labels(tmp_labels);

                        if (tmp_cost < best_swap_cost) {
                            best_swap_cost    = tmp_cost;
                            best_swap_cluster = cluster_id;
                            best_swap_elem    = elem;
                            mediods_changed   = true;
                        }
                    }
                }

                if (mediods_changed) {
                    curr_cluster_centers[best_swap_cluster] = best_swap_elem;
                    curr_cluster_labels = assign_cluster_label(curr_cluster_centers, m_placeable_limits, m_unplaceable_limits, m_max_cluster_size, m_cluster_group_size);
                    curr_cluster_cost   = evaluate_labels(curr_cluster_labels);

                    MemoryedGroup g(curr_cluster_labels, curr_cluster_cost, 1);
                    update_memoryed_groups(g, memory_threshold, memoryed_groups);
                }
            }

            if (curr_cluster_cost < best_cluster_cost) {
                best_cluster_centers = curr_cluster_centers;
                best_cluster_cost    = curr_cluster_cost;
                best_cluster_labels  = curr_cluster_labels;
            }
            retry_count += 1;
        }
        m_cluster_labels = best_cluster_labels;
    }

    std::vector<int> FilamentGroup::calc_min_flush_group(int* cost)
    {
        auto used_filaments = collect_sorted_used_filaments(ctx.model_info.layer_filaments);
        int n = (int)used_filaments.size();
        int k = (int)ctx.nozzle_info.nozzle_list.size();

        std::unordered_map<int, std::vector<int>> unplaceable_limits;
        extract_unprintable_limit_indices(ctx.model_info.unprintable_filaments, used_filaments, unplaceable_limits);
        unplaceable_limits = rebuild_nozzle_unprintables(used_filaments, unplaceable_limits, ctx.group_info.filament_volume_map);

        m_memoryed_heap = MemoryedGroupHeap();
        std::vector<int> result;

        long long estimated = estimate_dedup_enum_count(k, n, ctx);
        if (estimated < ENUM_THRESHOLD)
            result = calc_group_by_enum(k, used_filaments, unplaceable_limits, cost);
        else
            result = calc_group_by_kmedoids(k, used_filaments, unplaceable_limits, cost);

        change_memoryed_heaps_to_arrays(m_memoryed_heap, ctx.group_info.total_filament_num, used_filaments, m_memoryed_groups);

        return result;
    }

    std::map<int, int> FilamentGroup::rebuild_unprintables(const std::vector<unsigned int>& used_filaments, const std::map<int, int>& extruder_unprintables)
    {
        std::map<int, int> ret;
        for (int f_idx = 0; f_idx < used_filaments.size(); f_idx++) {
            int unprintable_ext = -1;
            if (extruder_unprintables.find(f_idx) != extruder_unprintables.end()) {
                unprintable_ext = extruder_unprintables.at(f_idx);
            }

            bool multi_unprintable = false;
            auto unprintable_volumes = ctx.model_info.unprintable_volumes[used_filaments[f_idx]];
            for (int nozzle_idx = 0; nozzle_idx != ctx.nozzle_info.nozzle_list.size(); nozzle_idx++) {
                auto nozzle_info = ctx.nozzle_info.nozzle_list[nozzle_idx];

                if (unprintable_volumes.count(nozzle_info.volume_type)) {
                    if (unprintable_ext == -1)
                        unprintable_ext = nozzle_info.extruder_id;
                    else if (unprintable_ext != nozzle_info.extruder_id)
                        multi_unprintable = true;
                }
            }

            if (!multi_unprintable && unprintable_ext != -1) ret[f_idx] = unprintable_ext;

        }
        return ret;
    }

    std::unordered_map<int, std::vector<int>> FilamentGroup::try_merge_filaments()
    {
        std::unordered_map<int, std::vector<int>>merged_filaments;

        std::unordered_map<std::string, std::vector<int>> merge_filament_map;

        auto unprintable_stat_to_str = [unprintable_filaments = this->ctx.model_info.unprintable_filaments](int idx) {
            std::string str;
            for (size_t eid = 0; eid < unprintable_filaments.size(); ++eid) {
                if (unprintable_filaments[eid].count(idx)) {
                    if (eid > 0)
                        str += ',';
                    str += std::to_string(idx);
                }
            }
            return str;
            };

        for (size_t idx = 0; idx < ctx.model_info.filament_ids.size(); ++idx) {
            std::string id = ctx.model_info.filament_ids[idx];
            Color color = ctx.model_info.filament_info[idx].color;
            std::string unprintable_str = unprintable_stat_to_str(idx);

            std::string key = id + "," + color.to_hex_str(true) + "," + unprintable_str;
            merge_filament_map[key].push_back(idx);
        }

        for (auto& elem : merge_filament_map) {
            if (elem.second.size() > 1) {
                merged_filaments[elem.second.front()] = elem.second;
            }
        }
        return merged_filaments;
    }

    std::vector<int> FilamentGroup::seperate_merged_filaments(const std::vector<int>& filament_map, const std::unordered_map<int, std::vector<int>>& merged_filaments)
    {
        std::vector<int> ret_map = filament_map;
        for (auto& elem : merged_filaments) {
            int src = elem.first;
            for (auto f : elem.second) {
                ret_map[f] = ret_map[src];
            }
        }
        return ret_map;
    }

    void  FilamentGroup::rebuild_context(const std::unordered_map<int, std::vector<int>>& merged_filaments)
    {
        if (merged_filaments.empty())
            return;

        FilamentGroupContext new_ctx = ctx;

        std::unordered_map<int, int> filament_merge_map = get_merged_filament_map(merged_filaments);

        // modify layer filaments
        for (auto& layer_filament : new_ctx.model_info.layer_filaments) {
            for (auto& f : layer_filament) {
                if (auto iter = filament_merge_map.find((int)(f)); iter != filament_merge_map.end()) {
                    f = iter->second;
                }
            }
        }

        for (auto& unprintables : new_ctx.model_info.unprintable_filaments) {
            std::set<int> new_unprintables;
            for (auto f : unprintables) {
                if (auto iter = filament_merge_map.find((int)(f)); iter != filament_merge_map.end()) {
                    new_unprintables.insert(iter->second);
                }
                else {
                    new_unprintables.insert(f);
                }
            }
        }

        ctx = new_ctx;
        return;
    }



    std::vector<int> FilamentGroup::calc_filament_group(int* cost)
    {
        /*auto extruder_variant_list = ctx.nozzle_info.extruder_nozzle_list;
        for (auto nozzle : ctx.nozzle_info.nozzle_list)
            if (nozzle.volume_type == NozzleVolumeType::nvtTPUHighFlow)
                return calc_filament_group_for_tpu(cost);*/

        try {
            if (FGMode::MatchMode == ctx.group_info.mode)
                return calc_filament_group_for_match(cost);
        }
        catch (const FilamentGroupException& e) {
        }

        return calc_filament_group_for_flush(cost);
    }

    std::vector<int> FilamentGroup::calc_filament_group_for_match(int* cost)
    {
        using namespace FlushPredict;
        constexpr int SupportPreferScore = 3;

        auto used_filaments = collect_sorted_used_filaments(ctx.model_info.layer_filaments);
        std::vector<FilamentGroupUtils::FilamentInfo> used_filament_list;
        for (auto f : used_filaments)
            used_filament_list.emplace_back(ctx.model_info.filament_info[f]);

        std::vector<MachineFilamentInfo> machine_filament_list;
        std::map<MachineFilamentInfo, std::set<int>> machine_filament_set;
        for (size_t eid = 0; eid < ctx.machine_info.machine_filament_info.size();++eid) {
            for (auto& filament : ctx.machine_info.machine_filament_info[eid]) {
                machine_filament_set[filament].insert(machine_filament_list.size());
                machine_filament_list.emplace_back(filament);
            }
        }

        if (machine_filament_list.empty())
            throw FilamentGroupException(FilamentGroupException::EmptyAmsFilaments,"Empty ams filament in For-Match mode.");

        std::map<int, int> unprintable_limit_indices; // key stores filament idx in used_filament, value stores unprintable extruder
        extract_unprintable_limit_indices(ctx.model_info.unprintable_filaments, used_filaments, unprintable_limit_indices);
        unprintable_limit_indices = rebuild_unprintables(used_filaments, unprintable_limit_indices);

        std::vector<std::vector<float>> color_dist_matrix(used_filament_list.size(), std::vector<float>(machine_filament_list.size()));
        for (size_t i = 0; i < used_filament_list.size(); ++i) {
            for (size_t j = 0; j < machine_filament_list.size(); ++j) {
                color_dist_matrix[i][j] = calc_color_distance(
                    RGBColor(used_filament_list[i].color.r, used_filament_list[i].color.g, used_filament_list[i].color.b),
                    RGBColor(machine_filament_list[j].color.r, machine_filament_list[j].color.g, machine_filament_list[j].color.b)
                );
            }
        }

        std::vector<int>l_nodes(used_filaments.size());
        std::iota(l_nodes.begin(), l_nodes.end(), 0);
        std::vector<int>r_nodes(machine_filament_list.size());
        std::iota(r_nodes.begin(), r_nodes.end(), 0);
        std::vector<int>machine_filament_capacity(machine_filament_list.size(),l_nodes.size());
        std::vector<int>extruder_filament_count(2, 0);

        auto is_extruder_filament_compatible = [&unprintable_limit_indices](int filament_idx, int extruder_id) {
            auto iter = unprintable_limit_indices.find(filament_idx);
            if (iter != unprintable_limit_indices.end() && iter->second == extruder_id)
                return false;
            return true;
            };

        auto build_unlink_limits = [](const std::vector<int>& l_nodes, const std::vector<int>& r_nodes, const std::function<bool(int, int)>& can_link) {
            std::unordered_map<int, std::vector<int>> unlink_limits;
            for (size_t i = 0; i < l_nodes.size(); ++i) {
                std::vector<int> unlink_filaments;
                for (size_t j = 0; j < r_nodes.size(); ++j) {
                    if (!can_link(l_nodes[i], r_nodes[j]))
                        unlink_filaments.emplace_back(j);
                }
                if (!unlink_filaments.empty())
                    unlink_limits.emplace(i, std::move(unlink_filaments));
            }
            return unlink_limits;
            };

        auto optimize_map_to_machine_filament = [&](const std::vector<int>& map_to_machine_filament, const std::vector<int>& l_nodes, const std::vector<int>& r_nodes, std::vector<int>& filament_map) {
            std::vector<int> ungrouped_filaments;
            std::vector<int> filaments_to_optimize;

            auto map_filament_to_machine_filament = [&](int filament_idx, int machine_filament_idx) {
                auto& machine_filament = machine_filament_list[machine_filament_idx];
                filament_map[used_filaments[filament_idx]] = machine_filament.extruder_id;  // set extruder id to filament map
                extruder_filament_count[machine_filament.extruder_id] += 1; // increase filament count in extruder
                };
            auto unmap_filament_to_machine_filament = [&](int filament_idx, int machine_filament_idx) {
                auto& machine_filament = machine_filament_list[machine_filament_idx];
                extruder_filament_count[machine_filament.extruder_id] -= 1; // increase filament count in extruder
                };

            for (size_t idx = 0; idx < map_to_machine_filament.size(); ++idx) {
                if (map_to_machine_filament[idx] == MaxFlowGraph::INVALID_ID) {
                    ungrouped_filaments.emplace_back(l_nodes[idx]);
                    continue;
                }
                int used_filament_idx = l_nodes[idx];
                int machine_filament_idx = r_nodes[map_to_machine_filament[idx]];
                auto& machine_filament = machine_filament_list[machine_filament_idx];
                if (machine_filament_set[machine_filament].size() > 1 && unprintable_limit_indices.count(used_filament_idx) == 0)
                    filaments_to_optimize.emplace_back(idx);

                map_filament_to_machine_filament(used_filament_idx, machine_filament_idx);
            }
            // try to optimize the result
            for (auto idx : filaments_to_optimize) {
                int filament_idx = l_nodes[idx];
                bool is_support_filament = used_filament_list[filament_idx].usage_type == FilamentUsageType::SupportOnly;
                int old_machine_filament_idx = r_nodes[map_to_machine_filament[idx]];
                auto& old_machine_filament = machine_filament_list[old_machine_filament_idx];

                unmap_filament_to_machine_filament(filament_idx, old_machine_filament_idx);

                auto optional_filaments = machine_filament_set[old_machine_filament];

                // Phase 1: collect all candidates and compute their preference scores
                std::vector<std::pair<int, int>> valid_candidates; // available machine-filament idx and its score
                for (auto machine_filament : optional_filaments) {
                    int new_extruder_id = machine_filament_list[machine_filament].extruder_id;

                    // preference score for this assignment
                    int preference_score = 0;
                    bool new_extruder_prefer_support = ctx.machine_info.prefer_non_model_filament[new_extruder_id];

                    // reward a support filament assigned to a support-preferring nozzle
                    if (is_support_filament && new_extruder_prefer_support) {
                        preference_score += SupportPreferScore;
                    }

                    valid_candidates.emplace_back(machine_filament, preference_score);
                }
                // Phase 2: determine the best preference score
                int best_preference_score = 0;
                for (const auto& candidate : valid_candidates) {
                    if (candidate.second >= best_preference_score) {
                        best_preference_score = candidate.second;
                    }
                }

                // Phase 3: among candidates with the best preference score, pick the most load-balanced one
                int best_candidate = -1;
                int best_gap = std::numeric_limits<int>::max();

                for (const auto& candidate : valid_candidates) {
                    // only consider candidates with the best preference score
                    int machine_filament = candidate.first;
                    int score = candidate.second;
                    if (score == best_preference_score) {
                        int new_extruder_id = machine_filament_list[machine_filament].extruder_id;
                        int new_gap = std::abs(extruder_filament_count[new_extruder_id] + 1 - extruder_filament_count[1 - new_extruder_id]);

                        // among equal-preference candidates, pick the one giving the most balanced load
                        if (new_gap < best_gap) {
                            best_gap = new_gap;
                            best_candidate = machine_filament;
                        }
                    }
                }
                // apply the best choice
                if (best_candidate != -1) {
                    map_filament_to_machine_filament(filament_idx, best_candidate);
                } else {
                    map_filament_to_machine_filament(filament_idx, old_machine_filament_idx);
                }
            }
            return ungrouped_filaments;
            };

        std::vector<int> group(ctx.group_info.total_filament_num, ctx.machine_info.master_extruder_id);
        std::vector<int> ungrouped_filaments;

        auto unlink_limits_full = build_unlink_limits(l_nodes, r_nodes, [&used_filament_list, &machine_filament_list, is_extruder_filament_compatible](int used_filament_idx, int machine_filament_idx) {
            return used_filament_list[used_filament_idx].type == machine_filament_list[machine_filament_idx].type &&
                used_filament_list[used_filament_idx].is_support == machine_filament_list[machine_filament_idx].is_support &&
                is_extruder_filament_compatible(used_filament_idx, machine_filament_list[machine_filament_idx].extruder_id);
            });

        {
            MatchModeGroupSolver s(color_dist_matrix, l_nodes, r_nodes, machine_filament_capacity, unlink_limits_full);
            ungrouped_filaments = optimize_map_to_machine_filament(s.solve(), l_nodes, r_nodes,group);
            if (ungrouped_filaments.empty())
                return group;
        }

        // additionally remove type limits
        {
            l_nodes = ungrouped_filaments;
            auto unlink_limits = build_unlink_limits(l_nodes, r_nodes, [&machine_filament_list, is_extruder_filament_compatible](int used_filament_idx, int machine_filament_idx) {
                return is_extruder_filament_compatible(used_filament_idx, machine_filament_list[machine_filament_idx].extruder_id);
                });

            MatchModeGroupSolver s(color_dist_matrix, l_nodes, r_nodes, machine_filament_capacity, unlink_limits);
            ungrouped_filaments = optimize_map_to_machine_filament(s.solve(), l_nodes, r_nodes, group);
            if (ungrouped_filaments.empty())
                return group;
        }

        // remove all limits
        {
            l_nodes = ungrouped_filaments;
            MatchModeGroupSolver s(color_dist_matrix, l_nodes, r_nodes, machine_filament_capacity, {});
            auto ret = optimize_map_to_machine_filament(s.solve(), l_nodes, r_nodes, group);
            for (size_t idx = 0; idx < ret.size(); ++idx) {
                if (ret[idx] == MaxFlowGraph::INVALID_ID)
                    assert(false);
                else
                    group[used_filaments[l_nodes[idx]]] = machine_filament_list[r_nodes[ret[idx]]].extruder_id;
            }
        }

        return group;
    }

    std::vector<int> FilamentGroup::calc_filament_group_for_flush(int* cost)
    {
        auto used_filaments = collect_sorted_used_filaments(ctx.model_info.layer_filaments);

        std::vector<int> ret = calc_min_flush_group(cost);
        std::vector<std::vector<int>> memoryed_maps = this->m_memoryed_groups;
        memoryed_maps.insert(memoryed_maps.begin(), ret);

        std::vector<FilamentGroupUtils::FilamentInfo> used_filament_info;
        for (auto f : used_filaments) {
            used_filament_info.emplace_back(ctx.model_info.filament_info[f]);
        }

        ret = select_best_group_for_ams(memoryed_maps, ctx.nozzle_info.nozzle_list, used_filaments, used_filament_info, ctx.machine_info.machine_filament_info, ctx.group_info.has_filament_switcher);
        return ret;
    }

    std::vector<int> FilamentGroup::calc_filament_group_for_tpu(int *cost) {

        auto used_filaments = collect_sorted_used_filaments(ctx.model_info.layer_filaments);
        std::vector<FilamentGroupUtils::FilamentInfo> used_filament_list;
        for (auto f : used_filaments)
            used_filament_list.emplace_back(ctx.model_info.filament_info[f]);

        std::vector<std::vector<float>> print_time_matrix(used_filaments.size(), std::vector<float>(ctx.nozzle_info.extruder_nozzle_list.size()));
        for (int i = 0; i < used_filaments.size(); ++i){
            for (int j = 0; j < ctx.nozzle_info.extruder_nozzle_list.size(); ++j){
                print_time_matrix[i][j] = ctx.speed_info.filament_print_time[used_filaments[i]][j];
                if (ctx.nozzle_info.nozzle_list[j].volume_type == nvtTPUHighFlow)   // when both TPU High Flow and other nozzle types exist, prefer assigning filament to the TPU High Flow nozzle
                    print_time_matrix[i][j] *= 0.9;
            }
        }

        std::vector<int> l_nodes(used_filaments.size());
        std::iota(l_nodes.begin(), l_nodes.end(), 0);
        std::vector<int> r_nodes(ctx.nozzle_info.extruder_nozzle_list.size());
        std::iota(r_nodes.begin(), r_nodes.end(), 0);
        std::vector<int> machine_filament_capacity({int(used_filaments.size()), int(used_filaments.size())});

        std::map<int, int> unprintable_limit_indices; // key stores filament idx in used_filament, value stores unprintable extruder
        extract_unprintable_limit_indices(ctx.model_info.unprintable_filaments, used_filaments, unprintable_limit_indices);
        unprintable_limit_indices = rebuild_unprintables(used_filaments, unprintable_limit_indices);

        std::unordered_map<int, std::vector<int>> unlink_limits(used_filaments.size());
        for (int i = 0; i < used_filaments.size(); i++) {
            auto iter = unprintable_limit_indices.find(i);
            if (iter == unprintable_limit_indices.end() || iter->second < 0 || iter->second >= 2) continue;
            unlink_limits[i].emplace_back(iter->second);
        }

        MatchModeGroupSolver s(print_time_matrix, l_nodes, r_nodes, machine_filament_capacity, unlink_limits);
        auto ret = s.solve();
        for (size_t idx = 0; idx < ret.size(); ++idx) {
            if (ret[idx] == MaxFlowGraph::INVALID_ID) {
                assert(false);
                ret[idx] = 1;
            }
        }
        std::vector<int> group(ctx.group_info.total_filament_num, ctx.machine_info.master_extruder_id);
        for (int i = 0; i < ret.size(); ++i) group[used_filaments[i]] = ret[i];
        return group;
    }

    // sorted used_filaments

    std::unordered_map<int, std::vector<int>> FilamentGroup::rebuild_nozzle_unprintables(const std::vector<unsigned int>& used_filaments, const std::unordered_map<int, std::vector<int>>& extruder_unprintables, const std::vector<int>& filament_volume_map)
    {
        std::unordered_map<int, std::vector<int>> nozzle_unprintables;

        for(size_t fidx = 0 ;fidx<used_filaments.size(); ++fidx){
            NozzleVolumeType expected_volume = NozzleVolumeType(filament_volume_map[used_filaments[fidx]]);
            std::vector<int> unexpected_extruders;
            if(extruder_unprintables.find(fidx) != extruder_unprintables.end()){
                unexpected_extruders = extruder_unprintables.at(fidx);
            }

            auto unprintable_volumes = ctx.model_info.unprintable_volumes[used_filaments[fidx]];

            std::vector<int> unprintable_nozzles;
            for(size_t nozzle_idx =0 ;nozzle_idx < ctx.nozzle_info.nozzle_list.size(); ++nozzle_idx){
                auto nozzle_info = ctx.nozzle_info.nozzle_list[nozzle_idx];

                if(std::find(unexpected_extruders.begin(), unexpected_extruders.end(), nozzle_info.extruder_id)!= unexpected_extruders.end() || (expected_volume!=nvtHybrid && expected_volume != nozzle_info.volume_type) ||
                  (unprintable_volumes.count(nozzle_info.volume_type) != 0))
                    unprintable_nozzles.push_back(nozzle_idx);
            }
            if(unprintable_nozzles.empty())
                continue;

            sort_remove_duplicates(unprintable_nozzles);
            nozzle_unprintables[fidx] = unprintable_nozzles;
        }

        return nozzle_unprintables;
    }


    std::vector<int> calc_filament_group_for_match_multi_nozzle(const FilamentGroupContext& ctx)
    {
        FilamentGroup fg1(ctx);
        auto filament_extruder_map = fg1.calc_filament_group_for_match();

        FilamentGroupContext new_ctx = ctx;
        auto used_filaments = collect_sorted_used_filaments(ctx.model_info.layer_filaments);
        for(size_t idx = 0; idx < used_filaments.size(); ++idx)
            new_ctx.model_info.unprintable_filaments[1 - filament_extruder_map[used_filaments[idx]]].insert(used_filaments[idx]);
        new_ctx.machine_info.max_group_size.assign(new_ctx.machine_info.max_group_size.size(), std::numeric_limits<int>::max());
        FilamentGroup fg(new_ctx);
        return fg.calc_filament_group_for_flush();
    }

    std::vector<FilamentPlanRes> plan_filament_nozzle_mapping_and_order(const FilamentGroupContext &ctx)
    {
        std::vector<FilamentPlanRes> res;

        // right nodes: nozzles
        std::vector<int> r_nodes(ctx.nozzle_info.nozzle_list.size(), -1);
        auto initial_nozzle = ctx.nozzle_info.nozzle_status;
        for (int r_id = 0; r_id < r_nodes.size(); r_id++) { if (initial_nozzle.count(r_id)) r_nodes[r_id] = initial_nozzle[r_id]; }

        std::vector<int> r_nodes_group(ctx.nozzle_info.nozzle_list.size(), -1);
        for (auto &nozzle_info : ctx.nozzle_info.nozzle_list) { r_nodes_group[nozzle_info.group_id] = nozzle_info.extruder_id; }

        int   layer_nums   = ctx.model_info.layer_filaments.size();
        auto &flush_matrix = ctx.model_info.flush_matrix;

        int  prev_layer_last_nozzle_id   = -1;
        bool used_prev_layer_last_nozzle = false;

        // per-layer filament->nozzle matching
        std::vector<int> layer_fil_nozzle_match(ctx.model_info.filament_info.size(), 0);
        for (int i = 0; i < layer_nums; i++) {
            // left nodes: filaments, deduplicated
            const auto      &layer_filaments = ctx.model_info.layer_filaments[i];
            std::vector<int> l_nodes(layer_filaments.begin(), layer_filaments.end());
            std::sort(l_nodes.begin(), l_nodes.end());
            l_nodes.erase(std::unique(l_nodes.begin(), l_nodes.end()), l_nodes.end());

            if (l_nodes.empty()) {
                res.emplace_back(FilamentPlanRes{{}, {}});
                continue;
            }

            // per-layer queue of filaments used within each nozzle
            std::vector<std::deque<int>> nozzle_fil_deq(r_nodes.size());

            // build a reverse map from nozzle-loaded filament to nozzle index to speed lookups from O(n) to O(1)
            std::unordered_map<int, size_t> filament_to_nozzle;
            filament_to_nozzle.reserve(r_nodes.size());
            for (size_t noz_id = 0; noz_id < r_nodes.size(); ++noz_id) {
                if (r_nodes[noz_id] >= 0) { filament_to_nozzle[r_nodes[noz_id]] = noz_id; }
            }

            const int epochs = std::ceil(double(l_nodes.size()) / r_nodes.size());
            for (int j = 0; j < epochs; j++) {
                // 1. filter out filaments already matching the state loaded in a nozzle
                std::vector<int>  remaining_l_nodes;
                std::vector<int>  remaining_r_nodes;
                std::vector<int>  remaining_r_nodes_to_origin;
                std::vector<bool> used_r_nodes(r_nodes.size(), false);

                remaining_l_nodes.reserve(l_nodes.size());
                remaining_r_nodes.reserve(r_nodes.size());
                remaining_r_nodes_to_origin.reserve(r_nodes.size());

                for (int f_id : l_nodes) {
                    auto it = filament_to_nozzle.find(f_id);
                    if (it != filament_to_nozzle.end()) {
                        size_t noz_id                = it->second;
                        layer_fil_nozzle_match[f_id] = noz_id;
                        nozzle_fil_deq[noz_id].push_back(f_id);
                        used_prev_layer_last_nozzle = (noz_id == prev_layer_last_nozzle_id);
                        used_r_nodes[noz_id]        = true;
                    } else {
                        remaining_l_nodes.emplace_back(f_id);
                    }
                }
                l_nodes = std::move(remaining_l_nodes);

                for (int r_id = 0; r_id < r_nodes.size(); r_id++) {
                    if (!used_r_nodes[r_id]) {
                        remaining_r_nodes.emplace_back(r_nodes[r_id]);
                        remaining_r_nodes_to_origin.emplace_back(r_id);
                    }
                }

                // 2. run min-cost flow on the remaining nodes
                GroupMinCostFlowSolver s(flush_matrix, l_nodes, remaining_r_nodes, r_nodes_group);
                auto                            match = s.solve();
                int                             write = 0;
                for (int l_id = 0; l_id < l_nodes.size(); l_id++) {
                    if (match[l_id] >= 0 && match[l_id] < remaining_r_nodes.size()) {
                        int noz_id                          = remaining_r_nodes_to_origin[match[l_id]];
                        int filament_id                     = l_nodes[l_id];
                        layer_fil_nozzle_match[filament_id] = noz_id;
                        r_nodes[noz_id]                     = filament_id;
                        nozzle_fil_deq[noz_id].push_back(filament_id);
                        // update the reverse map
                        filament_to_nozzle[filament_id] = noz_id;
                    } else {
                        l_nodes[write++] = l_nodes[l_id];
                    }
                }
                l_nodes.resize(write);
            }

            // order the filaments within the layer
            int start_extruder = 0;
            int start_nozzle   = 0;
            if (used_prev_layer_last_nozzle) {
                start_extruder = ctx.nozzle_info.nozzle_list[prev_layer_last_nozzle_id].extruder_id;
                start_nozzle   = prev_layer_last_nozzle_id;
            }

            std::deque<int> used_nozzle_deq;
            for (int m = 0; m < ctx.nozzle_info.extruder_nozzle_list.size(); m++) {
                int cur_extruder = (m + start_extruder) % ctx.nozzle_info.extruder_nozzle_list.size();
                for (auto noz_id : ctx.nozzle_info.extruder_nozzle_list.at(cur_extruder)) {
                    if (nozzle_fil_deq[noz_id].empty()) continue;
                    if (noz_id == start_nozzle)
                        used_nozzle_deq.push_front(noz_id);
                    else
                        used_nozzle_deq.push_back(noz_id);
                }
            }

            std::vector<int> layer_fil_order;
            for (auto noz_id : used_nozzle_deq) {
                auto deq = nozzle_fil_deq[noz_id];
                layer_fil_order.reserve(layer_fil_order.size() + deq.size());
                layer_fil_order.insert(layer_fil_order.end(), deq.begin(), deq.end());

                prev_layer_last_nozzle_id = noz_id;
            }

            FilamentPlanRes layer_pan{layer_fil_order, layer_fil_nozzle_match};
            res.emplace_back(layer_pan);
        }

        return res;
    }

    std::vector<int> calc_filament_group_for_manual_multi_nozzle(const std::vector<int>& filament_map_manual, const FilamentGroupContext& ctx)
    {
        FilamentGroupContext new_ctx = ctx;
        auto used_filaments = collect_sorted_used_filaments(ctx.model_info.layer_filaments);
        for(size_t idx = 0; idx < used_filaments.size(); ++idx)
            new_ctx.model_info.unprintable_filaments[1 - filament_map_manual[used_filaments[idx]]].insert(used_filaments[idx]);

        new_ctx.machine_info.max_group_size.assign(new_ctx.machine_info.max_group_size.size(), std::numeric_limits<int>::max());
        FilamentGroup fg(new_ctx);
        return fg.calc_filament_group_for_flush();
    }


}


