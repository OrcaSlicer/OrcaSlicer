#ifndef FILAMENT_GROUP_HPP
#define FILAMENT_GROUP_HPP

#include <chrono>
#include <memory>
#include <numeric>
#include <set>
#include <map>
#include <vector>
#include <queue>
#include "GCode/ToolOrderUtils.hpp"
#include "FilamentGroupUtils.hpp"

const static int DEFAULT_CLUSTER_SIZE = 16;

const static int ABSOLUTE_FLUSH_GAP_TOLERANCE = 10;


namespace Slic3r
{
    std::vector<unsigned int>collect_sorted_used_filaments(const std::vector<std::vector<unsigned int>>& layer_filaments);

    enum FGStrategy {
        BestCost,
        BestFit
    };

    enum FGMode {
        FlushMode,
        MatchMode
    };

    namespace FilamentGroupUtils
    {
        struct FlushTimeMachine
        {
        private:
            std::chrono::high_resolution_clock::time_point start;

        public:
            void time_machine_start()
            {
                start = std::chrono::high_resolution_clock::now();
            }

            int time_machine_end()
            {
                auto end = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                return duration.count();
            }
        };

        struct MemoryedGroup {
            MemoryedGroup() = default;
            MemoryedGroup(const std::vector<int>& group_, const double cost_, const int prefer_level_) :group(group_), cost(cost_), prefer_level(prefer_level_) {}
            bool operator>(const MemoryedGroup& other) const {
                return prefer_level < other.prefer_level || (prefer_level == other.prefer_level && cost > other.cost);
            }

            double cost{ 0 };
            int prefer_level{ 0 };
            std::vector<int>group;
        };

        using MemoryedGroupHeap = std::priority_queue<MemoryedGroup, std::vector<MemoryedGroup>, std::greater<MemoryedGroup>>;

        void update_memoryed_groups(const MemoryedGroup& item,const double gap_threshold, MemoryedGroupHeap& groups);
    }

    struct FilamentGroupContext
    {
        struct ModelInfo {
            std::vector<FlushMatrix> flush_matrix;
            std::vector<std::vector<unsigned int>> layer_filaments;
            std::vector<FilamentGroupUtils::FilamentInfo> filament_info;
            std::vector<std::string> filament_ids;
            std::vector<std::set<int>> unprintable_filaments;
            std::map<int, std::set<NozzleVolumeType>> unprintable_volumes;
        } model_info;

        struct GroupInfo {
            int total_filament_num;
            double max_gap_threshold;
            FGMode mode;
            FGStrategy strategy;
            bool ignore_ext_filament;
            bool has_filament_switcher = false;
            std::vector<int> filament_volume_map;
        } group_info;

        struct MachineInfo {
            std::vector<int> max_group_size;
            std::vector<std::vector<FilamentGroupUtils::MachineFilamentInfo>> machine_filament_info;
            std::vector<bool> prefer_non_model_filament;
            int master_extruder_id;
        } machine_info;

        struct SpeedInfo{
            std::unordered_map<int,std::unordered_map<int,double>> filament_print_time;
            double extruder_change_time;
            double filament_change_time;
            bool group_with_time;
            MultiNozzleUtils::FilamentChangeTimeParams change_time_params;
            std::vector<bool> ams_preload_enabled;
        } speed_info;

        struct NozzleInfo {
            std::map<int, std::vector<int>> extruder_nozzle_list;
            std::vector<MultiNozzleUtils::NozzleInfo> nozzle_list;
            std::unordered_map<int, int> nozzle_status;
        } nozzle_info;
    };

    std::vector<int> select_best_group_for_ams(const std::vector<std::vector<int>> &filament_to_nozzles,
        const std::vector<MultiNozzleUtils::NozzleInfo>& nozzle_list,
        const std::vector<unsigned int>& used_filaments,
        const std::vector<FilamentGroupUtils::FilamentInfo>& used_filament_info,
        const std::vector<std::vector<FilamentGroupUtils::MachineFilamentInfo>>& machine_filament_info,
        const bool has_filament_switcher = false,
        const double color_delta_threshold = 20);


    class FlushDistanceEvaluator
    {
    public:
        FlushDistanceEvaluator(const std::vector<FlushMatrix>& flush_matrix,const std::vector<unsigned int>&used_filaments,const std::vector<std::vector<unsigned int>>& layer_filaments, double p = 0.65);
        ~FlushDistanceEvaluator() = default;
        double get_distance(int idx_a, int idx_b, int extruder_id) const;
    private:
        std::vector<std::vector<std::vector<float>>>m_distance_matrix;

    };


    class TimeEvaluator
    {
    public:
        TimeEvaluator(const FilamentGroupContext::SpeedInfo& speed_info) : m_speed_info(speed_info) {}
        double get_estimated_time(const std::vector<int>& filament_map) const;
    private:
        FilamentGroupContext::SpeedInfo m_speed_info;
    };

    // Search budget for the k-medoids clustering, an anytime search. Each restart is seeded from its
    // own index, so what it returns depends on how many restarts complete before the clock expires,
    // and therefore on the speed of the machine. A timeout_ms <= 0 removes the clock and bounds the
    // search by max_restarts alone.
    struct ClusteringBudget
    {
        int timeout_ms = 3000;
        int max_restarts = 30;
    };

    class FilamentGroup
    {
        using MemoryedGroup = FilamentGroupUtils::MemoryedGroup;
        using MemoryedGroupHeap = FilamentGroupUtils::MemoryedGroupHeap;
    public:
        explicit FilamentGroup(const FilamentGroupContext& ctx_) :ctx(ctx_) {}
    public:
        void set_clustering_budget(const ClusteringBudget& budget) { m_clustering_budget = budget; }

        std::vector<int> calc_filament_group(int * cost = nullptr);
        std::vector<std::vector<int>> get_memoryed_groups()const { return m_memoryed_groups; }

    public:
        std::vector<int> calc_filament_group_for_match(int* cost = nullptr);
        std::vector<int> calc_filament_group_for_flush(int* cost = nullptr);
        std::vector<int> calc_filament_group_for_tpu(int* cost = nullptr);
    private:
        std::vector<int> calc_min_flush_group(int* cost = nullptr);

        std::vector<int> calc_group_by_enum(int k, const std::vector<unsigned int>& used_filaments,
            const std::unordered_map<int, std::vector<int>>& unplaceable_limits, int* cost = nullptr);
        std::vector<int> calc_group_by_kmedoids(int k, const std::vector<unsigned int>& used_filaments,
            const std::unordered_map<int, std::vector<int>>& unplaceable_limits, int* cost = nullptr);

        std::map<int, int> rebuild_unprintables(const std::vector<unsigned int>& used_filaments, const std::map<int,int>& extruder_unprintables);
        std::unordered_map<int, std::vector<int>> rebuild_nozzle_unprintables(const std::vector<unsigned int>& used_filaments, const std::unordered_map<int, std::vector<int>>& extruder_unprintables, const std::vector<int>& filament_volume_map);

        std::unordered_map<int, std::vector<int>> try_merge_filaments();
        void rebuild_context(const std::unordered_map<int, std::vector<int>>& merged_filaments);
        std::vector<int> seperate_merged_filaments(const std::vector<int>& filament_map, const std::unordered_map<int,std::vector<int>>& merged_filaments );

    private:
        FilamentGroupContext ctx;
        MemoryedGroupHeap m_memoryed_heap;
        std::vector<std::vector<int>> m_memoryed_groups;
        ClusteringBudget m_clustering_budget;
    public:
        std::optional<std::function<bool(int, std::vector<int>&)>> get_custom_seq;
    };


    std::vector<int> calc_filament_group_for_manual_multi_nozzle(const std::vector<int>& filament_map_manual,const FilamentGroupContext& ctx);

    std::vector<int> calc_filament_group_for_match_multi_nozzle(const FilamentGroupContext& ctx);

    struct FilamentPlanRes
    {
        std::vector<int> fil_order;
        std::vector<int> fil_nozzle_match;
    };

    std::vector<FilamentPlanRes> plan_filament_nozzle_mapping_and_order(const FilamentGroupContext& ctx);


    class KMediods
    {
    protected:
        using MemoryedGroupHeap = FilamentGroupUtils::MemoryedGroupHeap;
        using MemoryedGroup = FilamentGroupUtils::MemoryedGroup;
    public:
        KMediods(const int k, const int elem_count, const std::shared_ptr<FlushDistanceEvaluator>& evaluator, int default_group_id = 0) {
            m_k = k;
            m_evaluator = evaluator;
            m_max_cluster_size = std::vector<int>(k, DEFAULT_CLUSTER_SIZE);
            m_elem_count = elem_count;
            m_default_group_id = default_group_id;
        }
        // set max group size
        void set_max_cluster_size(const std::vector<int>& group_size) { m_max_cluster_size = group_size; }

        void set_cluster_group_size(const std::vector<std::pair<std::set<int>,int>>& cluster_group_size);

        // key stores elem, value stores the cluster id that the elem must be placed
        void set_placable_limits(const std::unordered_map<int, std::vector<int>>& placable_limits) { m_placeable_limits = placable_limits; }

        // key stores elem, value stores the cluster id that the elem cannot be placed
        void set_unplacable_limits(const std::unordered_map<int, std::vector<int>>& unplacable_limits) { m_unplaceable_limits = unplacable_limits; }

        void set_memory_threshold(double threshold) { memory_threshold = threshold; }
        MemoryedGroupHeap get_memoryed_groups()const { return memoryed_groups; }

        void do_clustering(const FilamentGroupContext& context, const ClusteringBudget& budget);
        std::vector<int> get_cluster_labels()const { return m_cluster_labels; }

    protected:
        bool have_enough_size(const std::vector<int>& cluster_size, const std::vector<std::pair<std::set<int>, int>>& cluster_group_size,int elem_count);
        // calculate cluster distance
        int calc_cost(const std::vector<int>& clusters, const std::vector<int>& cluster_centers, int cluster_id = -1);

        // get initial cluster center
        std::vector<int>init_cluster_center(const std::unordered_map<int, std::vector<int>>& placeable_limits, const std::unordered_map<int, std::vector<int>>& unplaceable_limits, const std::vector<int>& cluster_size, const std::vector<std::pair<std::set<int>, int>>& cluster_group_size, int seed);
        // assign each elem to the cluster
        std::vector<int> assign_cluster_label(const std::vector<int>& center, const std::unordered_map<int, std::vector<int>>& placeable_limits, const std::unordered_map<int, std::vector<int>>& unplaceable_limits, const std::vector<int>& group_size, const std::vector<std::pair<std::set<int>, int>>& cluster_group_size);

    protected:
        MemoryedGroupHeap memoryed_groups;
        std::shared_ptr<FlushDistanceEvaluator>m_evaluator;
        std::unordered_map<int, std::vector<int>> m_unplaceable_limits; // key: filament, value: nozzle ids it cannot be assigned to
        std::unordered_map<int, std::vector<int>> m_placeable_limits; // key: filament, value: nozzle ids it must be assigned to
        std::vector<int>m_max_cluster_size; // max number of filaments each nozzle can hold
        std::vector<int>m_cluster_labels;  // assignment result, resolved down to nozzle id
        std::vector<std::pair<std::set<int>,int>> m_cluster_group_size;
        std::vector<int> m_nozzle_to_extruder;


        int m_k;
        int m_elem_count;
        int m_default_group_id{ 0 };
        double memory_threshold{ 0 };
    };
}
#endif // !FILAMENT_GROUP_HPP
