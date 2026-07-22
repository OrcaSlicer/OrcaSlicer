#include "ToolOrderUtils.hpp"
#include <queue>
#include <set>
#include <map>
#include <cmath>
#include <boost/multiprecision/cpp_int.hpp>

namespace Slic3r
{
// ==================== MaxFlowWithLowerBounds ====================
    struct MaxFlowWithLowerBounds {
    public:

        void add_edge(int from, int to, int capacity);

        bool bfs();
        int dfs(int u, int f);
        int solve(std::vector<int>& matching);

    public:
        std::vector<int> l_nodes;
        std::vector<int> r_nodes;
        std::vector<Edge> edges;
        std::vector<std::vector<int>> adj;
        std::vector<int> level;
        std::vector<int> it;

        int total_nodes{ -1 };
        int source_id{ -1 };
        int sink_id{ -1 };
    };

    void MaxFlowWithLowerBounds::add_edge(int from, int to, int capacity)
    {
        adj[from].emplace_back(edges.size());
        edges.emplace_back(from, to, capacity, 0);
        // also add the reverse residual edge with zero capacity
        adj[to].emplace_back(edges.size());
        edges.emplace_back(to, from, 0, 0);
    }

    bool MaxFlowWithLowerBounds::bfs() {
        level.assign(total_nodes, -1);
        std::queue<int> q;
        q.push(source_id);
        level[source_id] = 0;

        while (!q.empty()) {
            int u = q.front(); q.pop();
            for (int eid : adj[u]) {
                Edge &e = edges[eid];
                if (e.flow < e.capacity && level[e.to] == -1) {
                    level[e.to] = level[u] + 1;
                    q.push(e.to);
                }
            }
        }
        return level[sink_id] != -1;
    }

    int MaxFlowWithLowerBounds::dfs(int u, int f) {
        if (u == sink_id) return f;
        for (int &i = it[u]; i < (int)adj[u].size(); ++i) {
            int eid = adj[u][i];
            Edge &e = edges[eid];
            if (e.flow < e.capacity && level[e.to] == level[u] + 1) {
                int pushed = dfs(e.to, std::min(f, e.capacity - e.flow));
                if (pushed > 0) {
                    e.flow += pushed;
                    edges[eid ^ 1].flow -= pushed;
                    return pushed;
                }
            }
        }
        return 0;
    }

    int MaxFlowWithLowerBounds::solve(std::vector<int>& matching) {
        int flow = 0;
        while (bfs()) {
            it.assign(total_nodes, 0);
            while (int pushed = dfs(source_id, MaxFlowGraph::INF))
                flow += pushed;
        }

        int L = l_nodes.size();
        int R = r_nodes.size();
        // collect l-r matches
        matching.resize(l_nodes.size(), MaxFlowGraph::INVALID_ID);
        for (int u = 0; u < L; ++u) {
            for (int eid : adj[u]) {
                Edge &e = edges[eid];
                if (e.flow > 0 && e.to >= L && e.to < L + R) {
                    matching[e.from] = e.to - L;
                }
            }
        }
        return flow;
    }

// ==================== MinCostMaxFlow ====================
    struct MinCostMaxFlow {
    public:
        std::vector<int> solve();
        void add_edge(int from, int to, int capacity, int cost);
        bool spfa(int source, int sink);
        int get_distance(int idx_in_left, int idx_in_right);

        std::vector<std::vector<float>> matrix;
        std::vector<int> l_nodes;
        std::vector<int> r_nodes;
        std::vector<Edge> edges;
        std::vector<std::vector<int>> adj;

        int total_nodes{ -1 };
        int source_id{ -1 };
        int sink_id{ -1 };
    };

    std::vector<int> MinCostMaxFlow::solve()
    {
        while (spfa(source_id, sink_id));

        std::vector<int>matching(l_nodes.size(), MaxFlowGraph::INVALID_ID);
        // to get the match info, just traverse the left nodes and
        // check the edges with flow > 0 and linked to right nodes
        for (int u = 0; u < l_nodes.size(); ++u) {
            for (int eid : adj[u]) {
                Edge& e = edges[eid];
                if (e.flow > 0 && e.to >= l_nodes.size() && e.to < l_nodes.size() + r_nodes.size())
                    matching[e.from] = r_nodes[e.to - l_nodes.size()];
            }
        }

        return matching;
    }

    void MinCostMaxFlow::add_edge(int from, int to, int capacity, int cost)
    {
        adj[from].emplace_back(edges.size());
        edges.emplace_back(from, to, capacity, cost);
        //also add reverse edge ,set capacity to zero,cost to negative
        adj[to].emplace_back(edges.size());
        edges.emplace_back(to, from, 0, -cost);
    }

    bool MinCostMaxFlow::spfa(int source, int sink)
    {
        std::vector<int>dist(total_nodes, MaxFlowGraph::INF);
        std::vector<bool>in_queue(total_nodes, false);
        std::vector<int>flow(total_nodes, MaxFlowGraph::INF);
        std::vector<int>prev(total_nodes, 0);

        std::queue<int>q;
        q.push(source);
        in_queue[source] = true;
        dist[source] = 0;

        while (!q.empty()) {
            int now_at = q.front();
            q.pop();
            in_queue[now_at] = false;

            for (auto eid : adj[now_at]) //traverse all linked edges
            {
                Edge& e = edges[eid];
                if (e.flow<e.capacity && dist[e.to]>dist[now_at] + e.cost) {
                    dist[e.to] = dist[now_at] + e.cost;
                    prev[e.to] = eid;
                    flow[e.to] = std::min(flow[now_at], e.capacity - e.flow);
                    if (!in_queue[e.to]) {
                        q.push(e.to);
                        in_queue[e.to] = true;
                    }
                }
            }
        }

        if (dist[sink] == MaxFlowGraph::INF)
            return false;

        int now_at = sink;
        while (now_at != source) {
            int prev_edge = prev[now_at];
            edges[prev_edge].flow += flow[sink];
            edges[prev_edge ^ 1].flow -= flow[sink];
            now_at = edges[prev_edge].from;
        }

        return true;
    }

    int MinCostMaxFlow::get_distance(int idx_in_left, int idx_in_right)
    {
        if (l_nodes[idx_in_left] == -1) {
            return 0;
        }

        float val = matrix[l_nodes[idx_in_left]][r_nodes[idx_in_right]];
        return std::min(static_cast<int>(val), MaxFlowGraph::MCMF_MAX_EDGE_COST);
    }


    MaxFlowSolver::MaxFlowSolver(const std::vector<int>& u_nodes, const std::vector<int>& v_nodes,
        const std::unordered_map<int, std::vector<int>>& uv_link_limits,
        const std::unordered_map<int, std::vector<int>>& uv_unlink_limits,
        const std::vector<int>& u_capacity,
        const std::vector<int>& v_capacity,
        const std::vector<std::pair<std::set<int>,int>>& v_group_capacity)
    {
        assert(u_capacity.empty() || u_capacity.size() == u_nodes.size());
        assert(v_capacity.empty() || v_capacity.size() == v_nodes.size());
        l_nodes = u_nodes;
        r_nodes = v_nodes;
        total_nodes = u_nodes.size() + v_nodes.size() + v_group_capacity.size() + 2;
        source_id = total_nodes - 2;
        sink_id = total_nodes - 1;

        adj.resize(total_nodes);

        std::vector<int>v_node_to(v_nodes.size(), sink_id);
        for (size_t gid = 0; gid < v_group_capacity.size(); ++gid) {
            for (auto vid : v_group_capacity[gid].first)
                v_node_to[vid] = l_nodes.size() + r_nodes.size() + gid;
        }

        // add edge from source to left nodes
        for (int idx = 0; idx < l_nodes.size(); ++idx) {
            int capacity = u_capacity.empty() ? 1 : u_capacity[idx];
            add_edge(source_id, idx, capacity);
        }
        // add edge from right nodes to v_node_to(sink node or temp group node)
        for (int idx = 0; idx < r_nodes.size(); ++idx) {
            int capacity = v_capacity.empty() ? 1 : v_capacity[idx];
            add_edge(l_nodes.size() + idx, v_node_to[idx], capacity);
        }

        // add edge from temp group node to sink node
        for (int idx = 0; idx < v_group_capacity.size(); ++idx) {
            int capacity = v_group_capacity[idx].second;
            add_edge(l_nodes.size() + r_nodes.size() + idx, sink_id, capacity);
        }

        // add edge from left nodes to right nodes
        for (int i = 0; i < l_nodes.size(); ++i) {
            int from_idx = i;
            // process link limits , i can only link to uv_link_limits
            if (auto iter = uv_link_limits.find(i); iter != uv_link_limits.end()) {
                for (auto r_id : iter->second)
                    add_edge(from_idx, l_nodes.size() + r_id, 1);
                continue;
            }
            // process unlink limits
            std::optional<std::vector<int>> unlink_limits;
            if (auto iter = uv_unlink_limits.find(i); iter != uv_unlink_limits.end())
                unlink_limits = iter->second;

            for (int j = 0; j < r_nodes.size(); ++j) {
                // check whether i can link to j
                if (unlink_limits.has_value() && std::find(unlink_limits->begin(), unlink_limits->end(), j) != unlink_limits->end())
                    continue;
                add_edge(from_idx, l_nodes.size() + j, 1);
            }
        }
    }

    void MaxFlowSolver::add_edge(int from, int to, int capacity)
    {
        adj[from].emplace_back(edges.size());
        edges.emplace_back(from, to, capacity);
        //also add reverse edge ,set capacity to zero
        adj[to].emplace_back(edges.size());
        edges.emplace_back(to, from, 0);
    }

    std::vector<int> MaxFlowSolver::solve() {
        std::vector<int> augment;
        std::vector<int> previous(total_nodes, 0);
        while (1) {
            std::vector<int>(total_nodes, 0).swap(augment);
            std::queue<int> travel;
            travel.push(source_id);
            augment[source_id] = MaxFlowGraph::INF;
            while (!travel.empty()) {
                int from = travel.front();
                travel.pop();

                // traverse all linked edges
                for (int i = 0; i < adj[from].size(); ++i) {
                    int eid = adj[from][i];
                    Edge& tmp = edges[eid];
                    if (augment[tmp.to] == 0 && tmp.capacity > tmp.flow) {
                        previous[tmp.to] = eid;
                        augment[tmp.to] = std::min(augment[from], tmp.capacity - tmp.flow);
                        travel.push(tmp.to);
                    }
                }

                // already find an extend path, stop and do update
                if (augment[sink_id] != 0)
                    break;
            }
            // no longer have extend path
            if (augment[sink_id] == 0)
                break;

            for (int i = sink_id; i != source_id; i = edges[previous[i]].from) {
                edges[previous[i]].flow += augment[sink_id];
                edges[previous[i] ^ 1].flow -= augment[sink_id];
            }
        }

        std::vector<int> matching(l_nodes.size(), MaxFlowGraph::INVALID_ID);
        // to get the match info, just traverse the left nodes and
        // check the edge with flow > 0 and linked to right nodes
        for (int u = 0; u < l_nodes.size(); ++u) {
            for (int eid : adj[u]) {
                Edge& e = edges[eid];
                if (e.flow > 0 && e.to >= l_nodes.size() && e.to < l_nodes.size() + r_nodes.size())
                    matching[e.from] = r_nodes[e.to - l_nodes.size()];
            }
        }
        return matching;
    }

    GeneralMinCostSolver::~GeneralMinCostSolver()
    {
    }

    GeneralMinCostSolver::GeneralMinCostSolver(const std::vector<std::vector<float>>& matrix_, const std::vector<int>& u_nodes, const std::vector<int>& v_nodes)
    {
        m_solver = std::make_unique<MinCostMaxFlow>();
        m_solver->matrix = matrix_;;
        m_solver->l_nodes = u_nodes;
        m_solver->r_nodes = v_nodes;

        m_solver->total_nodes = u_nodes.size() + v_nodes.size() + 2;

        m_solver->source_id =m_solver->total_nodes - 2;
        m_solver->sink_id = m_solver->total_nodes - 1;

        m_solver->adj.resize(m_solver->total_nodes);


        // add edge from source to left nodes,cost to 0
        for (int i = 0; i < m_solver->l_nodes.size(); ++i)
            m_solver->add_edge(m_solver->source_id, i, 1, 0);

        // add edge from right nodes to sink,cost to 0
        for (int i = 0; i < m_solver->r_nodes.size(); ++i)
            m_solver->add_edge(m_solver->l_nodes.size() + i, m_solver->sink_id, 1, 0);

        // add edge from left node to right nodes
        for (int i = 0; i < m_solver->l_nodes.size(); ++i) {
            int from_idx = i;
            for (int j = 0; j < m_solver->r_nodes.size(); ++j) {
                int to_idx = m_solver->l_nodes.size() + j;
                m_solver->add_edge(from_idx, to_idx, 1, m_solver->get_distance(i, j));
            }
        }
    }

    std::vector<int> GeneralMinCostSolver::solve() {
        return m_solver->solve();
    }

// ==================== GeneralMinCostLowerBoundsSolver ====================
    GeneralMinCostLowerBoundsSolver::~GeneralMinCostLowerBoundsSolver() = default;

    GeneralMinCostLowerBoundsSolver::GeneralMinCostLowerBoundsSolver(const std::vector<FlushMatrix>                  &matrix_,
                                                                     const std::vector<int>                          &u_nodes,
                                                                     const std::vector<int>                          &v_nodes,
                                                                     const std::vector<int>                          &v_nodes_group,
                                                                     const std::unordered_map<int, std::vector<int>> &uv_link_limits,
                                                                     const std::unordered_map<int, std::vector<int>> &uv_unlink_limits)
    {
        flush_matrix       = matrix_;
        l_nodes            = u_nodes;
        r_nodes            = v_nodes;
        r_nodes_group      = v_nodes_group;
        m_uv_link_limits   = uv_link_limits;
        m_uv_unlink_limits = uv_unlink_limits;
        num_groups         = *std::max_element(r_nodes_group.begin(), r_nodes_group.end()) + 1;

        m_solver_lower_bounds = std::make_unique<MaxFlowWithLowerBounds>();
        m_solver_min_cost = std::make_unique<MinCostMaxFlow>();
    }

    std::vector<int> GeneralMinCostLowerBoundsSolver::solve()
    {
        // group nodes that do not need a lower-bound constraint
        std::unordered_set<int> no_lower_group;
        for (int i = 0; i < r_nodes.size(); i++) {
            if (r_nodes[i] >= 0)
                no_lower_group.insert(r_nodes_group[i]);
        }

        // 1. build the lower-bound network graph
        build_feasible_graph(no_lower_group);

        // 2. compute the max flow
        int need = 0;
        for (int d : demand)
            if (d > 0) need += d;
        std::vector<int> feasible_matching;
        int pushed_flow = m_solver_lower_bounds->solve(feasible_matching);
        assert(need == pushed_flow);

        // 3. convert the lower-bound max-flow network into a min-cost-max-flow network
        build_graph_with_feasible_result();
        // 4. compute the min-cost max-flow
        auto min_cost_matching = m_solver_min_cost->solve();

        return min_cost_matching;
    }

    void GeneralMinCostLowerBoundsSolver::build_feasible_graph(const std::unordered_set<int> &no_lower_groups)
    {
        m_solver_lower_bounds->l_nodes = l_nodes;
        m_solver_lower_bounds->r_nodes = r_nodes;
        m_solver_lower_bounds->total_nodes = l_nodes.size() + r_nodes.size() + num_groups + 2;

        m_solver_lower_bounds->source_id = m_solver_lower_bounds->total_nodes - 2;
        m_solver_lower_bounds->sink_id   = m_solver_lower_bounds->total_nodes - 1;
        m_solver_lower_bounds->adj.resize(m_solver_lower_bounds->total_nodes);
        demand.resize(m_solver_lower_bounds->total_nodes, 0);

        const int L = m_solver_lower_bounds->l_nodes.size();
        const int R = m_solver_lower_bounds->r_nodes.size();

        // source -> l
        for (int i = 0; i < L; ++i)
            m_solver_lower_bounds->add_edge(m_solver_lower_bounds->source_id, i, 1);

        // u -> v (with link/unlink limits)
        for (int i = 0; i < L; ++i) {
            if (auto it = m_uv_link_limits.find(i); it != m_uv_link_limits.end()) {
                for (int j : it->second)
                    m_solver_lower_bounds->add_edge(i, L + j, 1);
                continue;
            }

            std::optional<std::vector<int>> unlink_limits;
            if (auto it = m_uv_unlink_limits.find(i); it != m_uv_unlink_limits.end())
                unlink_limits = it->second;

            for (int j = 0; j < R; ++j) {
                if (unlink_limits.has_value() && std::find(unlink_limits->begin(), unlink_limits->end(), j) != unlink_limits->end())
                    continue;
                m_solver_lower_bounds->add_edge(i, L + j, 1);
            }
        }

        // r -> group
        for (int j = 0; j < R; ++j) {
            int g = r_nodes_group[j];
            m_solver_lower_bounds->add_edge(L + j, L + R + g, 1);
        }

        // group -> sink (lower bound = 1)
        for (int g = 0; g < num_groups; ++g) {
            if (no_lower_groups.count(g))
                m_solver_lower_bounds->add_edge(L + R + g, m_solver_lower_bounds->sink_id, R);
            else
                add_edge_with_lower_bound(L + R + g, m_solver_lower_bounds->sink_id, 1, R, 0);
        }

        max_flow_edges = m_solver_lower_bounds->edges.size();

        // support lower bounds, add super source  super sink
        super_source = m_solver_lower_bounds->total_nodes++;
        super_sink = m_solver_lower_bounds->total_nodes++;

        m_solver_lower_bounds->adj.resize(m_solver_lower_bounds->total_nodes);
        demand.resize(m_solver_lower_bounds->total_nodes, 0);

        for (int i = 0; i < super_source; ++i) {
            if (demand[i] > 0) {
                m_solver_lower_bounds->add_edge(super_source, i, demand[i]);
            } else if (demand[i] < 0) {
                m_solver_lower_bounds->add_edge(i, super_sink, -demand[i]);
            }
        }
        m_solver_lower_bounds->add_edge(m_solver_lower_bounds->sink_id, m_solver_lower_bounds->source_id, MaxFlowGraph::INF);
        source_id = m_solver_lower_bounds->source_id;
        sink_id = m_solver_lower_bounds->sink_id;
        m_solver_lower_bounds->source_id = super_source;
        m_solver_lower_bounds->sink_id = super_sink;
    }

    void GeneralMinCostLowerBoundsSolver::build_graph_with_feasible_result()
    {
        for (auto&lb:lower_bound_edges){
            m_solver_lower_bounds->edges[lb.edge_id].flow += lb.lower;
            m_solver_lower_bounds->edges[lb.edge_id ^ 1].flow -= lb.lower;
        }

        m_solver_min_cost->l_nodes = m_solver_lower_bounds->l_nodes;
        m_solver_min_cost->r_nodes = m_solver_lower_bounds->r_nodes;

        m_solver_min_cost->source_id = source_id;
        m_solver_min_cost->sink_id = sink_id;
        m_solver_min_cost->total_nodes = sink_id + 1;

        m_solver_min_cost->edges = m_solver_lower_bounds->edges;
        m_solver_min_cost->edges.erase(m_solver_min_cost->edges.begin() + max_flow_edges, m_solver_min_cost->edges.end());

        m_solver_min_cost->adj = m_solver_lower_bounds->adj;
        m_solver_min_cost->adj.resize(m_solver_min_cost->total_nodes);
        for (auto &node_edges : m_solver_min_cost->adj) {
            node_edges.erase(std::remove_if(node_edges.begin(), node_edges.end(), [this](int val) {return val >= this->max_flow_edges;}), node_edges.end());
        }


        for (auto& e : m_solver_min_cost->edges) {
            int L            = m_solver_min_cost->l_nodes.size();
            int R            = m_solver_min_cost->r_nodes.size();

            if (e.from < L && e.to >= L && e.to < L + R) {
                int idx_in_left  = e.from;
                int idx_in_right = e.to - L;
                int group_id = r_nodes_group[idx_in_right];

                if (r_nodes[idx_in_right] == -1) continue;
                e.cost = flush_matrix[group_id][l_nodes[idx_in_left]][r_nodes[idx_in_right]];
            }
        }
    }
    void GeneralMinCostLowerBoundsSolver::add_edge_with_lower_bound(int from, int to, int lower, int upper, int cost)
    {
        int eid = m_solver_lower_bounds->edges.size();
        m_solver_lower_bounds->add_edge(from, to, upper - lower);

        lower_bound_edges.push_back({eid, lower});
        demand[from] -= lower;
        demand[to]   += lower;
    }

// ==================== GroupMinCostFlowSolver ====================
    GroupMinCostFlowSolver::~GroupMinCostFlowSolver() = default;

    GroupMinCostFlowSolver::GroupMinCostFlowSolver(const std::vector<FlushMatrix>                  &matrix_,
                                                   const std::vector<int>                          &u_nodes,
                                                   const std::vector<int>                          &v_nodes,
                                                   const std::vector<int>                          &v_nodes_group,
                                                   const std::unordered_map<int, std::vector<int>> &uv_link_limits,
                                                   const std::unordered_map<int, std::vector<int>> &uv_unlink_limits)
    {
        flush_matrix       = matrix_;
        l_nodes            = u_nodes;
        r_nodes            = v_nodes;
        r_nodes_group      = v_nodes_group;
        m_uv_link_limits   = uv_link_limits;
        m_uv_unlink_limits = uv_unlink_limits;
        num_groups         = *std::max_element(r_nodes_group.begin(), r_nodes_group.end()) + 1;

        m_solver = std::make_unique<MinCostMaxFlow>();
        build_graph();
    }

    int GroupMinCostFlowSolver::get_flush_cost(int l_idx, int r_idx)
    {
        if (r_nodes[r_idx] == -1)
            return 0;
        int group_id = r_nodes_group[r_idx];
        return (int)flush_matrix[group_id][l_nodes[l_idx]][r_nodes[r_idx]];
    }

    void GroupMinCostFlowSolver::build_graph()
    {
        const int L = (int)l_nodes.size();
        const int R = (int)r_nodes.size();
        const int G = num_groups;

        m_solver->l_nodes    = l_nodes;
        m_solver->r_nodes    = r_nodes;
        m_solver->total_nodes = L + R + G + 2;
        m_solver->source_id  = L + R + G;
        m_solver->sink_id    = L + R + G + 1;
        m_solver->adj.resize(m_solver->total_nodes);

        int max_flush = 0;
        for (const auto &mat : flush_matrix)
            for (const auto &row : mat)
                for (float v : row)
                    max_flush = std::max(max_flush, (int)v);
        int bonus = max_flush * L + 1;

        // source -> l_i
        for (int i = 0; i < L; ++i)
            m_solver->add_edge(m_solver->source_id, i, 1, 0);

        // l_i -> r_j (with link/unlink limits)
        for (int i = 0; i < L; ++i) {
            if (auto it = m_uv_link_limits.find(i); it != m_uv_link_limits.end()) {
                for (int j : it->second)
                    m_solver->add_edge(i, L + j, 1, get_flush_cost(i, j));
                continue;
            }

            std::optional<std::vector<int>> unlink_limits;
            if (auto it = m_uv_unlink_limits.find(i); it != m_uv_unlink_limits.end())
                unlink_limits = it->second;

            for (int j = 0; j < R; ++j) {
                if (unlink_limits.has_value() && std::find(unlink_limits->begin(), unlink_limits->end(), j) != unlink_limits->end())
                    continue;
                m_solver->add_edge(i, L + j, 1, get_flush_cost(i, j));
            }
        }

        // r_j -> group_g
        // Compute per-nozzle incoming edge count as capacity upper bound.
        // When unlink_limits restrict multiple filaments to the same nozzle,
        // capacity=1 would block valid assignments. Using the actual in-degree
        // allows the necessary flow while still preserving nozzle-level balance
        // (a nozzle with fewer forced filaments keeps a tighter cap).
        // The first unit carries a small nozzle-bonus to encourage spreading
        // filaments across distinct nozzles within the same group.
        int nozzle_bonus = max_flush + 1;
        std::vector<int> r_in_degree(R, 0);
        for (int i = 0; i < L; ++i) {
            if (auto it = m_uv_link_limits.find(i); it != m_uv_link_limits.end()) {
                for (int j : it->second)
                    r_in_degree[j]++;
                continue;
            }
            std::optional<std::vector<int>> unlink_limits;
            if (auto it = m_uv_unlink_limits.find(i); it != m_uv_unlink_limits.end())
                unlink_limits = it->second;
            for (int j = 0; j < R; ++j) {
                if (unlink_limits.has_value() && std::find(unlink_limits->begin(), unlink_limits->end(), j) != unlink_limits->end())
                    continue;
                r_in_degree[j]++;
            }
        }

        for (int j = 0; j < R; ++j) {
            int g = r_nodes_group[j];
            int cap = std::max(r_in_degree[j], 1);
            // First unit gets -nozzle_bonus to prefer using distinct nozzles
            m_solver->add_edge(L + j, L + R + g, 1, -nozzle_bonus);
            if (cap > 1)
                m_solver->add_edge(L + j, L + R + g, cap - 1, 0);
        }

        // group_g -> sink (split: first unit gets -bonus, rest gets 0)
        // bonus >> nozzle_bonus, so group coverage always takes priority
        for (int g = 0; g < G; ++g) {
            m_solver->add_edge(L + R + g, m_solver->sink_id, 1, -bonus);
            if (L > 1)
                m_solver->add_edge(L + R + g, m_solver->sink_id, L - 1, 0);
        }
    }

    std::vector<int> GroupMinCostFlowSolver::solve()
    {
        return m_solver->solve();
    }

// ==================== MinFlushFlowSolver ====================
    MinFlushFlowSolver::~MinFlushFlowSolver()
    {
    }

    MinFlushFlowSolver::MinFlushFlowSolver(const std::vector<std::vector<float>>& matrix_, const std::vector<int>& u_nodes, const std::vector<int>& v_nodes,
        const std::unordered_map<int, std::vector<int>>& uv_link_limits,
        const std::unordered_map<int, std::vector<int>>& uv_unlink_limits,
        const std::vector<int>& u_capacity,
        const std::vector<int>& v_capacity,
        const std::vector<std::pair<std::set<int>,int>>&v_group_capacity)
    {
        assert(u_capacity.empty() || u_capacity.size() == u_nodes.size());
        assert(v_capacity.empty() || v_capacity.size() == v_nodes.size());
        m_solver = std::make_unique<MinCostMaxFlow>();
        m_solver->matrix = matrix_;;
        m_solver->l_nodes = u_nodes;
        m_solver->r_nodes = v_nodes;

        m_solver->total_nodes = u_nodes.size() + v_nodes.size() + v_group_capacity.size() + 2;

        m_solver->source_id =m_solver->total_nodes - 2;
        m_solver->sink_id = m_solver->total_nodes - 1;

        m_solver->adj.resize(m_solver->total_nodes);

        std::vector<int> v_node_to(v_nodes.size(), m_solver->sink_id);
        for (size_t gid = 0; gid < v_group_capacity.size(); ++gid) {
            for (auto vid : v_group_capacity[gid].first)
                v_node_to[vid] = m_solver->l_nodes.size() + m_solver->r_nodes.size() + gid;
        }

        // add edge from source to left nodes,cost to 0
        for (int i = 0; i < m_solver->l_nodes.size(); ++i) {
            int capacity = u_capacity.empty() ? 1 : u_capacity[i];
            m_solver->add_edge(m_solver->source_id, i, capacity, 0);
        }
        // add edge from right nodes to sink,cost to 0
        for (int i = 0; i < m_solver->r_nodes.size(); ++i) {
            int capacity = v_capacity.empty() ? 1 : v_capacity[i];
            m_solver->add_edge(m_solver->l_nodes.size() + i, v_node_to[i], capacity, 0);
        }
        // add edge from temp group node to sink node
        for(int i=0;i<v_group_capacity.size();++i){
            int capacity = v_group_capacity[i].second;
            m_solver->add_edge(m_solver->l_nodes.size() + m_solver->r_nodes.size() + i, m_solver->sink_id, capacity, 0);
        }
        // add edge from left node to right nodes
        for (int i = 0; i < m_solver->l_nodes.size(); ++i) {
            int from_idx = i;
            // process link limits, i can only link to link_limits
            if (auto iter = uv_link_limits.find(i); iter != uv_link_limits.end()) {
                for (auto r_id : iter->second)
                    m_solver->add_edge(from_idx, m_solver->l_nodes.size() + r_id, 1, m_solver->get_distance(i, r_id));
                continue;
            }

            // process unlink limits, check whether i can link to j
            std::optional<std::vector<int>> unlink_limits;
            if (auto iter = uv_unlink_limits.find(i); iter != uv_unlink_limits.end())
                unlink_limits = iter->second;
            for (int j = 0; j < m_solver->r_nodes.size(); ++j) {
                if (unlink_limits.has_value() && std::find(unlink_limits->begin(), unlink_limits->end(), j) != unlink_limits->end())
                    continue;
                m_solver->add_edge(from_idx, m_solver->l_nodes.size() + j, 1, m_solver->get_distance(i, j));
            }
        }
    }

    std::vector<int> MinFlushFlowSolver::solve() {
        return m_solver->solve();
    }

    MatchModeGroupSolver::~MatchModeGroupSolver()
    {
    }

    MatchModeGroupSolver::MatchModeGroupSolver(const std::vector<std::vector<float>>& matrix_, const std::vector<int>& u_nodes, const std::vector<int>& v_nodes, const std::vector<int>& v_capacity, const std::unordered_map<int, std::vector<int>>& uv_unlink_limits)
    {
        assert(v_nodes.size() == v_capacity.size());
        m_solver = std::make_unique<MinCostMaxFlow>();
        m_solver->matrix = matrix_;;
        m_solver->l_nodes = u_nodes;
        m_solver->r_nodes = v_nodes;

        m_solver->total_nodes = u_nodes.size() + v_nodes.size() + 2;

        m_solver->source_id = m_solver->total_nodes - 2;
        m_solver->sink_id = m_solver->total_nodes - 1;

        m_solver->adj.resize(m_solver->total_nodes);


        // add edge from source to left nodes,cost to 0
        for (int i = 0; i < m_solver->l_nodes.size(); ++i)
            m_solver->add_edge(m_solver->source_id, i, 1, 0);

        // add edge from right nodes to sink,cost to 0
        for (int i = 0; i < m_solver->r_nodes.size(); ++i)
            m_solver->add_edge(m_solver->l_nodes.size() + i, m_solver->sink_id, v_capacity[i], 0);

        // add edge from left node to right nodes
        for (int i = 0; i < m_solver->l_nodes.size(); ++i) {
            int from_idx = i;

            // process unlink limits, check whether i can link to j
            std::optional<std::vector<int>> unlink_limits;
            if (auto iter = uv_unlink_limits.find(i); iter != uv_unlink_limits.end())
                unlink_limits = iter->second;
            for (int j = 0; j < m_solver->r_nodes.size(); ++j) {
                if (unlink_limits.has_value() && std::find(unlink_limits->begin(), unlink_limits->end(), j) != unlink_limits->end())
                    continue;
                m_solver->add_edge(from_idx, m_solver->l_nodes.size() + j, 1, m_solver->get_distance(i, j));
            }
        }
    }

    std::vector<int> MatchModeGroupSolver::solve() {
        return m_solver->solve();
    }

    //solve the problem by searching the least flush of current filament
    static std::vector<unsigned int> solve_extruder_order_with_greedy(const std::vector<std::vector<float>>& wipe_volumes,
        const std::vector<unsigned int> curr_layer_extruders,
        const std::optional<unsigned int>& start_extruder_id,
        float* min_cost)
    {
        float cost = 0;
        std::vector<unsigned int> best_seq;
        std::vector<bool>is_visited(curr_layer_extruders.size(), false);
        std::optional<unsigned int>prev_filament = start_extruder_id;
        int idx = curr_layer_extruders.size();
        while (idx > 0) {
            if (!prev_filament) {
                auto iter = std::find_if(is_visited.begin(), is_visited.end(), [](auto item) {return item == 0; });
                assert(iter != is_visited.end());
                prev_filament = curr_layer_extruders[iter - is_visited.begin()];
            }
            int target_idx = -1;
            int target_cost = std::numeric_limits<int>::max();
            for (size_t k = 0; k < is_visited.size(); ++k) {
                if (!is_visited[k]) {
                    if (wipe_volumes[*prev_filament][curr_layer_extruders[k]] < target_cost ||
                        (wipe_volumes[*prev_filament][curr_layer_extruders[k]] == target_cost && prev_filament == curr_layer_extruders[k])) {
                        target_idx = k;
                        target_cost = wipe_volumes[*prev_filament][curr_layer_extruders[k]];
                    }
                }
            }
            assert(target_idx != -1);
            cost += target_cost;
            best_seq.emplace_back(curr_layer_extruders[target_idx]);
            prev_filament = curr_layer_extruders[target_idx];
            is_visited[target_idx] = true;
            idx -= 1;
        }
        if (min_cost)
            *min_cost = cost;
        return best_seq;
    }

    //solve the problem by forcasting one layer
    static std::vector<unsigned int> solve_extruder_order_with_forcast(const std::vector<std::vector<float>>& wipe_volumes,
        std::vector<unsigned int> curr_layer_extruders,
        std::vector<unsigned int> next_layer_extruders,
        const std::optional<unsigned int>& start_extruder_id,
        float* min_cost)
    {
        std::sort(curr_layer_extruders.begin(), curr_layer_extruders.end());
        std::sort(next_layer_extruders.begin(), next_layer_extruders.end());
        float best_cost = std::numeric_limits<float>::max();
        int best_change = std::numeric_limits<int>::max(); // add filament change check in case flush volume between different filament is 0
        std::vector<unsigned int>best_seq;

        auto get_filament_change_count = [](const std::vector<unsigned int>& curr_seq, const std::vector<unsigned int>& next_seq,const std::optional<unsigned int>& start_extruder_id) {
            int count = 0;
            auto prev_extruder_id = start_extruder_id;
            for (auto seq : { curr_seq,next_seq }) {
                for (auto eid : seq) {
                    if (prev_extruder_id && prev_extruder_id != eid) {
                        count += 1;
                    }
                    prev_extruder_id = eid;
                }
            }
            return count;

            };

        do {
            std::optional<unsigned int>prev_extruder_1 = start_extruder_id;
            float curr_layer_cost = 0;
            for (size_t idx = 0; idx < curr_layer_extruders.size(); ++idx) {
                if (prev_extruder_1)
                    curr_layer_cost += wipe_volumes[*prev_extruder_1][curr_layer_extruders[idx]];
                prev_extruder_1 = curr_layer_extruders[idx];
            }
            if (curr_layer_cost > best_cost)
                continue;
            do {
                std::optional<unsigned int>prev_extruder_2 = prev_extruder_1;
                float total_cost = curr_layer_cost;
                int total_change = get_filament_change_count(curr_layer_extruders, next_layer_extruders, start_extruder_id);

                for (size_t idx = 0; idx < next_layer_extruders.size(); ++idx) {
                    if (prev_extruder_2)
                        total_cost += wipe_volumes[*prev_extruder_2][next_layer_extruders[idx]];
                    prev_extruder_2 = next_layer_extruders[idx];
                }

                if (total_cost < best_cost || (total_cost == best_cost && total_change < best_change)) {
                    best_cost = total_cost;
                    best_seq = curr_layer_extruders;
                    best_change = total_change;
                }
            } while (std::next_permutation(next_layer_extruders.begin(), next_layer_extruders.end()));
        } while (std::next_permutation(curr_layer_extruders.begin(), curr_layer_extruders.end()));

        if (min_cost) {
            float real_cost = 0;
            std::optional<unsigned int>prev_extruder = start_extruder_id;
            for (size_t idx = 0; idx < best_seq.size(); ++idx) {
                if (prev_extruder)
                    real_cost += wipe_volumes[*prev_extruder][best_seq[idx]];
                prev_extruder = best_seq[idx];
            }
            *min_cost = real_cost;
        }
        return best_seq;
    }

    // Shortest hamilton path problem
    static std::vector<unsigned int> solve_extruder_order(const std::vector<std::vector<float>>& wipe_volumes,
        std::vector<unsigned int> all_extruders,
        std::optional<unsigned int> start_extruder_id,
        float* min_cost)
    {
        bool add_start_extruder_flag = false;

        if (start_extruder_id) {
            auto start_iter = std::find(all_extruders.begin(), all_extruders.end(), start_extruder_id);
            if (start_iter == all_extruders.end())
                all_extruders.insert(all_extruders.begin(), *start_extruder_id), add_start_extruder_flag = true;
            else
                std::swap(*all_extruders.begin(), *start_iter);
        }
        else {
            start_extruder_id = all_extruders.front();
        }

        unsigned int iterations = (1 << all_extruders.size());
        unsigned int final_state = iterations - 1;
        std::vector<std::vector<float>>cache(iterations, std::vector<float>(all_extruders.size(), 0x7fffffff));
        std::vector<std::vector<int>>prev(iterations, std::vector<int>(all_extruders.size(), -1));
        cache[1][0] = 0.;
        for (unsigned int state = 0; state < iterations; ++state) {
            if (state & 1) {
                for (unsigned int target = 0; target < all_extruders.size(); ++target) {
                    if (state >> target & 1) {
                        for (unsigned int mid_point = 0; mid_point < all_extruders.size(); ++mid_point) {
                            if (state >> mid_point & 1) {
                                auto tmp = cache[state - (1 << target)][mid_point] + wipe_volumes[all_extruders[mid_point]][all_extruders[target]];
                                if (cache[state][target] > tmp) {
                                    cache[state][target] = tmp;
                                    prev[state][target] = mid_point;
                                }
                            }
                        }
                    }
                }
            }
        }

        //get res
        float cost = std::numeric_limits<float>::max();
        int final_dst = 0;
        for (unsigned int dst = 0; dst < all_extruders.size(); ++dst) {
            if (all_extruders[dst] != start_extruder_id && cost > cache[final_state][dst]) {
                cost = cache[final_state][dst];
                if (min_cost)
                    *min_cost = cost;
                final_dst = dst;
            }
        }

        std::vector<unsigned int>path;
        unsigned int curr_state = final_state;
        int curr_point = final_dst;
        while (curr_point != -1) {
            path.emplace_back(all_extruders[curr_point]);
            auto mid_point = prev[curr_state][curr_point];
            curr_state -= (1 << curr_point);
            curr_point = mid_point;
        };

        if (add_start_extruder_flag)
            path.pop_back();

        std::reverse(path.begin(), path.end());
        return path;
    }



    template<class T>
    static std::vector<T> collect_filaments_in_groups(const std::unordered_set<unsigned int>& group, const std::vector<unsigned int>& filament_list) {
        std::vector<T>ret;
        ret.reserve(group.size());
        for (auto& f : filament_list) {
            if (auto iter = group.find(f); iter != group.end())
                ret.emplace_back(static_cast<T>(f));
        }
        return ret;
    }

    // get best filament order of single nozzle
    std::vector<unsigned int> get_extruders_order(const std::vector<std::vector<float>>& wipe_volumes,
        const std::vector<unsigned int>& curr_layer_extruders,
        const std::vector<unsigned int>& next_layer_extruders,
        const std::optional<unsigned int>& start_extruder_id,
        bool use_forcast,
        float* cost)
    {
        if (curr_layer_extruders.empty()) {
            if (cost)
                *cost = 0;
            return curr_layer_extruders;
        }
        if (curr_layer_extruders.size() == 1) {
            if (cost) {
                *cost = 0;
                if (start_extruder_id)
                    *cost = wipe_volumes[*start_extruder_id][curr_layer_extruders[0]];
            }
            return curr_layer_extruders;
        }

        if (use_forcast)
            return solve_extruder_order_with_forcast(wipe_volumes, curr_layer_extruders, next_layer_extruders, start_extruder_id, cost);
        else if (curr_layer_extruders.size() <= 20)
            return solve_extruder_order(wipe_volumes, curr_layer_extruders, start_extruder_id, cost);
        else
            return solve_extruder_order_with_greedy(wipe_volumes, curr_layer_extruders, start_extruder_id, cost);
    }


    // Single-nozzle flush-minimizing reorder over one filament set / one flush matrix, with an
    // optional seed filament. Extracted from the group loop so the multi-nozzle reorder can call it
    // per physical nozzle.
    // TODO: add custom sequence
    static int reorder_filaments_for_minimum_flush_volume_base(const std::vector<unsigned int>& filament_lists,
        const std::vector<std::vector<unsigned int>>& layer_filaments,
        const FlushMatrix& flush_matrix,
        const std::function<bool(int, std::vector<int>&)> get_custom_seq,
        std::vector<std::vector<unsigned int>>* filament_sequences,
        std::optional<unsigned int> initial_filament_id = std::nullopt)
    {
        constexpr int max_n_with_forcast = 5;
        using uint128_t = boost::multiprecision::uint128_t;

        if (filament_sequences) {
            filament_sequences->clear();
            filament_sequences->reserve(layer_filaments.size());
        }
        auto          filament_list_to_hash_key = [](const std::vector<unsigned int>& curr_layer_filaments, const std::vector<unsigned int>& next_layer_filaments,
            const std::optional<unsigned int>& prev_filament, bool use_forcast) -> uint128_t {
                uint128_t hash_key = 0;
                // 31-0 bit define current layer extruder,63-32 bit define next layer extruder,95~64 define prev extruder
                if (prev_filament) hash_key |= (uint128_t(1) << (64 + *prev_filament));

                if (use_forcast) {
                    for (auto item : next_layer_filaments) { hash_key |= (uint128_t(1) << (32 + item)); }
                }

                for (auto item : curr_layer_filaments) { hash_key |= (uint128_t(1) << item); }
                return hash_key;
            };

        int cost = 0;
        std::map<size_t, std::vector<unsigned int>> custom_layer_sequence_map;
        std::unordered_map<uint128_t, std::pair<float, std::vector<unsigned int>>> caches;
        std::unordered_set<unsigned int> filament_sets(filament_lists.begin(), filament_lists.end());
        std::optional<unsigned int>      curr_filament_id;
        // use the provided initial filament id as the starting state when it is valid
        if (initial_filament_id.has_value() && *initial_filament_id < flush_matrix.size()) {
            curr_filament_id = initial_filament_id;
        }

        for (size_t layer = 0; layer < layer_filaments.size(); ++layer){
            const auto& curr_lf = layer_filaments[layer];
            std::vector<int> custom_filament_seq;
            if (get_custom_seq && get_custom_seq(layer, custom_filament_seq) && !custom_filament_seq.empty()) {
                std::vector<unsigned int> unsign_custom_extruder_seq;
                for (int extruder : custom_filament_seq) {
                    unsigned int unsign_extruder = static_cast<unsigned int>(extruder) - 1;
                    auto it = std::find(layer_filaments[layer].begin(), layer_filaments[layer].end(), unsign_extruder);
                    if (it != layer_filaments[layer].end())
                        unsign_custom_extruder_seq.emplace_back(unsign_extruder);
                }
                assert(layer_filaments[layer].size() == unsign_custom_extruder_seq.size());

                custom_layer_sequence_map[layer] = unsign_custom_extruder_seq;
            }
        }

        for (size_t layer = 0; layer < layer_filaments.size(); ++layer) {
            const auto& curr_lf = layer_filaments[layer];

            if(auto iter = custom_layer_sequence_map.find(layer); iter != custom_layer_sequence_map.end()){
                auto sequence_in_group = collect_filaments_in_groups<unsigned int>(std::unordered_set<unsigned int>(filament_lists.begin(),filament_lists.end()), iter->second);

                std::optional<unsigned int> prev = curr_filament_id;
                for (auto& f: sequence_in_group){
                    if(prev)
                        cost += flush_matrix[*prev][f];
                    prev = f;
                }

                if(!sequence_in_group.empty()){
                    curr_filament_id = sequence_in_group.back();
                }

                if(filament_sequences)
                    filament_sequences->emplace_back(sequence_in_group);

                continue;
            }

            std::vector<unsigned int> filament_used = collect_filaments_in_groups<unsigned int>(filament_sets, curr_lf);
            std::vector<unsigned int> next_lf;
            if (layer + 1 < layer_filaments.size()) next_lf = layer_filaments[layer + 1];
            std::vector<unsigned int> filament_used_next_layer = collect_filaments_in_groups<unsigned int>(filament_sets, next_lf);

            // Enable inter-layer forecast: when choosing filament ordering for current layer,
            // also consider next layer's filament set to minimize inter-layer transition flush.
            // solve_extruder_order_with_forcast() tries all permutations of curr+next layer
            // and picks the ordering that minimizes total flush across both layers.
            // This avoids expensive inter-layer transitions (e.g. ending layer with F2 when
            // next layer starts with F3, costing flush[F2→F3], instead of ending with F3
            // which gives flush[F3→F3]=0). Limited to ≤5 filaments due to O(N!×M!) complexity.
            // The per-nozzle base reorder does not use the inter-layer forecast. This function drives
            // BBL multi-extruder grouping cost and H2C ordering, so keeping it false avoids perturbing
            // existing H2D/H2C output.
            bool                      use_forcast = false;
            float                     tmp_cost = 0;
            std::vector<unsigned int> sequence;
            uint128_t                 hash_key = filament_list_to_hash_key(filament_used, filament_used_next_layer, curr_filament_id, use_forcast);
            if (auto iter = caches.find(hash_key); iter != caches.end()) {
                tmp_cost = iter->second.first;
                sequence = iter->second.second;
            }
            else {
                sequence = get_extruders_order(flush_matrix, filament_used, filament_used_next_layer, curr_filament_id, use_forcast, &tmp_cost);
                caches[hash_key] = { tmp_cost,sequence };
            }

            if (filament_sequences)
                filament_sequences->emplace_back(sequence);

            if (!sequence.empty())
                curr_filament_id = sequence.back();

            cost += tmp_cost;
        }

        return cost;
    }

    int reorder_filaments_for_minimum_flush_volume(const std::vector<unsigned int>& filament_lists,
        const std::vector<int>& filament_maps,
        const std::vector<std::vector<unsigned int>>& layer_filaments,
        const std::vector<FlushMatrix>& flush_matrix,
        std::optional<std::function<bool(int, std::vector<int>&)>> get_custom_seq,
        std::vector<std::vector<unsigned int>>* filament_sequences,
        const std::unordered_map<int, int>& nozzle_status)
    {
        //only when layer filament num <= 5,we do forcast
        constexpr int max_n_with_forcast = 5;
        int cost = 0;
        std::vector<std::unordered_set<unsigned int>>groups(2); //save the grouped filaments
        std::vector<std::vector<std::vector<unsigned int>>> layer_sequences(2); //save the reordered filament sequence by group
        std::map<size_t, std::vector<unsigned int>> custom_layer_sequence_map; // save the filament sequences of custom layer

        // group the filament
        for (int i = 0; i < filament_maps.size(); ++i) {
            if (filament_maps[i] == 0)
                groups[0].insert(filament_lists[i]);
            if (filament_maps[i] == 1)
                groups[1].insert(filament_lists[i]);
        }

        // store custom layer sequence
        for (size_t layer = 0; layer < layer_filaments.size(); ++layer) {
            const auto& curr_lf = layer_filaments[layer];

            std::vector<int>custom_filament_seq;
            if (get_custom_seq && (*get_custom_seq)(layer, custom_filament_seq) && !custom_filament_seq.empty()) {
                std::vector<unsigned int> unsign_custom_extruder_seq;
                for (int extruder : custom_filament_seq) {
                    unsigned int unsign_extruder = static_cast<unsigned int>(extruder) - 1;
                    auto it = std::find(curr_lf.begin(), curr_lf.end(), unsign_extruder);
                    if (it != curr_lf.end())
                        unsign_custom_extruder_seq.emplace_back(unsign_extruder);
                }
                assert(curr_lf.size() == unsign_custom_extruder_seq.size());

                custom_layer_sequence_map[layer] = unsign_custom_extruder_seq;
            }
        }
        using uint128_t = boost::multiprecision::uint128_t;
        auto extruders_to_hash_key = [](const std::vector<unsigned int>& curr_layer_extruders,
           const std::vector<unsigned int>& next_layer_extruders,
           const std::optional<unsigned int>& prev_extruder,
           bool use_forcast)->uint128_t
           {
               uint128_t hash_key = 0;
               //31-0 bit define current layer extruder,63-32 bit define next layer extruder,95~64 define prev extruder
               if (prev_extruder)
                   hash_key |= (uint128_t(1) << (64 + *prev_extruder));

               if (use_forcast) {
                   for (auto item : next_layer_extruders)
                       hash_key |= (uint128_t(1) << (32 + item));
               }

               for (auto item : curr_layer_extruders)
                   hash_key |= (uint128_t(1) << item);
               return hash_key;
           };


        // get best layer sequence by group
        for (size_t idx = 0; idx < groups.size(); ++idx) {
            // case with one group
            if (groups[idx].empty())
                continue;
            std::optional<unsigned int>current_extruder_id;
            // seed the group (nozzle) with the filament already loaded, if nozzle_status supplies one
            if (auto it = nozzle_status.find(static_cast<int>(idx)); it != nozzle_status.end() && it->second >= 0) {
                unsigned int initial_fil = static_cast<unsigned int>(it->second);
                if (initial_fil < flush_matrix[idx].size())
                    current_extruder_id = initial_fil;
            }

            std::unordered_map<uint128_t, std::pair<float, std::vector<unsigned int>>> caches;

            for (size_t layer = 0; layer < layer_filaments.size(); ++layer) {
                const auto& curr_lf = layer_filaments[layer];

                if (auto iter = custom_layer_sequence_map.find(layer); iter != custom_layer_sequence_map.end()) {
                    auto sequence_in_group = collect_filaments_in_groups<unsigned int>(groups[idx], iter->second);

                    float tmp_cost = 0;
                    std::optional<unsigned int>prev = current_extruder_id;
                    for (auto& f : sequence_in_group) {
                        if (prev) { tmp_cost += flush_matrix[idx][*prev][f]; }
                        prev = f;
                    }
                    cost += tmp_cost;

                    if (!sequence_in_group.empty())
                        current_extruder_id = sequence_in_group.back();
                    //insert an empty array
                    if (filament_sequences)
                        layer_sequences[idx].emplace_back(std::vector<unsigned int>());

                    continue;
                }

                std::vector<unsigned int>filament_used_in_group = collect_filaments_in_groups<unsigned int>(groups[idx], curr_lf);

                std::vector<unsigned int>next_lf;
                if (layer + 1 < layer_filaments.size())
                    next_lf = layer_filaments[layer + 1];
                std::vector<unsigned int>filament_used_in_group_next_layer = collect_filaments_in_groups<unsigned int>(groups[idx], next_lf);

                bool use_forcast = (filament_used_in_group.size() <= max_n_with_forcast && filament_used_in_group_next_layer.size() <= max_n_with_forcast);
                float tmp_cost = 0;
                std::vector<unsigned int>sequence;
                uint128_t hash_key = extruders_to_hash_key(filament_used_in_group, filament_used_in_group_next_layer, current_extruder_id, use_forcast);
                if (auto iter = caches.find(hash_key); iter != caches.end()) {
                    tmp_cost = iter->second.first;
                    sequence = iter->second.second;
                }
                else {
                    sequence = get_extruders_order(flush_matrix[idx], filament_used_in_group, filament_used_in_group_next_layer, current_extruder_id, use_forcast, &tmp_cost);
                    caches[hash_key] = { tmp_cost,sequence };
                }

                assert(sequence.size() == filament_used_in_group.size());

                if (filament_sequences)
                    layer_sequences[idx].emplace_back(sequence);

                if (!sequence.empty())
                    current_extruder_id = sequence.back();
                cost += tmp_cost;
            }
        }

        // get the final layer sequences
        // if only have one group,we need to check whether layer sequence[idx] is valid
        if (filament_sequences) {
            filament_sequences->clear();
            filament_sequences->resize(layer_filaments.size());
            int last_group_id = 0;
            //if last_group == 0,print group 0 first ,else print group 1 first
            if (!custom_layer_sequence_map.empty()) {
                const auto& first_layer = custom_layer_sequence_map.begin()->first;
                const auto& first_layer_filaments = custom_layer_sequence_map.begin()->second;
                assert(!first_layer_filaments.empty());

                bool first_group = groups[0].count(first_layer_filaments.front()) ? 0 : 1;
                last_group_id = (first_layer & 1) ? !first_group : first_group;
            }

            for (size_t layer = 0; layer < layer_filaments.size(); ++layer) {
                auto& curr_layer_seq = (*filament_sequences)[layer];
                if (custom_layer_sequence_map.find(layer) != custom_layer_sequence_map.end()) {
                    curr_layer_seq = custom_layer_sequence_map[layer];
                    if (!curr_layer_seq.empty()) {
                        last_group_id = groups[0].count(curr_layer_seq.back()) ? 0 : 1;
                    }
                    continue;
                }
                if (last_group_id == 1) {
                    // try reuse the last group
                    if (!layer_sequences[1].empty() && !layer_sequences[1][layer].empty())
                        curr_layer_seq.insert(curr_layer_seq.end(), layer_sequences[1][layer].begin(), layer_sequences[1][layer].end());
                    if (!layer_sequences[0].empty() && !layer_sequences[0][layer].empty()) {
                        curr_layer_seq.insert(curr_layer_seq.end(), layer_sequences[0][layer].begin(), layer_sequences[0][layer].end());
                        last_group_id = 0; // update last group id
                    }
                }
                else if(last_group_id == 0) {
                    if (!layer_sequences[0].empty() && !layer_sequences[0][layer].empty()) {
                        curr_layer_seq.insert(curr_layer_seq.end(), layer_sequences[0][layer].begin(), layer_sequences[0][layer].end());
                    }
                    if (!layer_sequences[1].empty() && !layer_sequences[1][layer].empty()) {
                        curr_layer_seq.insert(curr_layer_seq.end(), layer_sequences[1][layer].begin(), layer_sequences[1][layer].end());
                        last_group_id = 1; // update last group id
                    }
                }
            }
        }

        return cost;
    }

    int reorder_filaments_for_multi_nozzle_extruder(const std::vector<unsigned int>& filament_lists,
        const MultiNozzleUtils::LayeredNozzleGroupResult& nozzle_group_result,
        const std::vector<std::vector<unsigned int>>& layer_filaments,
        const std::vector<FlushMatrix>& flush_matrix,
        const std::function<bool(int, std::vector<int>&)> get_custom_seq,
        std::vector<std::vector<unsigned int>>* filament_sequences,
        const MultiNozzleUtils::NozzleStatusRecorder& initial_status)
    {
        std::map<int,std::set<unsigned int>> nozzle_filament_groups;
        std::map<int,std::set<int>> extruder_to_nozzle;

        for(auto filament_idx : filament_lists){
            auto nozzle_info = nozzle_group_result.get_nozzle_for_filament(filament_idx, -1);
            if (!nozzle_info)
                continue;
            nozzle_filament_groups[nozzle_info->group_id].insert(filament_idx);
            extruder_to_nozzle[nozzle_info->extruder_id].insert(nozzle_info->group_id);
        }

        std::map<size_t, std::vector<unsigned int>>custom_layer_sequence_map;// save the filament sequences of custom layer
        for (size_t layer = 0; layer < layer_filaments.size(); ++layer){
            const auto& curr_lf = layer_filaments[layer];
            std::vector<int> custom_filament_seq;
            if (get_custom_seq && get_custom_seq(layer, custom_filament_seq) && !custom_filament_seq.empty()) {
                std::vector<unsigned int> unsign_custom_extruder_seq;
                for (int extruder : custom_filament_seq) {
                    unsigned int unsign_extruder = static_cast<unsigned int>(extruder) - 1;
                    auto it = std::find(layer_filaments[layer].begin(), layer_filaments[layer].end(), unsign_extruder);
                    if (it != layer_filaments[layer].end())
                        unsign_custom_extruder_seq.emplace_back(unsign_extruder);
                }
                assert(layer_filaments[layer].size() == unsign_custom_extruder_seq.size());

                custom_layer_sequence_map[layer] = unsign_custom_extruder_seq;
            }
        }


        std::map<int, std::vector<std::vector<unsigned int>>> nozzle_filament_sequences;
        bool store_sequence = filament_sequences != nullptr;

        int cost = 0;
        for(auto& group : nozzle_filament_groups){
            int nozzle_id = group.first;
            auto& filament_in_nozzle = group.second;

            int extruder_id = 0;
            for(auto& [ext, nozzle_set] : extruder_to_nozzle){
                if(nozzle_set.count(nozzle_id)){
                    extruder_id = ext;
                    break;
                }
            }

            if(filament_in_nozzle.empty())
                continue;

            std::vector<unsigned int> filament_vec_in_nozzle(filament_in_nozzle.begin(), filament_in_nozzle.end());

            int initial_fil = initial_status.get_filament_in_nozzle(nozzle_id);
            std::optional<unsigned int> initial_fil_id = (initial_fil >= 0 && initial_fil < flush_matrix[extruder_id].size())? std::optional<unsigned int>(initial_fil) : std::nullopt;

            std::vector<std::vector<unsigned int>> filament_seq;
            cost += reorder_filaments_for_minimum_flush_volume_base(filament_vec_in_nozzle, layer_filaments, flush_matrix[extruder_id], get_custom_seq,
                                                                    store_sequence ? &filament_seq : nullptr, initial_fil_id);
            if(store_sequence)
                nozzle_filament_sequences.emplace(nozzle_id, std::move(filament_seq));

        }

        if(!store_sequence)
            return cost;

        std::vector<int> extruders;
        std::map<int, std::vector<int>> nozzles_per_extruder;
        for (auto& [extruder_id, nozzle_set] : extruder_to_nozzle) {
            extruders.push_back(extruder_id);
            nozzles_per_extruder[extruder_id] = std::vector<int>(
                nozzle_set.begin(), nozzle_set.end()
            );
        }

        filament_sequences->clear();
        filament_sequences->resize(layer_filaments.size());

        // No filament in filament_lists resolved to a nozzle in nozzle_group_result
        // (e.g. a degenerate input where a layer references a filament index outside the range's
        // grouping map). Emit each layer's filaments in their given order so the caller still gets a
        // valid per-layer sequence, and skip the cross-nozzle reorder. Guards the unchecked
        // max_element(extruders) below, which would dereference end() on an empty range.
        if (extruders.empty()) {
            for (size_t layer = 0; layer < layer_filaments.size(); ++layer)
                (*filament_sequences)[layer] = layer_filaments[layer];
            return cost;
        }

        auto get_extruder_for_filament = [nozzle_group_result](unsigned int filament_idx) {
            auto nozzle = nozzle_group_result.get_nozzle_for_filament(filament_idx, -1);
            if (!nozzle)
                return -1;
            return nozzle->extruder_id;
            };

        auto get_nozzle_idx_for_filament = [nozzles_per_extruder, nozzle_group_result](unsigned int filament_idx)->int {
            auto nozzle = nozzle_group_result.get_nozzle_for_filament(filament_idx, -1);
            if (!nozzle)
                return -1;
            return std::find(nozzles_per_extruder.at(nozzle->extruder_id).begin(), nozzles_per_extruder.at(nozzle->extruder_id).end(), nozzle->group_id) - nozzles_per_extruder.at(nozzle->extruder_id).begin();
            };

        int initial_extruder = initial_status.get_current_extruder_id();
        int last_extruder_idx = (initial_extruder >= 0 && initial_extruder < extruders.size())? initial_extruder : 0;
        // set size to max extruder_id in case extruder_id is not continuous
        std::vector<int> last_nozzle_idx(*std::max_element(extruders.begin(),extruders.end()) + 1,0);
        for (int ext_id = 0; ext_id < static_cast<int>(last_nozzle_idx.size()); ext_id++) {
            int initial_nozzle = initial_status.get_nozzle_in_extruder(ext_id);
            auto ext_nozzles = nozzles_per_extruder[ext_id];
            auto it = std::find(ext_nozzles.begin(), ext_nozzles.end(), initial_nozzle);
            if (it != ext_nozzles.end())
                last_nozzle_idx[ext_id] = static_cast<int>(std::distance(ext_nozzles.begin(), it));
        }

        for (size_t layer = 0; layer < layer_filaments.size(); ++layer) {
            auto& out_seq = (*filament_sequences)[layer];

            if (custom_layer_sequence_map.find(layer) != custom_layer_sequence_map.end()) {
                out_seq = custom_layer_sequence_map[layer];
                if (!out_seq.empty()) {
                    last_extruder_idx = get_extruder_for_filament(out_seq.back());
                    for (auto filament : out_seq) {
                        int cur_ext_id = get_extruder_for_filament(filament);
                        last_nozzle_idx[cur_ext_id] = get_nozzle_idx_for_filament(filament);
                    }
                }
                continue;
            }

            if (last_extruder_idx == -1)
                last_extruder_idx = 0;

            int curr_last_extruder_idx = last_extruder_idx;
            auto curr_last_nozzle_idx = last_nozzle_idx;
            for (int i = 0; i < extruders.size(); ++i) {
                int extruder_id = extruders[(last_extruder_idx + i) % extruders.size()];
                auto& base_nozzles = nozzles_per_extruder[extruder_id];

                bool has_seq = false;
                if (last_nozzle_idx[extruder_id] == -1)
                    last_nozzle_idx[extruder_id] = 0;

                for (int j = 0; j < base_nozzles.size(); ++j) {
                    int nozzle_idx = (last_nozzle_idx[extruder_id] + j) % base_nozzles.size();
                    int nozzle_id = base_nozzles[nozzle_idx];
                    const auto& frag = nozzle_filament_sequences[nozzle_id][layer];
                    if (frag.empty())
                        continue;
                    has_seq = true;
                    curr_last_nozzle_idx[extruder_id] = nozzle_idx;
                    out_seq.insert(out_seq.end(), frag.begin(), frag.end());
                }

                if (has_seq)
                    curr_last_extruder_idx = extruder_id;
            }
            last_extruder_idx = curr_last_extruder_idx;
            last_nozzle_idx = curr_last_nozzle_idx;
        }
        return cost;
    }
}
