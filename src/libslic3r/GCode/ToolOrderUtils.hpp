#ifndef TOOL_ORDER_UTILS_HPP
#define TOOL_ORDER_UTILS_HPP

#include <vector>
#include <optional>
#include <functional>
#include <limits>
#include <memory>
#include <unordered_set>
#include "../MultiNozzleUtils.hpp"

#define DEBUG_MULTI_NOZZLE_MCMF 0
namespace Slic3r {

using FlushMatrix = std::vector<std::vector<float>>;

namespace MaxFlowGraph {
    const int INF = std::numeric_limits<int>::max();
    const int INVALID_ID = -1;
}

class MaxFlowSolver
{
private:
    struct Edge {
        int from, to, capacity, flow;
        Edge(int u, int v, int cap) :from(u), to(v), capacity(cap), flow(0) {}
    };
public:
    MaxFlowSolver(const std::vector<int>& u_nodes, const std::vector<int>& v_nodes,
        const std::unordered_map<int, std::vector<int>>& uv_link_limits = {},
        const std::unordered_map<int, std::vector<int>>& uv_unlink_limits = {},
        const std::vector<int>& u_capacity = {},
        const std::vector<int>& v_capacity = {},
        const std::vector<std::pair<std::set<int>, int>>& v_group_capacity = {}
    );
    std::vector<int> solve();

private:
    void add_edge(int from, int to, int capacity);

    int total_nodes;
    int source_id;
    int sink_id;
    std::vector<Edge>edges;
    std::vector<int>l_nodes;
    std::vector<int>r_nodes;
    std::vector<std::vector<int>>adj;
};


struct MinCostMaxFlow;

class GeneralMinCostSolver
{
public:
    GeneralMinCostSolver(const std::vector<std::vector<float>>& matrix_,
        const std::vector<int>& u_nodes,
        const std::vector<int>& v_nodes);

    std::vector<int> solve();
    ~GeneralMinCostSolver();
private:
    std::unique_ptr<MinCostMaxFlow> m_solver;
};


class MinFlushFlowSolver
{
public:
    MinFlushFlowSolver(const std::vector<std::vector<float>>& matrix_,
        const std::vector<int>& u_nodes,
        const std::vector<int>& v_nodes,
        const std::unordered_map<int, std::vector<int>>& uv_link_limits = {},
        const std::unordered_map<int, std::vector<int>>& uv_unlink_limits = {},
        const std::vector<int>& u_capacity = {},
        const std::vector<int>& v_capacity = {},
        const std::vector<std::pair<std::set<int>, int>>& v_group_capacity = {});

    std::vector<int> solve();
    ~MinFlushFlowSolver();
private:
    std::unique_ptr<MinCostMaxFlow> m_solver;
};


class MatchModeGroupSolver
{
public:
    MatchModeGroupSolver(const std::vector<std::vector<float>>& matrix_,
        const std::vector<int>& u_nodes,
        const std::vector<int>& v_nodes,
        const std::vector<int>& v_capacity,
        const std::unordered_map<int, std::vector<int>>& uv_unlink_limits = {});

    std::vector<int> solve();
    ~MatchModeGroupSolver();
private:
    std::unique_ptr<MinCostMaxFlow> m_solver;
};


#if DEBUG_MULTI_NOZZLE_MCMF
int reorder_filaments_for_multi_nozzle_extruder(const std::vector<unsigned int>& filament_lists,
                                                const std::vector<int>& filament_maps,
                                                const std::vector<std::vector<unsigned int>>& layer_filaments,
                                                const std::vector<FlushMatrix>& flush_matrix,
                                                std::optional<std::function<bool(int, std::vector<int> &)>> get_custom_seq,
                                                int multi_nozzle_extruder_id,
                                                int multi_nozzle_num,
                                                std::vector<std::vector<unsigned int>> * layer_sequences = nullptr,
                                                std::vector<std::vector<std::vector<int>>>* nozzle_match_per_layer = nullptr);

std::vector<unsigned int> get_extruders_order(const std::vector<std::vector<float>> &wipe_volumes,
    const std::vector<unsigned int> &curr_layer_extruders,
    const std::vector<unsigned int> &next_layer_extruders,
    const std::optional<unsigned int> &start_extruder_id,
    bool use_forcast = false,
    float *cost = nullptr);

#endif


int reorder_filaments_for_multi_nozzle_extruder(const std::vector<unsigned int>& filament_lists,
                                                const MultiNozzleUtils::MultiNozzleGroupResult& nozzle_group_result,
                                                const std::vector<std::vector<unsigned int>>& layer_filaments,
                                                const std::vector<FlushMatrix>& flush_matrix,
                                                const std::function<bool(int,std::vector<int>&)> get_custom_seq,
                                                std::vector<std::vector<unsigned int>> * filament_sequences);


std::vector<unsigned int> get_extruders_order(const std::vector<std::vector<float>> &wipe_volumes,
                                              const std::vector<unsigned int> &curr_layer_extruders,
                                              const std::vector<unsigned int> &next_layer_extruders,
                                              const std::optional<unsigned int> &start_extruder_id,
                                              bool use_forcast = false,
                                              float *cost = nullptr);

int reorder_filaments_for_minimum_flush_volume(const std::vector<unsigned int> &filament_lists,
                                               const std::vector<int> &filament_maps,
                                               const std::vector<std::vector<unsigned int>> &layer_filaments,
                                               const std::vector<FlushMatrix> &flush_matrix,
                                               std::optional<std::function<bool(int, std::vector<int> &)>> get_custom_seq,
                                               std::vector<std::vector<unsigned int>> *filament_sequences);

}
#endif // !TOOL_ORDER_UTILS_HPP
