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

    class FilamentGroupMultiNozzle;

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
            bool ignore_ext_filament;
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
        } speed_info;

        struct NozzleInfo {
            std::map<int, std::vector<int>> extruder_nozzle_list;
            std::vector<MultiNozzleUtils::NozzleInfo> nozzle_list;
        } nozzle_info;
    };

    std::vector<int> select_best_group_for_ams(const std::vector<std::vector<int>> &filament_to_nozzles,
        const std::vector<MultiNozzleUtils::NozzleInfo>& nozzle_list,
        const std::vector<unsigned int>& used_filaments,
        const std::vector<FilamentGroupUtils::FilamentInfo>& used_filament_info,
        const std::vector<std::vector<FilamentGroupUtils::MachineFilamentInfo>>& machine_filament_info,
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
        std::vector<int> calc_min_flush_group_by_pam (const std::vector<unsigned int>& used_filaments, int* cost = nullptr, int timeout_ms = 300, int retry = 15);

        std::unordered_map<int, std::vector<int>> try_merge_filaments();
        void rebuild_context(const std::unordered_map<int, std::vector<int>>& merged_filaments);
        std::vector<int> seperate_merged_filaments(const std::vector<int>& filament_map, const std::unordered_map<int,std::vector<int>>& merged_filaments );

    private:
        FilamentGroupContext ctx;
        std::vector<std::vector<int>> m_memoryed_groups;
        friend FilamentGroupMultiNozzle;

    public:
        std::optional<std::function<bool(int, std::vector<int>&)>> get_custom_seq;
    };

    class FilamentGroupMultiNozzle
    {
    public:
        FilamentGroupMultiNozzle(const FilamentGroupContext& context) :m_context(context) {}
    public:
        std::vector<int> calc_filament_group_by_mcmf();
        std::vector<int> calc_filament_group_by_pam();

    private:
        std::unordered_map<int, std::vector<int>> rebuild_nozzle_unprintables(const std::vector<unsigned int>& used_filaments, const std::unordered_map<int, std::vector<int>>& extruder_unprintables, const std::vector<int>& filament_volume_map);
    private:
        FilamentGroupContext m_context;
    };

    std::vector<int> calc_filament_group_for_manual_multi_nozzle(const std::vector<int>& filament_map_manual,const FilamentGroupContext& ctx);

    std::vector<int> calc_filament_group_for_match_multi_nozzle(const FilamentGroupContext& ctx);

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

    // non_zero_clusters
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

        void do_clustering(const FilamentGroupContext& context, int timeout_ms = 100, int retry = 10);
        std::vector<int> get_cluster_labels()const { return m_cluster_labels; }

    protected:
        bool have_enough_size(const std::vector<int>& cluster_size, const std::vector<std::pair<std::set<int>, int>>& cluster_group_size,int elem_count);
        // calculate cluster distance
        int calc_cost(const std::vector<int>& clusters, const std::vector<int>& cluster_centers, int cluster_id = -1);

        std::vector<int> cluster_small_data(const FilamentGroupContext& context);
        // get initial cluster center
        std::vector<int>init_cluster_center(const std::unordered_map<int, std::vector<int>>& placeable_limits, const std::unordered_map<int, std::vector<int>>& unplaceable_limits, const std::vector<int>& cluster_size, const std::vector<std::pair<std::set<int>, int>>& cluster_group_size, int seed);
        // assign each elem to the cluster
        std::vector<int> assign_cluster_label(const std::vector<int>& center, const std::unordered_map<int, std::vector<int>>& placeable_limits, const std::unordered_map<int, std::vector<int>>& unplaceable_limits, const std::vector<int>& group_size, const std::vector<std::pair<std::set<int>, int>>& cluster_group_size);

    protected:
        MemoryedGroupHeap memoryed_groups;
        std::shared_ptr<FlushDistanceEvaluator>m_evaluator;
        std::unordered_map<int, std::vector<int>> m_unplaceable_limits; // 材料不允许分配到特定喷嘴
        std::unordered_map<int, std::vector<int>> m_placeable_limits; // 材料必须分配到特定喷嘴
        std::vector<int>m_max_cluster_size; // 每个喷嘴能够分配的最大耗材数量
        std::vector<int>m_cluster_labels;  // 分配结果，细化到喷嘴id
        std::vector<std::pair<std::set<int>,int>> m_cluster_group_size;
        std::vector<int> m_nozzle_to_extruder;


        int m_k;
        int m_elem_count;
        int m_default_group_id{ 0 };
        double memory_threshold{ 0 };
    };
}
#endif // !FILAMENT_GROUP_HPP
