#ifndef TOOL_ORDER_UTILS_HPP
#define TOOL_ORDER_UTILS_HPP

#include <vector>
#include <optional>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include "../MultiNozzleUtils.hpp"

namespace Slic3r {

using FlushMatrix = std::vector<std::vector<float>>;

namespace MaxFlowGraph {
    const int INF = std::numeric_limits<int>::max();
    const int INVALID_ID = -1;
    // Upper bound for MCMF edge cost to prevent int overflow in SPFA causing infinite loops
    constexpr int MCMF_MAX_EDGE_COST = 10000000;
}

// Namespace-scope edge shared by the max-flow / min-cost-max-flow solvers below.
// The default cost keeps the plain max-flow solvers (which never read cost) source-compatible.
struct Edge
{
    int from, to, capacity, cost, flow;
    Edge(int u, int v, int cap, int cst = 0) : from(u), to(v), capacity(cap), cost(cst), flow(0) {}
};

class MaxFlowSolver
{
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
struct MaxFlowWithLowerBounds;

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

class GeneralMinCostLowerBoundsSolver
{
public:
    GeneralMinCostLowerBoundsSolver(
        const std::vector<FlushMatrix> &matrix_,
        const std::vector<int>& u_nodes,
        const std::vector<int>& v_nodes,
        const std::vector<int>& v_nodes_group,
        const std::unordered_map<int, std::vector<int>>& uv_link_limits = {},
        const std::unordered_map<int, std::vector<int>>& uv_unlink_limits = {});

    std::vector<int> solve();
    ~GeneralMinCostLowerBoundsSolver();

private:
    void build_feasible_graph(const std::unordered_set<int>& no_lower_groups);

    void build_graph_with_feasible_result();

    void add_edge_with_lower_bound(int from, int to, int lower, int upper, int cost);

    int get_distance(const int idx_in_left,const int idx_in_right);

private:
    std::unique_ptr<MaxFlowWithLowerBounds> m_solver_lower_bounds;
    std::unique_ptr<MinCostMaxFlow> m_solver_min_cost;

    std::vector<FlushMatrix> flush_matrix;
    std::vector<int> l_nodes;
    std::vector<int> r_nodes;
    std::vector<int> r_nodes_group;
    std::unordered_map<int, std::vector<int>> m_uv_link_limits;
    std::unordered_map<int, std::vector<int>> m_uv_unlink_limits;
    int num_groups = 0;

    // support lower bounds
    struct LowerBoundEdge{
        int edge_id;
        int lower;
    };

    std::vector<int> demand;
    std::vector<LowerBoundEdge> lower_bound_edges;

    int super_source = -1;
    int super_sink = -1;
    int source_id = -1;
    int sink_id = -1;
    int max_flow_edges = 0;
};

class GroupMinCostFlowSolver
{
public:
    GroupMinCostFlowSolver(
        const std::vector<FlushMatrix> &matrix_,
        const std::vector<int> &u_nodes,
        const std::vector<int> &v_nodes,
        const std::vector<int> &v_nodes_group,
        const std::unordered_map<int, std::vector<int>> &uv_link_limits = {},
        const std::unordered_map<int, std::vector<int>> &uv_unlink_limits = {});

    std::vector<int> solve();
    ~GroupMinCostFlowSolver();

private:
    void build_graph();
    int get_flush_cost(int l_idx, int r_idx);

    std::unique_ptr<MinCostMaxFlow> m_solver;
    std::vector<FlushMatrix> flush_matrix;
    std::vector<int> l_nodes;
    std::vector<int> r_nodes;
    std::vector<int> r_nodes_group;
    std::unordered_map<int, std::vector<int>> m_uv_link_limits;
    std::unordered_map<int, std::vector<int>> m_uv_unlink_limits;
    int num_groups = 0;
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
        const std::vector<std::pair<std::set<int>, int>>& v_group_capacity = {}
    );
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
                                               std::vector<std::vector<unsigned int>> *filament_sequences,
                                               const std::unordered_map<int, int>& nozzle_status = {});

// Order filaments within a per-nozzle grouping result (multi-nozzle extruders). Threads a
// NozzleStatusRecorder describing the initial physical nozzle occupancy so the reorder can reward
// keeping an already-loaded filament in place.
int reorder_filaments_for_multi_nozzle_extruder(const std::vector<unsigned int>& filament_lists,
                                                const MultiNozzleUtils::LayeredNozzleGroupResult& nozzle_group_result,
                                                const std::vector<std::vector<unsigned int>>& layer_filaments,
                                                const std::vector<FlushMatrix>& flush_matrix,
                                                const std::function<bool(int,std::vector<int>&)> get_custom_seq,
                                                std::vector<std::vector<unsigned int>> * filament_sequences,
                                                const MultiNozzleUtils::NozzleStatusRecorder& initial_status = {});

}
#endif // !TOOL_ORDER_UTILS_HPP
