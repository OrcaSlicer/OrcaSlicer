// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "extract_non_manifold_edge_curves.h"
#include <algorithm>
#include <cassert>
#include <list>
#include <vector>
#include <unordered_map>

template<
typename DerivedF,
typename DerivedEMAP,
typename uE2EType >
IGL_INLINE void igl::extract_non_manifold_edge_curves(
        const Eigen::PlainObjectBase<DerivedF>& F,
        const Eigen::PlainObjectBase<DerivedEMAP>& /*EMAP*/,
        const std::vector<std::vector<uE2EType> >& uE2E,
        std::vector<std::vector<size_t> >& curves) {
    const size_t num_faces = F.rows();
    assert(F.cols() == 3);
    //typedef std::pair<size_t, size_t> Edge;
    auto edge_index_to_face_index = [&](size_t ei) { return ei % num_faces; };
    auto edge_index_to_corner_index = [&](size_t ei) { return ei / num_faces; };
    auto get_edge_end_points = [&](size_t ei, size_t& s, size_t& d) {
        const size_t fi = edge_index_to_face_index(ei);
        const size_t ci = edge_index_to_corner_index(ei);
        s = F(fi, (ci+1)%3);
        d = F(fi, (ci+2)%3);
    };

    curves.clear();
    const size_t num_unique_edges = uE2E.size();
    std::unordered_multimap<size_t, size_t> vertex_edge_adjacency;
    std::vector<size_t> non_manifold_edges;
    for (size_t i=0; i<num_unique_edges; i++) {
        const auto& adj_edges = uE2E[i];
        if (adj_edges.size() == 2) continue;

        const size_t ei = adj_edges[0];
        size_t s,d;
        get_edge_end_points(ei, s, d);
        vertex_edge_adjacency.insert({{s, i}, {d, i}});
        non_manifold_edges.push_back(i);
        assert(vertex_edge_adjacency.count(s) > 0);
        assert(vertex_edge_adjacency.count(d) > 0);
    }

    auto expand_forward = [&](std::list<size_t>& edge_curve,
            size_t& front_vertex, size_t& end_vertex) {
        while(vertex_edge_adjacency.count(front_vertex) == 2 &&
                front_vertex != end_vertex) {
            auto adj_edges = vertex_edge_adjacency.equal_range(front_vertex);
            for (auto itr = adj_edges.first; itr!=adj_edges.second; itr++) {
                const size_t uei = itr->second;
                assert(uE2E.at(uei).size() != 2);
                const size_t ei = uE2E[uei][0];
                if (uei == edge_curve.back()) continue;
                size_t s,d;
                get_edge_end_points(ei, s, d);
                edge_curve.push_back(uei);
                if (s == front_vertex) {
                    front_vertex = d;
                } else if (d == front_vertex) {
                    front_vertex = s;
                } else {
                    throw "Invalid vertex/edge adjacency!";
                }
                break;
            }
        }
    };

    auto expand_backward = [&](std::list<size_t>& edge_curve,
            size_t& front_vertex, size_t& end_vertex) {
        while(vertex_edge_adjacency.count(front_vertex) == 2 &&
                front_vertex != end_vertex) {
            auto adj_edges = vertex_edge_adjacency.equal_range(front_vertex);
            for (auto itr = adj_edges.first; itr!=adj_edges.second; itr++) {
                const size_t uei = itr->second;
                assert(uE2E.at(uei).size() != 2);
                const size_t ei = uE2E[uei][0];
                if (uei == edge_curve.front()) continue;
                size_t s,d;
                get_edge_end_points(ei, s, d);
                edge_curve.push_front(uei);
                if (s == front_vertex) {
                    front_vertex = d;
                } else if (d == front_vertex) {
                    front_vertex = s;
                } else {
                    throw "Invalid vertex/edge adjcency!";
                }
                break;
            }
        }
    };

    std::vector<bool> visited(num_unique_edges, false);
    for (const size_t i : non_manifold_edges) {
        if (visited[i]) continue;
        std::list<size_t> edge_curve;
        edge_curve.push_back(i);

        const auto& adj_edges = uE2E[i];
        assert(adj_edges.size() != 2);
        const size_t ei = adj_edges[0];
        size_t s,d;
        get_edge_end_points(ei, s, d);

        expand_forward(edge_curve, d, s);
        expand_backward(edge_curve, s, d);
        curves.emplace_back(edge_curve.begin(), edge_curve.end());
        for (auto itr = edge_curve.begin(); itr!=edge_curve.end(); itr++) {
            visited[*itr] = true;
        }
    }

}
