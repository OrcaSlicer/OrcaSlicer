#include "FilamentGroup.hpp"
#include "GCode/ToolOrderUtils.hpp"
#include "FlushVolPredictor.hpp"
#include <queue>
#include <random>
#include <cassert>
#include <sstream>

namespace Slic3r
{
    using namespace FilamentGroupUtils;
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

    static uint64_t fnv_hash_two_ints(const int a, const int b)
        {
        constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
        constexpr uint64_t FNV_PRIME        = 1099511628211ULL;
        constexpr uint64_t SALT_A           = 0xA5A5A5A5A5A5A5A5ULL;
        constexpr uint64_t SALT_B           = 0x5A5A5A5A5A5A5A5AULL;

        uint64_t h = FNV_OFFSET_BASIS;
        h ^= static_cast<uint64_t>(a) + SALT_A;
        h *= FNV_PRIME;
        h ^= static_cast<uint64_t>(b) + SALT_B;
        h *= FNV_PRIME;

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
        const double color_threshold)
    {
        using namespace FlushPredict;

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
                    // Bounds-guard the flush_matrix indexing. flush_matrix[k] is
                    // sized for the GLOBAL filament table at the time it was
                    // built, but used_filaments[] holds IDs from the model, which
                    // can exceed that size when the CLI bootstrap doesn't pre-size
                    // the per-nozzle matrix for the loaded filament count
                    // (observed: 2-filament CLI run on H2C dual-nozzle SIGSEGV).
                    // Treat out-of-range pairs as zero flush distance.
                    const auto& mk = flush_matrix[k];
                    const unsigned int fi = used_filaments[i];
                    const unsigned int fj = used_filaments[j];
                    float max_val = 0.f, min_val = 0.f;
                    if (fi < mk.size() && fj < mk.size() && fj < mk[fi].size() && fi < mk[fj].size()) {
                        max_val = std::max(mk[fi][fj], mk[fj][fi]);
                        min_val = std::min(mk[fi][fj], mk[fj][fi]);
                    }
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


    std::vector<int> KMediods2::cluster_small_data(const std::map<int, int>& unplaceable_limits, const std::vector<int>& group_size)
    {
        std::vector<int>labels(m_elem_count, -1);
        std::vector<int>new_group_size = group_size;

        for (auto& [elem, center] : unplaceable_limits) {
            if (labels[elem] == -1) {
                int gid = 1 - center;
                labels[elem] = gid;
                new_group_size[gid] -= 1;
            }
        }

        for (auto& label : labels) {
            if (label == -1) {
                int gid = -1;
                for (size_t idx = 0; idx < new_group_size.size(); ++idx) {
                    if (new_group_size[idx] > 0) {
                        gid = idx;
                        break;
                    }
                }
                if (gid != -1) {
                    label = gid;
                    new_group_size[gid] -= 1;
                }
                else {
                    label = m_default_group_id;
                }
            }
        }

        return labels;
    }

    std::vector<int> KMediods2::assign_cluster_label(const std::vector<int>& center, const std::map<int, int>& unplaceable_limtis, const std::vector<int>& group_size, const FGStrategy& strategy)
    {
        struct Comp {
            bool operator()(const std::pair<int, int>& a, const std::pair<int, int>& b) {
                return a.second > b.second;
            }
        };

        std::vector<std::set<int>>groups(2);
        std::vector<int>new_max_group_size = group_size;
        // store filament idx and distance gap between center 0 and center 1
        std::priority_queue<std::pair<int, int>, std::vector<std::pair<int, int>>, Comp>min_heap;

        for (int i = 0; i < m_elem_count; ++i) {
            if (auto it = unplaceable_limtis.find(i); it != unplaceable_limtis.end()) {
                int gid = it->second;
                assert(gid == 0 || gid == 1);
                groups[1 - gid].insert(i);   // insert to group
                new_max_group_size[1 - gid] = std::max(new_max_group_size[1 - gid] - 1, 0); // decrease group_size
                continue;
            }
            int distance_to_0 = m_evaluator->get_distance(i, center[0], 0);
            int distance_to_1 = m_evaluator->get_distance(i, center[1], 1);
            min_heap.push({ i,distance_to_0 - distance_to_1 });
        }

        bool have_enough_size = (min_heap.size() <= (new_max_group_size[0] + new_max_group_size[1]));

        if (have_enough_size || strategy == FGStrategy::BestFit) {
            while (!min_heap.empty()) {
                auto top = min_heap.top();
                min_heap.pop();
                if (groups[0].size() < new_max_group_size[0] && (top.second <= 0 || groups[1].size() >= new_max_group_size[1]))
                    groups[0].insert(top.first);
                else if (groups[1].size() < new_max_group_size[1] && (top.second > 0 || groups[0].size() >= new_max_group_size[0]))
                    groups[1].insert(top.first);
                else {
                    if (top.second <= 0)
                        groups[0].insert(top.first);
                    else
                        groups[1].insert(top.first);
                }
            }
        }
        else {
            while (!min_heap.empty()) {
                auto top = min_heap.top();
                min_heap.pop();
                if (top.second <= 0)
                    groups[0].insert(top.first);
                else
                    groups[1].insert(top.first);
            }
        }

        std::vector<int>labels(m_elem_count);
        for (auto& f : groups[0])
            labels[f] = 0;
        for (auto& f : groups[1])
            labels[f] = 1;

        return labels;
    }

    int KMediods2::calc_cost(const std::vector<int>& labels, const std::vector<int>& medoids)
    {
        int total_cost = 0;
        for (int i = 0; i < m_elem_count; ++i)
            total_cost += m_evaluator->get_distance(i, medoids[labels[i]], labels[i]);
        return total_cost;
    }

    void KMediods2::do_clustering(const FGStrategy& g_strategy, int timeout_ms)
    {
        FlushTimeMachine T;
        T.time_machine_start();

        if (m_elem_count < m_k) {
            m_cluster_labels = cluster_small_data(m_unplaceable_limits, m_max_cluster_size);
            {
                std::vector<int>cluster_center(m_k, -1);
                for (size_t idx = 0; idx < m_cluster_labels.size(); ++idx) {
                    if (cluster_center[m_cluster_labels[idx]] == -1)
                        cluster_center[m_cluster_labels[idx]] = idx;
                }
                MemoryedGroup g(m_cluster_labels, calc_cost(m_cluster_labels, cluster_center), 1);
                update_memoryed_groups(g, memory_threshold, memoryed_groups);
            }
            return;
        }

        std::vector<int>best_labels;
        int best_cost = std::numeric_limits<int>::max();

        for (int center_0 = 0; center_0 < m_elem_count; ++center_0) {
            if (auto iter = m_unplaceable_limits.find(center_0); iter != m_unplaceable_limits.end() && iter->second == 0)
                continue;
            for (int center_1 = 0; center_1 < m_elem_count; ++center_1) {
                if (center_0 == center_1)
                    continue;
                if (auto iter = m_unplaceable_limits.find(center_1); iter != m_unplaceable_limits.end() && iter->second == 1)
                    continue;

                std::vector<int>new_centers = { center_0,center_1 };
                std::vector<int>new_labels = assign_cluster_label(new_centers, m_unplaceable_limits, m_max_cluster_size, g_strategy);

                int new_cost = calc_cost(new_labels, new_centers);
                if (new_cost < best_cost) {
                    best_cost = new_cost;
                    best_labels = new_labels;
                }

                {
                    MemoryedGroup g(new_labels,new_cost,1);
                    update_memoryed_groups(g, memory_threshold, memoryed_groups);
                }

                if (T.time_machine_end() > timeout_ms)
                    break;
            }
            if (T.time_machine_end() > timeout_ms)
                break;
        }
        this->m_cluster_labels = best_labels;
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

    std::vector<int> KMediods::cluster_small_data(const FilamentGroupContext &context) {

        //1.Determine the groups each filament is allowed to be assigned to
        std::vector<std::vector<int>> candidates(m_elem_count);
        for (int i = 0; i < m_elem_count; i++) {
            std::unordered_set<int> group_set;
            if (auto it = m_placeable_limits.find(i); it != m_placeable_limits.end()) {
                group_set.insert(it->second.begin(), it->second.end());
            } else {
                for (int g = 0; g < m_k; g++) group_set.insert(g);
            }
            if (auto it = m_unplaceable_limits.find(i); it != m_unplaceable_limits.end()) {
                for (int g : it->second) group_set.erase(g);
            }
            candidates[i].assign(group_set.begin(), group_set.end());
        }

        // Store the hash value of each nozzle and its filaments in each group
        auto vector_equal = [](const std::vector<uint64_t> &a, const std::vector<uint64_t> &b) -> bool {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); i++) {
                if (a[i] != b[i]) return false;
            }
            return true;
        };
        auto vector_hash = [](const std::vector<uint64_t> &k) -> size_t {
            size_t h = 0;
            for (auto v : k) { h ^= v + GOLDEN_RATIO_32 + (h << 6) + (h >> 2); }
            return h;
        };
        std::unordered_set<std::vector<uint64_t>, decltype(vector_hash), decltype(vector_equal)> group_set(0, vector_hash, vector_equal);
        std::vector<uint64_t> group_hashs;

        //2.Compute hash value based on nozzle type  left/right extruder + high/standard flow
        std::vector<size_t> nozzles_hash(m_k);
        for (auto nozzle : context.nozzle_info.nozzle_list) {
            nozzles_hash[nozzle.group_id] = fnv_hash_two_ints(nozzle.volume_type, nozzle.group_id > 0);
        }

        //3.Enumerate group assignments
        const std::vector<unsigned int> used_filaments = collect_sorted_used_filaments(context.model_info.layer_filaments);
        std::vector<int> labels(context.group_info.total_filament_num, 0);
        std::vector<int> used_labels(used_filaments.size(), 0);
        if (!memoryed_groups.empty()) memoryed_groups = FilamentGroupUtils::MemoryedGroupHeap();

        const long long total = std::pow(m_k, m_elem_count);
        for (long long mask = 0; mask < total; mask++) {
            long long num = mask;
            std::unordered_map<int, std::vector<int>> nozzles_filaments;
            std::vector<int> groups(m_cluster_group_size.size());

            std::vector<std::unordered_map<NozzleVolumeType, std::set<int>>> used_nozzles(m_cluster_group_size.size());
            std::vector<int> prefer_vs(3, 0);

            //4.calculate group info
            for (int i = 0; i < m_elem_count; i++) {
                int n_id = num % m_k;
                num /= m_k;
                int ex_id = m_nozzle_to_extruder[n_id];

                labels[used_filaments[i]] = n_id;
                used_labels[i]            = n_id;
                nozzles_filaments[n_id].emplace_back(i);
                groups[ex_id]++;

                used_nozzles[ex_id][context.nozzle_info.nozzle_list[n_id].volume_type].insert(n_id);

                if (std::find(candidates[i].begin(), candidates[i].end(), n_id) != candidates[i].end()) prefer_vs[0] += 1;
            }
            for (int i = 0; i < groups.size(); i++) prefer_vs[1] += std::min(groups[i], int(m_cluster_group_size[i].first.size()));

            std::unordered_set<NozzleVolumeType> nozzles_type;
            for (int i = 0; i < used_nozzles.size(); i++) {
                const auto &nozzles = used_nozzles[i];
                if (nozzles.size() == 1 && nozzles.begin()->second.size() == groups[i]) {
                    prefer_vs[2] += 1;
                    nozzles_type.insert(nozzles.begin()->first);
                }
            }
            if (nozzles_type.size() == 1) prefer_vs[2] += 1;

            int weight_for_placeable = 10000;
            int weight_for_extruder_filaments = 100;
            int weight_for_nozzle_type = 1;
            int prefer_level = prefer_vs[0] * weight_for_placeable + prefer_vs[1] * weight_for_extruder_filaments + prefer_vs[2] * weight_for_nozzle_type;

            //5.compute group hash
            group_hashs.clear();
            for (auto &nozzle_filaments : nozzles_filaments) {
                uint64_t filament_mask = 0;
                for (int filament : nozzle_filaments.second) filament_mask |= (1ULL << filament);
                size_t group_hash = nozzles_hash[nozzle_filaments.first];
                group_hash ^= (std::hash<uint64_t>{}(filament_mask) + GOLDEN_RATIO_32 + (group_hash << 6) + (group_hash >> 2));
                group_hashs.emplace_back(group_hash);
            }
            std::sort(group_hashs.begin(), group_hashs.end());
            if (group_set.find(group_hashs) != group_set.end()) continue;
            group_set.insert(group_hashs);

            //6.Evaluate group scores
            MultiNozzleUtils::MultiNozzleGroupResult group_res(labels, context.nozzle_info.nozzle_list, used_filaments);
            auto change_count = get_estimate_extruder_filament_change_count(context.model_info.layer_filaments, group_res);
            auto flush_volume = calc_cost(used_labels,std::vector<int>(m_k,0),-1);
            double time = change_count.first *context.speed_info.extruder_change_time + change_count.second *context.speed_info.filament_change_time;
            double score = evaluate_score(flush_volume, time, true);

            int master_ex_id = context.machine_info.master_extruder_id;
            if (master_ex_id < groups.size() && groups[master_ex_id] < (used_filaments.size() + 1) / 2)
                score += ABSOLUTE_FLUSH_GAP_TOLERANCE; //slightly prefer master extruders with more flush
            MemoryedGroup group(used_labels, score, prefer_level);
            update_memoryed_groups(group, memory_threshold, memoryed_groups);
        }
        return memoryed_groups.top().group;
    }

    // make sure each cluster at least have one elements
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

        // if still has -1，it means there has some filaments that cannot be placed under the limit. We neglect the -1 here since we
        // are deciding the cluster center, -1 can be handled in later steps
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
                    distance_matrix[i][j] = 0;
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
            // TODO: xcr：throw exception here?
            // adjust group size to elem count if the group cannot contain all of the filamnts
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

        // If there are materials that have not been grouped in the last step, assign them to the default group.
        for (size_t idx = 0; idx < labels.size(); ++idx) {
            if (labels[idx] == -1) {
                labels[idx] = m_default_group_id;
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
    void KMediods::do_clustering(const FilamentGroupContext &context, int timeout_ms, int retry)
    {
        FlushTimeMachine T;
        T.time_machine_start();

        if (m_elem_count <= m_k) {
            m_cluster_labels = cluster_small_data(context);
            return;
        }

        std::vector<int> best_cluster_centers = std::vector<int>(m_k, 0);
        std::vector<int> best_cluster_labels  = std::vector<int>(m_elem_count, m_default_group_id);
        int              best_cluster_cost    = std::numeric_limits<int>::max();
        int              retry_count          = 0;

        while (retry_count < retry && T.time_machine_end() < timeout_ms) {
            std::vector<int> curr_cluster_centers = init_cluster_center(m_placeable_limits, m_unplaceable_limits, m_max_cluster_size, m_cluster_group_size, retry_count);
            std::vector<int> curr_cluster_labels = assign_cluster_label(curr_cluster_centers, m_placeable_limits, m_unplaceable_limits, m_max_cluster_size, m_cluster_group_size);
            int              curr_cluster_cost   = calc_cost(curr_cluster_labels, curr_cluster_centers);

            MemoryedGroup g(curr_cluster_labels, curr_cluster_cost, 1);
            update_memoryed_groups(g, memory_threshold, memoryed_groups);

            bool mediods_changed = true;
            while (mediods_changed && T.time_machine_end() < timeout_ms) {
                mediods_changed       = false;
                int best_swap_cost    = curr_cluster_cost;
                int best_swap_cluster = -1;
                int best_swap_elem    = -1;

                for (size_t cluster_id = 0; cluster_id < m_k; ++cluster_id) {
                    if (curr_cluster_centers[cluster_id] == -1) continue; // skip the empty cluster
                    for (int elem = 0; elem < m_elem_count; ++elem) {
                        if (std::find(curr_cluster_centers.begin(), curr_cluster_centers.end(), elem) != curr_cluster_centers.end() ||
                            std::find(m_unplaceable_limits[cluster_id].begin(), m_unplaceable_limits[cluster_id].end(), elem) != m_unplaceable_limits[cluster_id].end())
                            continue;
                        std::vector<int> tmp_centers = curr_cluster_centers;
                        tmp_centers[cluster_id]      = elem; // swap the mediod
                        std::vector<int> tmp_labels  = assign_cluster_label(tmp_centers, m_placeable_limits, m_unplaceable_limits, m_max_cluster_size, m_cluster_group_size);
                        int              tmp_cost    = calc_cost(tmp_labels, tmp_centers);

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
                    curr_cluster_cost   = calc_cost(curr_cluster_labels, curr_cluster_centers);

                    MemoryedGroup g(curr_cluster_labels, curr_cluster_cost, 1); // in non enum mode, we use the same prefer level
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
        int used_filament_num = used_filaments.size();

        if (used_filament_num < 10)
            return calc_min_flush_group_by_enum(used_filaments, cost);
        else
            return calc_min_flush_group_by_pam2(used_filaments, cost, 500);
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
        try {
            if (FGMode::MatchMode == ctx.group_info.mode)
                return calc_filament_group_for_match(cost);
        }
        catch (const FilamentGroupException& e) {
        }

        auto merged_map = try_merge_filaments();
        rebuild_context(merged_map);
        auto filamnet_map = calc_filament_group_for_flush(cost);
        return seperate_merged_filaments(filamnet_map, merged_map);
    }

    std::vector<int> FilamentGroup::calc_filament_group_for_match(int* cost)
    {
        using namespace FlushPredict;

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

        auto optimize_map_to_machine_filament = [&](const std::vector<int>& map_to_machine_filament, const std::vector<int>& l_nodes, const std::vector<int>& r_nodes, std::vector<int>& filament_map, bool consider_capacity) {
            std::vector<int> ungrouped_filaments;
            std::vector<int> filaments_to_optimize;

            auto map_filament_to_machine_filament = [&](int filament_idx, int machine_filament_idx) {
                auto& machine_filament = machine_filament_list[machine_filament_idx];
                machine_filament_capacity[machine_filament_idx] = std::max(0, machine_filament_capacity[machine_filament_idx] - 1);  // decrease machine filament capacity
                filament_map[used_filaments[filament_idx]] = machine_filament.extruder_id;  // set extruder id to filament map
                extruder_filament_count[machine_filament.extruder_id] += 1; // increase filament count in extruder
                };
            auto unmap_filament_to_machine_filament = [&](int filament_idx, int machine_filament_idx) {
                auto& machine_filament = machine_filament_list[machine_filament_idx];
                machine_filament_capacity[machine_filament_idx] += 1;  // increase machine filament capacity
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
                int old_machine_filament_idx = r_nodes[map_to_machine_filament[idx]];
                auto& old_machine_filament = machine_filament_list[old_machine_filament_idx];

                int curr_gap = std::abs(extruder_filament_count[0] - extruder_filament_count[1]);
                unmap_filament_to_machine_filament(filament_idx, old_machine_filament_idx);

                auto optional_filaments = machine_filament_set[old_machine_filament];
                auto iter = optional_filaments.begin();
                for (; iter != optional_filaments.end(); ++iter) {
                    int new_extruder_id = machine_filament_list[*iter].extruder_id;
                    int new_gap = std::abs(extruder_filament_count[new_extruder_id] + 1 - extruder_filament_count[1 - new_extruder_id]);
                    if (new_gap < curr_gap && (!consider_capacity || machine_filament_capacity[*iter] > 0)) {
                        map_filament_to_machine_filament(filament_idx, *iter);
                        break;
                    }
                }

                if (iter == optional_filaments.end())
                    map_filament_to_machine_filament(filament_idx, old_machine_filament_idx);
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
            ungrouped_filaments = optimize_map_to_machine_filament(s.solve(), l_nodes, r_nodes,group,false);
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
            ungrouped_filaments = optimize_map_to_machine_filament(s.solve(), l_nodes, r_nodes, group,false);
            if (ungrouped_filaments.empty())
                return group;
        }

        // remove all limits
        {
            l_nodes = ungrouped_filaments;
            MatchModeGroupSolver s(color_dist_matrix, l_nodes, r_nodes, machine_filament_capacity, {});
            auto ret = optimize_map_to_machine_filament(s.solve(), l_nodes, r_nodes, group,false);
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

        ret = select_best_group_for_ams(memoryed_maps, ctx.nozzle_info.nozzle_list, used_filaments, used_filament_info, ctx.machine_info.machine_filament_info);
        return ret;
    }


    // sorted used_filaments
    std::vector<int> FilamentGroup::calc_min_flush_group_by_enum(const std::vector<unsigned int>& used_filaments, int* cost)
    {
        static constexpr int UNPLACEABLE_LIMIT_REWARD = 10000;  // reward value if the group result follows the unprintable limit
        static constexpr int MAX_SIZE_LIMIT_REWARD = 5000;    // reward value if the group result follows the max size per extruder
        static constexpr int SUPPORT_PREFER_REWARD = 100;   // reward value if the group result follow the supoport limit
        static constexpr int BEST_FIT_LIMIT_REWARD = 10;     // reward value if the group result try to fill the max size per extruder

        MemoryedGroupHeap memoryed_groups;

        auto bit_count_one = [](uint64_t n)
            {
                int count = 0;
                while (n != 0)
                {
                    n &= n - 1;
                    count++;
                }
                return count;
            };

        std::map<int, int>unplaceable_limit_indices;
        extract_unprintable_limit_indices(ctx.model_info.unprintable_filaments, used_filaments, unplaceable_limit_indices);

        int used_filament_num = used_filaments.size();
        uint64_t max_group_num = (static_cast<uint64_t>(1) << used_filament_num);

        struct CachedGroup{
            std::vector<int> filament_map;
            double flush{0};
            double time{0};
            int prefer_level {0};
            double score{1};
        };

        CachedGroup best_group;
        std::vector<CachedGroup> cached_groups;

        for (uint64_t i = 0; i < max_group_num; ++i) {
            std::vector<std::set<int>>groups(2);
            for (int j = 0; j < used_filament_num; ++j) {
                if (i & (static_cast<uint64_t>(1) << j))
                    groups[1].insert(j);
                else
                    groups[0].insert(j);
            }

            int prefer_level = 0;

            if (check_printable(groups, unplaceable_limit_indices))
                prefer_level += UNPLACEABLE_LIMIT_REWARD;
            if (groups[0].size() <= ctx.machine_info.max_group_size[0] && groups[1].size() <= ctx.machine_info.max_group_size[1])
                prefer_level += MAX_SIZE_LIMIT_REWARD;
            if (FGStrategy::BestFit == ctx.group_info.strategy && groups[0].size() >= ctx.machine_info.max_group_size[0] && groups[1].size() >= ctx.machine_info.max_group_size[1])
                prefer_level += BEST_FIT_LIMIT_REWARD;

            int prefer_filament_count = 0;
            for (int gidx = 0; gidx < 2; ++gidx) {
                if (ctx.machine_info.prefer_non_model_filament[gidx]) {
                    for (int fidx : groups[gidx]) {
                        if (ctx.model_info.filament_info[fidx].usage_type == FilamentGroupUtils::SupportOnly)
                            prefer_filament_count += 1;
                    }
                }
            }

            prefer_level += prefer_filament_count * SUPPORT_PREFER_REWARD;

            std::vector<int>filament_maps(used_filament_num);
            for (int i = 0; i < used_filament_num; ++i) {
                if (groups[0].find(i) != groups[0].end())
                    filament_maps[i] = 0;
                if (groups[1].find(i) != groups[1].end())
                    filament_maps[i] = 1;
            }

            int total_cost = reorder_filaments_for_minimum_flush_volume(
                used_filaments,
                filament_maps,
                ctx.model_info.layer_filaments,
                ctx.model_info.flush_matrix,
                get_custom_seq,
                nullptr
            );

            if (groups[ctx.machine_info.master_extruder_id].size() < (used_filaments.size() + 1) / 2)
                total_cost += ABSOLUTE_FLUSH_GAP_TOLERANCE;  // slightly prefer master extruders with more flush

            CachedGroup curr_group;
            curr_group.flush = total_cost;
            curr_group.prefer_level = prefer_level;
            curr_group.filament_map.resize(ctx.group_info.total_filament_num, 0);
            for (size_t i = 0; i < filament_maps.size(); ++i)
                curr_group.filament_map[used_filaments[i]] = filament_maps[i];

            TimeEvaluator time_evaluator(ctx.speed_info);
            curr_group.time = time_evaluator.get_estimated_time(curr_group.filament_map);
            cached_groups.emplace_back(std::move(curr_group));
        }

        // 如果归一化，没法处理边界情况
        {
            // double min_flush = std::min_element(cached_groups.begin(),cached_groups.end(),[](const CachedGroup& a, const CachedGroup& b) {return a.flush < b.flush;})->flush;
            // double max_flush = std::max_element(cached_groups.begin(),cached_groups.end(),[](const CachedGroup& a, const CachedGroup& b) {return a.flush < b.flush;})->flush;
            // double min_time = std::min_element(cached_groups.begin(),cached_groups.end(),[](const CachedGroup& a, const CachedGroup& b) {return a.time < b.time;})->time;
            // double max_time = std::max_element(cached_groups.begin(),cached_groups.end(),[](const CachedGroup& a, const CachedGroup& b) {return a.time < b.time;})->time;

            int count = 0;
            for (CachedGroup& cached_group : cached_groups) {
                //double norm_flush = (max_flush - min_flush == 0) ? 0 : (cached_group.flush - min_flush) / (max_flush - min_flush);
                //double norm_time = (max_time - min_time == 0) ? 0 : (cached_group.time - min_time) / (max_time - min_time);
                cached_group.score = evaluate_score(cached_group.flush, cached_group.time, ctx.speed_info.group_with_time);

                if(cached_group.prefer_level > best_group.prefer_level || (cached_group.prefer_level == best_group.prefer_level && cached_group.score < best_group.score)){
                    best_group = cached_group;
            }

            {
                    MemoryedGroup mg(cached_group.filament_map, cached_group.score, cached_group.prefer_level);
                update_memoryed_groups(mg, ctx.group_info.max_gap_threshold, memoryed_groups);
            }
                BOOST_LOG_TRIVIAL(info) << "Filament group" << count++ << ", score : " << cached_group.score << " , flush : " << cached_group.flush << " , time : " << cached_group.time << " , prefer : " << cached_group.prefer_level;
        }
        }


        if (cost)
            *cost =best_group.flush;

        m_memoryed_groups.clear();
        while(!memoryed_groups.empty()){
            auto top = memoryed_groups.top();
            memoryed_groups.pop();
            m_memoryed_groups.push_back(top.group);
        }

        return best_group.filament_map;
    }

    // sorted used_filaments
    std::vector<int> FilamentGroup::calc_min_flush_group_by_pam2(const std::vector<unsigned int>& used_filaments, int* cost, int timeout_ms)
    {
        std::vector<int>filament_labels_ret(ctx.group_info.total_filament_num, ctx.machine_info.master_extruder_id);

        std::map<int, int>unplaceable_limits;
        extract_unprintable_limit_indices(ctx.model_info.unprintable_filaments, used_filaments, unplaceable_limits);

        auto distance_evaluator = std::make_shared<FlushDistanceEvaluator>(ctx.model_info.flush_matrix, used_filaments, ctx.model_info.layer_filaments);
        KMediods2 PAM((int)used_filaments.size(), distance_evaluator, ctx.machine_info.master_extruder_id);
        PAM.set_max_cluster_size(ctx.machine_info.max_group_size);
        PAM.set_unplaceable_limits(unplaceable_limits);
        PAM.set_memory_threshold(ctx.group_info.max_gap_threshold);
        PAM.do_clustering(ctx.group_info.strategy, timeout_ms);

        std::vector<int>filament_labels = PAM.get_cluster_labels();

        {
            auto memoryed_groups = PAM.get_memoryed_groups();
            change_memoryed_heaps_to_arrays(memoryed_groups, ctx.group_info.total_filament_num, used_filaments, m_memoryed_groups);
        }

        if (cost)
            *cost = reorder_filaments_for_minimum_flush_volume(used_filaments, filament_labels, ctx.model_info.layer_filaments, ctx.model_info.flush_matrix, std::nullopt, nullptr);

        for (int i = 0; i < filament_labels.size(); ++i)
            filament_labels_ret[used_filaments[i]] = filament_labels[i];
        return filament_labels_ret;
    }

    std::unordered_map<int, std::vector<int>> FilamentGroupMultiNozzle::rebuild_nozzle_unprintables(const std::vector<unsigned int>& used_filaments, const std::unordered_map<int, std::vector<int>>& extruder_unprintables, const std::vector<int>& filament_volume_map)
    {
        std::unordered_map<int, std::vector<int>> nozzle_unprintables;

        for(size_t fidx = 0 ;fidx<used_filaments.size(); ++fidx){
            NozzleVolumeType expected_volume = NozzleVolumeType(filament_volume_map[used_filaments[fidx]]);
            std::vector<int> unexpected_extruders;
            if(extruder_unprintables.find(fidx) != extruder_unprintables.end()){
                unexpected_extruders = extruder_unprintables.at(fidx);
            }

            std::vector<int> unprintable_nozzles;
            for(size_t nozzle_idx =0 ;nozzle_idx < m_context.nozzle_info.nozzle_list.size(); ++nozzle_idx){
                auto nozzle_info = m_context.nozzle_info.nozzle_list[nozzle_idx];

                if(std::find(unexpected_extruders.begin(), unexpected_extruders.end(), nozzle_info.extruder_id)!= unexpected_extruders.end() || (expected_volume!=nvtHybrid && expected_volume != nozzle_info.volume_type))
                    unprintable_nozzles.push_back(nozzle_idx);
            }
            if(unprintable_nozzles.empty())
                continue;

            sort_remove_duplicates(unprintable_nozzles);
            nozzle_unprintables[fidx] = unprintable_nozzles;
        }

        return nozzle_unprintables;
    }

    std::vector<int> FilamentGroupMultiNozzle::calc_filament_group_by_mcmf()
    {
        std::vector<unsigned int> used_filaments = collect_sorted_used_filaments(m_context.model_info.layer_filaments);

        std::map<int, int>unplaceable_limits;
        extract_unprintable_limit_indices(m_context.model_info.unprintable_filaments, used_filaments, unplaceable_limits);

        auto distance_evaluator = std::make_shared<FlushDistanceEvaluator>(m_context.model_info.flush_matrix, used_filaments, m_context.model_info.layer_filaments);
        std::vector<std::set<int>>groups(2);

        // first cluster
        {
            KMediods2 PAM((int)used_filaments.size(), distance_evaluator);
            PAM.set_max_cluster_size({(int)m_context.nozzle_info.extruder_nozzle_list[0].size(),(int)m_context.nozzle_info.extruder_nozzle_list[1].size()});
            PAM.set_unplaceable_limits(unplaceable_limits);
            PAM.do_clustering(FGStrategy::BestFit);
            auto first_clustered_labels = PAM.get_cluster_labels();
            int total_nozzle_num = m_context.nozzle_info.nozzle_list.size();

            if (total_nozzle_num > used_filaments.size()) {
                std::vector<int>ret(m_context.group_info.total_filament_num);
                for (size_t idx = 0; idx < first_clustered_labels.size(); ++idx) {
                    ret[used_filaments[idx]] = first_clustered_labels[idx];
                }
                return ret;
            }

            // first place the elem if it follows the limit
            for (size_t idx = 0; idx < first_clustered_labels.size(); ++idx) {
                if (unplaceable_limits.count(idx) > 0)
                    groups[first_clustered_labels[idx]].insert(idx);
            }
            // then fullfill the nozzle with other filaments
            for (size_t idx = 0; idx < first_clustered_labels.size(); ++idx) {
                // place the elem in first cluster if the elem follow the limit
                int gidx = first_clustered_labels[idx];
                if (groups[gidx].size() < m_context.nozzle_info.extruder_nozzle_list[gidx].size())
                    groups[gidx].insert(idx);
            }
        }

        std::vector<int>ret_map(m_context.group_info.total_filament_num);
        // second cluster
        {
            std::map<int, int>unplaceable_limits;
            for (size_t idx = 0; idx < groups.size(); ++idx) {
                for (auto& f : groups[idx])
                    unplaceable_limits.emplace(f, (int)(1 - idx));
            }
            KMediods2 PAM((int)used_filaments.size(), distance_evaluator);
            PAM.set_max_cluster_size(m_context.machine_info.max_group_size);
            PAM.set_unplaceable_limits(unplaceable_limits);
            PAM.do_clustering(FGStrategy::BestFit);
            auto labels = PAM.get_cluster_labels();

            for (size_t idx = 0; idx < labels.size(); ++idx)
                ret_map[used_filaments[idx]] = labels[idx];
        }
        return ret_map;
    }

    std::vector<int> FilamentGroupMultiNozzle::calc_filament_group_by_pam()
    {
        std::vector<unsigned int> used_filaments = collect_sorted_used_filaments(m_context.model_info.layer_filaments);
        std::unordered_map<int, std::vector<int>>unplaceable_limits;
        extract_unprintable_limit_indices(m_context.model_info.unprintable_filaments, used_filaments, unplaceable_limits); // turn filament idx to idx in used filaments
        unplaceable_limits = rebuild_nozzle_unprintables(used_filaments,unplaceable_limits,m_context.group_info.filament_volume_map);

        int k = m_context.nozzle_info.nozzle_list.size();
        auto distance_evaluator = std::make_shared<FlushDistanceEvaluator>(m_context.model_info.flush_matrix, used_filaments, m_context.model_info.layer_filaments);

         KMediods PAM(k, (int)used_filaments.size(), distance_evaluator);
        PAM.set_unplacable_limits(unplaceable_limits);

        std::vector<std::pair<std::set<int>, int>> cluster_size_limit;

        //TODO: 全改成map
        for (auto& extruder_nozzles : m_context.nozzle_info.extruder_nozzle_list) {
            auto& extruder_id = extruder_nozzles.first;
            auto& nozzles = extruder_nozzles.second;
            std::pair<std::set<int>, int> clusters;
            clusters.first = std::set<int>(nozzles.begin(), nozzles.end());
            if (extruder_id < 0 || (size_t)extruder_id >= m_context.machine_info.max_group_size.size()) {
                BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": extruder_id " << extruder_id
                    << " out of range for max_group_size (size " << m_context.machine_info.max_group_size.size() << "); using INT_MAX";
                clusters.second = std::numeric_limits<int>::max();
            } else {
                clusters.second = m_context.machine_info.max_group_size[extruder_id];
            }
            cluster_size_limit.emplace_back(clusters);
        }

        PAM.set_cluster_group_size(cluster_size_limit);
        PAM.set_memory_threshold(m_context.group_info.max_gap_threshold);
        PAM.do_clustering(m_context, 1500);

        auto memoryed_groups = PAM.get_memoryed_groups();
        std::vector<std::vector<int>> filament_to_nozzles;
        change_memoryed_heaps_to_arrays(memoryed_groups, m_context.group_info.total_filament_num, used_filaments, filament_to_nozzles);

        std::vector<FilamentGroupUtils::FilamentInfo> used_filament_info;
        for (auto f : used_filaments) { used_filament_info.emplace_back(m_context.model_info.filament_info[f]); }

        auto ret = select_best_group_for_ams(filament_to_nozzles, m_context.nozzle_info.nozzle_list, used_filaments, used_filament_info, m_context.machine_info.machine_filament_info);

        return ret;
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
        FilamentGroupMultiNozzle fg(new_ctx);
        return fg.calc_filament_group_by_pam();
    }

    std::vector<int> calc_filament_group_for_manual_multi_nozzle(const std::vector<int>& filament_map_manual, const FilamentGroupContext& ctx)
    {
        FilamentGroupContext new_ctx = ctx;
        auto used_filaments = collect_sorted_used_filaments(ctx.model_info.layer_filaments);
        for(size_t idx = 0; idx < used_filaments.size(); ++idx)
            new_ctx.model_info.unprintable_filaments[1 - filament_map_manual[used_filaments[idx]]].insert(used_filaments[idx]);

        new_ctx.machine_info.max_group_size.assign(new_ctx.machine_info.max_group_size.size(), std::numeric_limits<int>::max());
        FilamentGroupMultiNozzle fg(new_ctx);
        return fg.calc_filament_group_by_pam();
    }


}


