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

const static int ABSOLUTE_FLUSH_GAP_TOLERANCE = 5;

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
            MemoryedGroup(const std::vector<int>& group_, const int cost_, const int prefer_level_) :group(group_), cost(cost_), prefer_level(prefer_level_) {}
            bool operator>(const MemoryedGroup& other) const {
                return prefer_level < other.prefer_level || (prefer_level == other.prefer_level && cost > other.cost);
            }

            int cost{ 0 };
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
        } model_info;

        struct GroupInfo {
            int total_filament_num;
            double max_gap_threshold;
            FGMode mode;
            FGStrategy strategy;
            bool ignore_ext_filament;  //wai gua filament
        } group_info;

        struct MachineInfo {
            std::vector<int> max_group_size;
            std::vector<std::vector<FilamentGroupUtils::MachineFilamentInfo>> machine_filament_info;
            std::vector<std::pair<std::set<int>, int>> extruder_group_size;
            int master_extruder_id;
        } machine_info;
    };

    std::vector<int> select_best_group_for_ams(const std::vector<std::vector<int>>& map_lists,
        const std::vector<unsigned int>& used_filaments,
        const std::vector<FilamentGroupUtils::FilamentInfo>& used_filament_info,
        const std::vector<std::vector<FilamentGroupUtils::MachineFilamentInfo>>& machine_filament_info,
        const double color_delta_threshold = 20);

    std::vector<int> optimize_group_for_master_extruder(const std::vector<unsigned int>& used_filaments, const FilamentGroupContext& ctx, const std::vector<int>& filament_map);

    bool can_swap_groups(const int extruder_id_0, const std::set<int>& group_0, const int extruder_id_1, const std::set<int>& group_1, const FilamentGroupContext& ctx);

    std::vector<int> calc_filament_group_for_tpu(const std::set<int>& tpu_filaments, const int filament_nums, const int master_extruder_id);

    class FlushDistanceEvaluator
    {
    public:
        FlushDistanceEvaluator(const FlushMatrix& flush_matrix,const std::vector<unsigned int>&used_filaments,const std::vector<std::vector<unsigned int>>& layer_filaments, double p = 0.65);
        ~FlushDistanceEvaluator() = default;
        double get_distance(int idx_a, int idx_b) const;
    private:
        std::vector<std::vector<float>>m_distance_matrix;

    };

    class FilamentGroup
    {
        using MemoryedGroup = FilamentGroupUtils::MemoryedGroup;
        using MemoryedGroupHeap = FilamentGroupUtils::MemoryedGroupHeap;
    public:
        explicit FilamentGroup(const FilamentGroupContext& ctx_) :ctx(ctx_) {}
    public:
        std::vector<int> calc_filament_group(int * cost = nullptr);
        std::vector<std::vector<int>> get_memoryed_groups()const { return m_memoryed_groups; }

    public:
        std::vector<int> calc_filament_group_for_match(int* cost = nullptr);
        std::vector<int> calc_filament_group_for_flush(int* cost = nullptr);

    private:
        std::vector<int> calc_min_flush_group(int* cost = nullptr);
        std::vector<int> calc_min_flush_group_by_enum(const std::vector<unsigned int>& used_filaments, int* cost = nullptr);
        std::vector<int> calc_min_flush_group_by_pam2(const std::vector<unsigned int>& used_filaments, int* cost = nullptr, int timeout_ms = 300);

        std::unordered_map<int, std::vector<int>> try_merge_filaments();
        void rebuild_context(const std::unordered_map<int, std::vector<int>>& merged_filaments);
        std::vector<int> seperate_merged_filaments(const std::vector<int>& filament_map, const std::unordered_map<int,std::vector<int>>& merged_filaments );

    private:
        FilamentGroupContext ctx;
        std::vector<std::vector<int>> m_memoryed_groups;

    public:
        std::optional<std::function<bool(int, std::vector<int>&)>> get_custom_seq;
    };


    class KMediods2
    {
        using MemoryedGroupHeap = FilamentGroupUtils::MemoryedGroupHeap;
        using MemoryedGroup = FilamentGroupUtils::MemoryedGroup;

        enum INIT_TYPE
        {
            Random = 0,
            Farthest
        };
    public:
        KMediods2(const int elem_count, const std::shared_ptr<FlushDistanceEvaluator>& evaluator, int default_group_id = 0) :
            m_evaluator{ evaluator },
            m_elem_count{ elem_count },
            m_default_group_id{ default_group_id }
        {
            m_max_cluster_size = std::vector<int>(m_k, DEFAULT_CLUSTER_SIZE);
        }

        // set max group size
        void set_max_cluster_size(const std::vector<int>& group_size) { m_max_cluster_size = group_size; }

        // key stores elem idx, value stores the cluster id that elem cnanot be placed
        void set_unplaceable_limits(const std::map<int, int>& placeable_limits) { m_unplaceable_limits = placeable_limits; }

        void do_clustering(const FGStrategy& g_strategy,int timeout_ms = 100);

        void set_memory_threshold(double threshold) { memory_threshold = threshold; }
        MemoryedGroupHeap get_memoryed_groups()const { return memoryed_groups; }

        std::vector<int>get_cluster_labels()const { return m_cluster_labels; }

    private:
        std::vector<int>cluster_small_data(const std::map<int, int>& unplaceable_limits, const std::vector<int>& group_size);
        std::vector<int>assign_cluster_label(const std::vector<int>& center, const std::map<int, int>& unplaceable_limits, const std::vector<int>& group_size, const FGStrategy& strategy);
        int calc_cost(const std::vector<int>& labels, const std::vector<int>& medoids);
    protected:
        FilamentGroupUtils::MemoryedGroupHeap memoryed_groups;
        std::shared_ptr<FlushDistanceEvaluator> m_evaluator;
        std::map<int, int>m_unplaceable_limits;
        std::vector<int>m_cluster_labels;
        std::vector<int>m_max_cluster_size;

        const int m_k = 2;
        int m_elem_count;
        int m_default_group_id{ 0 };
        double memory_threshold{ 0 };
    };
}
#endif // !FILAMENT_GROUP_HPP
