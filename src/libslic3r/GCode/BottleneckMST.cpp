// Bottleneck MST ordering: minimize the maximum edge length in the path.

#include "BottleneckMST.hpp"
#include "TSPPostProcessing.hpp"
#include "../Print.hpp"
#include "../Geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace Slic3r {

// Core algorithm: bottleneck MST ordering on a raw point set.
std::vector<size_t> bottleneck_mst_core(const Points& centers)
{
    if (centers.empty()) return {};

    size_t n = centers.size();
    if (n == 1) return {0};

    // Precompute all pairwise squared distances and sort unique values for binary search.
    std::vector<double> sorted_dists;
    sorted_dists.reserve(n * n);
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            double d2 = (centers[i].cast<double>() - centers[j].cast<double>()).squaredNorm();
            sorted_dists.push_back(d2);
        }
    }
    std::sort(sorted_dists.begin(), sorted_dists.end());

    // Union-Find for connectivity checking.
    struct UF {
        std::vector<int> parent, rank_;
        int components;
        UF(size_t sz) : parent(sz), rank_(sz, 0), components(static_cast<int>(sz)) {
            for (size_t i = 0; i < sz; ++i) parent[i] = static_cast<int>(i);
        }
        int find(int x) {
            while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
            return x;
        }
        void unite(int a, int b) {
            a = find(a); b = find(b);
            if (a == b) return;
            if (rank_[a] < rank_[b]) std::swap(a, b);
            parent[b] = a;
            if (rank_[a] == rank_[b]) ++rank_[a];
            --components;
        }
    };

    // Binary search: find minimum threshold T² where graph is connected.
    int lo = 0, hi = static_cast<int>(sorted_dists.size()) - 1;
    double best_threshold2 = sorted_dists[hi];

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        double threshold2 = sorted_dists[mid];

        UF uf(n);
        for (size_t i = 0; i < n && uf.components > 1; ++i) {
            for (size_t j = i + 1; j < n && uf.components > 1; ++j) {
                double d2 = (centers[i].cast<double>() - centers[j].cast<double>()).squaredNorm();
                if (d2 <= threshold2) uf.unite(static_cast<int>(i), static_cast<int>(j));
            }
        }

        if (uf.components == 1) {
            best_threshold2 = threshold2;
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }

    // Build adjacency list for the thresholded subgraph.
    struct Neighbor { size_t idx; double dist; };
    std::vector<std::vector<Neighbor>> adj(n);
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            double d2 = (centers[i].cast<double>() - centers[j].cast<double>()).squaredNorm();
            if (d2 <= best_threshold2) {
                double d = std::sqrt(d2);
                adj[i].push_back({j, d});
                adj[j].push_back({i, d});
            }
        }
    }

    // Nearest-neighbor traversal within the thresholded subgraph.
    // This produces a better initial path than DFS for max-edge minimization
    // because it greedily picks the shortest available edge at each step.
    std::vector<size_t> path;
    path.reserve(n);
    std::vector<bool> visited_nn(n, false);

    size_t curr = 0;
    visited_nn[0] = true;
    path.push_back(0);

    while (path.size() < n) {
        double best_d2 = std::numeric_limits<double>::max();
        size_t best_next = SIZE_MAX;

        // Find nearest unvisited neighbor within threshold.
        for (const auto& nb : adj[curr]) {
            if (!visited_nn[nb.idx] && nb.dist * nb.dist < best_d2) {
                best_d2 = nb.dist * nb.dist;
                best_next = nb.idx;
            }
        }

        if (best_next == SIZE_MAX) {
            // No neighbor within threshold; pick any unvisited node.
            for (size_t j = 0; j < n; ++j) {
                if (!visited_nn[j]) { best_next = j; break; }
            }
        }

        visited_nn[best_next] = true;
        path.push_back(best_next);
        curr = best_next;
    }

    // Post-processing: max-edge-minimizing 2-opt.
    // Unlike standard 2-opt which minimizes total path length, this variant
    // specifically targets reducing the longest single edge. It accepts swaps
    // that reduce the current maximum edge even if total length increases slightly.
    {
        size_t pn = path.size();
        
        // Find current max edge.
        auto compute_max_edge2 = [&]() -> double {
            double mx = 0;
            for (size_t i = 0; i < pn; ++i) {
                size_t j = (i + 1) % pn;
                double d2 = (centers[path[i]].cast<double>() - centers[path[j]].cast<double>()).squaredNorm();
                if (d2 > mx) mx = d2;
            }
            return mx;
        };

        double current_max2 = compute_max_edge2();
        bool improved = true;
        int max_passes = 10;
        
        while (improved && max_passes-- > 0) {
            improved = false;
            for (size_t i = 0; i < pn; ++i) {
                size_t i_next = (i + 1) % pn;
                double edge_i_sq = (centers[path[i]].cast<double>() - centers[path[i_next]].cast<double>()).squaredNorm();
                
                for (size_t j = i + 2; j < pn; ++j) {
                    if (j == i_next) continue;
                    if (j == (pn - 1) && i == 0) continue;
                    size_t j_next = (j + 1) % pn;
                    double edge_j_sq = (centers[path[j]].cast<double>() - centers[path[j_next]].cast<double>()).squaredNorm();

                    // Current max of the two removed edges.
                    double removed_max2 = std::max(edge_i_sq, edge_j_sq);
                    
                    // New edges after swap.
                    double new_edge1_sq = ((centers[path[j]].cast<double>() - centers[path[i]].cast<double>())).squaredNorm();
                    double new_edge2_sq = ((centers[path[j_next]].cast<double>() - centers[path[i_next]].cast<double>())).squaredNorm();
                    double added_max2 = std::max(new_edge1_sq, new_edge2_sq);

                    // Accept if the swap reduces the maximum edge.
                    // Only useful if one of the removed edges was the current max.
                    if (removed_max2 >= current_max2 && added_max2 < current_max2) {
                        while (i_next < j) { std::swap(path[i_next], path[j]); ++i_next; --j; }
                        current_max2 = compute_max_edge2();
                        improved = true;
                        break;
                    }
                }
                if (improved) break;
            }
        }
    }

    // Standard crossing removal and rotation.
    tsp_remove_crossings(path, centers);
    tsp_rotate_minimize_closing(path, centers);

    return path;
}

// Production wrapper.
std::vector<const PrintInstance*> chain_print_object_instances_bottleneck_mst(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
{
    return chain_instances_with_core(print_objects, start_near, bottleneck_mst_core);
}

std::vector<const PrintInstance*> chain_print_object_instances_bottleneck_mst(const Print& print)
{
    return chain_print_object_instances_bottleneck_mst(print.objects().vector(), nullptr);
}

} // namespace Slic3r
