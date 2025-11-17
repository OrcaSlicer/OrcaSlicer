#include "ToolOrderUtils.hpp"
#include <queue>
#include <set>
#include <map>
#include <cmath>
#include <boost/multiprecision/cpp_int.hpp>

namespace Slic3r
{
    struct MinCostMaxFlow {
    public:
        struct Edge {
            int from, to, capacity, cost, flow;
            Edge(int u, int v, int cap, int cst) : from(u), to(v), capacity(cap), cost(cst), flow(0) {}
        };

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
            //TODO: test more here
            int sum = 0;
            for (int i = 0; i < matrix.size(); ++i)
                sum += matrix[i][idx_in_right];
            sum /= matrix.size();
            return -sum;
        }

        return matrix[l_nodes[idx_in_left]][r_nodes[idx_in_right]];
    }


    MaxFlowSolver::MaxFlowSolver(const std::vector<int>& u_nodes, const std::vector<int>& v_nodes,
        const std::unordered_map<int, std::vector<int>>& uv_link_limits,
        const std::unordered_map<int, std::vector<int>>& uv_unlink_limits,
        const std::vector<int>& u_capacity,
        const std::vector<int>& v_capacity)
    {
        assert(u_capacity.empty() || u_capacity.size() == u_nodes.size());
        assert(v_capacity.empty() || v_capacity.size() == v_nodes.size());
        l_nodes = u_nodes;
        r_nodes = v_nodes;
        total_nodes = u_nodes.size() + v_nodes.size() + 2;
        source_id = total_nodes - 2;
        sink_id = total_nodes - 1;

        adj.resize(total_nodes);

        // add edge from source to left nodes
        for (int idx = 0; idx < l_nodes.size(); ++idx) {
            int capacity = u_capacity.empty() ? 1 : u_capacity[idx];
            add_edge(source_id, idx, capacity);
        }
        // add edge from right nodes to sink node
        for (int idx = 0; idx < r_nodes.size(); ++idx) {
            int capacity = v_capacity.empty() ? 1 : v_capacity[idx];
            add_edge(l_nodes.size() + idx, sink_id, capacity);
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

    MinFlushFlowSolver::~MinFlushFlowSolver()
    {
    }

    MinFlushFlowSolver::MinFlushFlowSolver(const std::vector<std::vector<float>>& matrix_, const std::vector<int>& u_nodes, const std::vector<int>& v_nodes,
        const std::unordered_map<int, std::vector<int>>& uv_link_limits,
        const std::unordered_map<int, std::vector<int>>& uv_unlink_limits,
        const std::vector<int>& u_capacity,
        const std::vector<int>& v_capacity)
    {
        assert(u_capacity.empty() || u_capacity.size() == u_nodes.size());
        assert(v_capacity.empty() || v_capacity.size() == v_nodes.size());
        m_solver = std::make_unique<MinCostMaxFlow>();
        m_solver->matrix = matrix_;;
        m_solver->l_nodes = u_nodes;
        m_solver->r_nodes = v_nodes;

        m_solver->total_nodes = u_nodes.size() + v_nodes.size() + 2;

        m_solver->source_id =m_solver->total_nodes - 2;
        m_solver->sink_id = m_solver->total_nodes - 1;

        m_solver->adj.resize(m_solver->total_nodes);

        // add edge from source to left nodes,cost to 0
        for (int i = 0; i < m_solver->l_nodes.size(); ++i) {
            int capacity = u_capacity.empty() ? 1 : u_capacity[i];
            m_solver->add_edge(m_solver->source_id, i, capacity, 0);
        }
        // add edge from right nodes to sink,cost to 0
        for (int i = 0; i < m_solver->r_nodes.size(); ++i) {
            int capacity = v_capacity.empty() ? 1 : v_capacity[i];
            m_solver->add_edge(m_solver->l_nodes.size() + i, m_solver->sink_id, capacity, 0);
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


    int reorder_filaments_for_minimum_flush_volume(const std::vector<unsigned int>& filament_lists,
        const std::vector<int>& filament_maps,
        const std::vector<std::vector<unsigned int>>& layer_filaments,
        const std::vector<FlushMatrix>& flush_matrix,
        std::optional<std::function<bool(int, std::vector<int>&)>> get_custom_seq,
        std::vector<std::vector<unsigned int>>* filament_sequences)
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
}
