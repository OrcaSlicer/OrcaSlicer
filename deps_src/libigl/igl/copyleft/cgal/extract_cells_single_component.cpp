// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
//
#include "extract_cells_single_component.h"
#include "order_facets_around_edge.h"
#include "../../C_STR.h"
#include "../../get_seconds.h"
#include <limits>
#include <vector>
#include <set>
#include <map>
#include <queue>

//#define EXTRACT_CELLS_SINGLE_COMPONENT_TIMING

template<
  typename DerivedV,
  typename DerivedF,
  typename DerivedP,
  typename DeriveduE,
  typename DeriveduEC,
  typename DeriveduEE,
  typename DerivedC>
IGL_INLINE int igl::copyleft::cgal::extract_cells_single_component(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedF>& F,
  const Eigen::MatrixBase<DerivedP>& P,
  const Eigen::MatrixBase<DeriveduE>& uE,
  const Eigen::MatrixBase<DeriveduEC>& uEC,
  const Eigen::MatrixBase<DeriveduEE>& uEE,
  Eigen::PlainObjectBase<DerivedC>& cells)
{
#ifdef EXTRACT_CELLS_SINGLE_COMPONENT_TIMING
  const auto & tictoc = []() -> double
  {
    static double t_start = igl::get_seconds();
    double diff = igl::get_seconds()-t_start;
    t_start += diff;
    return diff;
  };
  const auto log_time = [&](const std::string& label) -> void {
    printf("%50s: %0.5lf\n",
      C_STR("extrac*_single_component." << label),tictoc());
  };
  tictoc();
#else
  // no-op
  const auto log_time = [](const std::string){};
#endif
  const size_t num_faces = F.rows();
  // Input:
  //   index  index into #F*3 list of undirect edges
  // Returns index into face
  const auto e2f = [&num_faces](size_t index) { return index % num_faces; };
  // Determine if a face (containing undirected edge {s,d} is consistently
  // oriented with directed edge {s,d} (or otherwise it is with {d,s})
  //
  // Inputs:
  //   fid  face index into F
  //   s  source index of edge
  //   d  destination index of edge
  // Returns true if face F(fid,:) is consistent with {s,d}
  const auto is_consistent =
    [&F](const size_t fid, const size_t s, const size_t d) -> bool
  {
    if ((size_t)F(fid, 0) == s && (size_t)F(fid, 1) == d) return false;
    if ((size_t)F(fid, 1) == s && (size_t)F(fid, 2) == d) return false;
    if ((size_t)F(fid, 2) == s && (size_t)F(fid, 0) == d) return false;

    if ((size_t)F(fid, 0) == d && (size_t)F(fid, 1) == s) return true;
    if ((size_t)F(fid, 1) == d && (size_t)F(fid, 2) == s) return true;
    if ((size_t)F(fid, 2) == d && (size_t)F(fid, 0) == s) return true;
    throw "Invalid face!";
    return false;
  };

  const size_t num_unique_edges = uE.rows();
  const size_t num_patches = P.maxCoeff() + 1;

  // Build patch-patch adjacency list.
  //
  // Does this really need to be a map? Or do I just want a list of neighbors
  // and for each neighbor an index to a unique edge? (i.e., a sparse matrix)
  std::vector<std::map<size_t, size_t> > patch_adj(num_patches);
  for (size_t i=0; i<num_unique_edges; i++)
  {
    //const auto adj_faces = uE2E[i];
    //const size_t num_adj_faces = adj_faces.size();
    const size_t num_adj_faces = uEC(i+1)-uEC(i);
    if (num_adj_faces > 2)
    {
      //for (size_t j=0; j<num_adj_faces; j++) {
      //  const auto aj = adj_faces[j];
      for (size_t ij=uEC(i); ij<uEC(i+1); ij++) 
      {
        const auto aj = uEE(ij);
        const size_t patch_j = P[e2f(aj)];
        //for (size_t k=j+1; k<num_adj_faces; k++) {
        //  const auto ak = adj_faces[k];
        for (size_t ik=ij+1; ik<uEC(i+1); ik++) 
        {
          const auto ak = uEE(ik);
          const size_t patch_k = P[e2f(ak)];
          if (patch_adj[patch_j].find(patch_k) == patch_adj[patch_j].end())
          {
            patch_adj[patch_j].insert({patch_k, i});
          }
          if (patch_adj[patch_k].find(patch_j) == patch_adj[patch_k].end())
          {
            patch_adj[patch_k].insert({patch_j, i});
          }
        }
      }
    }
  }
  log_time("patch-adjacency");


  const int INVALID = std::numeric_limits<int>::max();
  //std::vector<size_t> cell_labels(num_patches * 2);
  //for (size_t i=0; i<num_patches; i++) cell_labels[i] = i;
  std::vector<std::set<size_t> > equivalent_cells(num_patches*2);
  std::vector<bool> processed(num_unique_edges, false);

  // bottleneck appears to be `order_facets_around_edge`
  for (size_t i=0; i<num_patches; i++) 
  {
    for (const auto& entry : patch_adj[i]) 
    {
      const size_t uei = entry.second;
      if (processed[uei]) continue;
      processed[uei] = true;

      //const auto& adj_faces = uE2E[uei];
      //const size_t num_adj_faces = adj_faces.size();
      const size_t num_adj_faces = uEC(uei+1)-uEC(uei);
      assert(num_adj_faces > 2);

      const size_t s = uE(uei,0);
      const size_t d = uE(uei,1);

      std::vector<int> signed_adj_faces;
      //for (auto ej : adj_faces)
      for(size_t ij = uEC(uei);ij<uEC(uei+1);ij++)
      {
        const size_t ej = uEE(ij);
        const size_t fid = e2f(ej);
        bool cons = is_consistent(fid, s, d);
        signed_adj_faces.push_back((fid+1)*(cons ? 1:-1));
      }
      {
        // Sort adjacent faces cyclically around {s,d}
        Eigen::VectorXi order;
        // order[f] will reveal the order of face f in signed_adj_faces
        order_facets_around_edge(V, F, s, d, signed_adj_faces, order);
        for (size_t j=0; j<num_adj_faces; j++) 
        {
          const size_t curr_idx = j;
          const size_t next_idx = (j+1)%num_adj_faces;
          //const size_t curr_patch_idx = P[e2f(adj_faces[order[curr_idx]])];
          //const size_t next_patch_idx = P[e2f(adj_faces[order[next_idx]])];
          const size_t curr_patch_idx = P[e2f( uEE(uEC(uei)+order[curr_idx]) )];
          const size_t next_patch_idx = P[e2f( uEE(uEC(uei)+order[next_idx]) )];
          const bool curr_cons = signed_adj_faces[order[curr_idx]] > 0;
          const bool next_cons = signed_adj_faces[order[next_idx]] > 0;
          const size_t curr_cell_idx = curr_patch_idx*2 + (curr_cons?0:1);
          const size_t next_cell_idx = next_patch_idx*2 + (next_cons?1:0);
          equivalent_cells[curr_cell_idx].insert(next_cell_idx);
          equivalent_cells[next_cell_idx].insert(curr_cell_idx);
        }
      }
    }
  }
#ifdef EXTRACT_CELLS_SINGLE_COMPONENT_TIMING
  log_time("equivalent_cells");
#endif 

  size_t count=0;
  cells.resize(num_patches, 2);
  cells.setConstant(INVALID);
  const auto extract_equivalent_cells = [&](size_t i) {
    if (cells(i/2, i%2) != INVALID) return;
    std::queue<size_t> Q;
    Q.push(i);
    cells(i/2, i%2) = count;
    while (!Q.empty()) {
      const size_t index = Q.front();
      Q.pop();
      for (const auto j : equivalent_cells[index]) {
        if (cells(j/2, j%2) == INVALID) {
          cells(j/2, j%2) = count;
          Q.push(j);
        }
      }
    }
    count++;
  };
  for (size_t i=0; i<num_patches; i++) {
    extract_equivalent_cells(i*2);
    extract_equivalent_cells(i*2+1);
  }
  log_time("extract-equivalent_cells");

  assert((cells.array() != INVALID).all());
  return count;
}


#ifdef IGL_STATIC_LIBRARY
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
// Explicit template instantiation
template int igl::copyleft::cgal::extract_cells_single_component<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, 
    Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template int igl::copyleft::cgal::extract_cells_single_component<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, 
    Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
