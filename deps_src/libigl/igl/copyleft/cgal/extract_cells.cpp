// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//
#include "extract_cells.h"
#include "closest_facet.h"
#include "order_facets_around_edge.h"
#include "outer_facet.h"
#include "submesh_aabb_tree.h"
#include "../../extract_manifold_patches.h"
#include "../../facet_components.h"
#include "../../get_seconds.h"
#include "../../triangle_triangle_adjacency.h"
#include "../../unique_edge_map.h"
#include "../../vertex_triangle_adjacency.h"

#include <CGAL/AABB_tree.h>
#include <CGAL/AABB_traits.h>
#include <CGAL/AABB_triangle_primitive.h>
#include <CGAL/intersections.h>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>

#include <iostream>
#include <vector>
#include <queue>
#include <map>
#include <set>

//#define EXTRACT_CELLS_DEBUG

template<
  typename DerivedV,
  typename DerivedF,
  typename DerivedC >
IGL_INLINE size_t igl::copyleft::cgal::extract_cells(
  const Eigen::PlainObjectBase<DerivedV>& V,
  const Eigen::PlainObjectBase<DerivedF>& F,
  Eigen::PlainObjectBase<DerivedC>& cells)
{
  const size_t num_faces = F.rows();
  // Construct edge adjacency
  Eigen::MatrixXi E, uE;
  Eigen::VectorXi EMAP;
  std::vector<std::vector<size_t> > uE2E;
  igl::unique_edge_map(F, E, uE, EMAP, uE2E);
  // Cluster into manifold patches
  Eigen::VectorXi P;
  igl::extract_manifold_patches(F, EMAP, uE2E, P);
  // Extract cells
  DerivedC per_patch_cells;
  const size_t num_cells =
    igl::copyleft::cgal::extract_cells(V,F,P,E,uE,uE2E,EMAP,per_patch_cells);
  // Distribute per-patch cell information to each face
  cells.resize(num_faces, 2);
  for (size_t i=0; i<num_faces; i++) 
  {
    cells.row(i) = per_patch_cells.row(P[i]);
  }
  return num_cells;
}


template<
  typename DerivedV,
  typename DerivedF,
  typename DerivedP,
  typename DerivedE,
  typename DeriveduE,
  typename uE2EType,
  typename DerivedEMAP,
  typename DerivedC >
IGL_INLINE size_t igl::copyleft::cgal::extract_cells(
  const Eigen::PlainObjectBase<DerivedV>& V,
  const Eigen::PlainObjectBase<DerivedF>& F,
  const Eigen::PlainObjectBase<DerivedP>& P,
  const Eigen::PlainObjectBase<DerivedE>& E,
  const Eigen::PlainObjectBase<DeriveduE>& uE,
  const std::vector<std::vector<uE2EType> >& uE2E,
  const Eigen::PlainObjectBase<DerivedEMAP>& EMAP,
  Eigen::PlainObjectBase<DerivedC>& cells) 
{
  // Trivial base case
  if(P.size() == 0)
  {
    assert(F.size() == 0);
    cells.resize(0,2);
    return 0;
  }

  typedef CGAL::Exact_predicates_exact_constructions_kernel Kernel;
  typedef Kernel::Point_3 Point_3;
  typedef Kernel::Plane_3 Plane_3;
  typedef Kernel::Segment_3 Segment_3;
  typedef Kernel::Triangle_3 Triangle;
  typedef std::vector<Triangle>::iterator Iterator;
  typedef CGAL::AABB_triangle_primitive<Kernel, Iterator> Primitive;
  typedef CGAL::AABB_traits<Kernel, Primitive> AABB_triangle_traits;
  typedef CGAL::AABB_tree<AABB_triangle_traits> Tree;

#ifdef EXTRACT_CELLS_DEBUG
  const auto & tictoc = []() -> double
  {
    static double t_start = igl::get_seconds();
    double diff = igl::get_seconds()-t_start;
    t_start += diff;
    return diff;
  };
  const auto log_time = [&](const std::string& label) -> void {
    std::cout << "extract_cells." << label << ": "
      << tictoc() << std::endl;
  };
  tictoc();
#else
  // no-op
  const auto log_time = [](const std::string){};
#endif
  const size_t num_faces = F.rows();
  typedef typename DerivedF::Scalar Index;
  assert(P.size() > 0);
  const size_t num_patches = P.maxCoeff()+1;

  // Extract all cells...
  DerivedC raw_cells;
  const size_t num_raw_cells =
    extract_cells_single_component(V,F,P,uE,uE2E,EMAP,raw_cells);
  log_time("extract_single_component_cells");

  // Compute triangle-triangle adjacency data-structure
  std::vector<std::vector<std::vector<Index > > > TT,_1;
  igl::triangle_triangle_adjacency(E, EMAP, uE2E, false, TT, _1);
  log_time("compute_face_adjacency");

  // Compute connected components of the mesh
  Eigen::VectorXi C, counts;
  igl::facet_components(TT, C, counts);
  log_time("form_components");

  const size_t num_components = counts.size();
  // components[c] --> list of face indices into F of faces in component c
  std::vector<std::vector<size_t> > components(num_components);
  // Loop over all faces
  for (size_t i=0; i<num_faces; i++) 
  {
    components[C[i]].push_back(i);
  }
  // Convert vector lists to Eigen lists...
  // and precompute data-structures for each component
  std::vector<std::vector<size_t> > VF,VFi;
  igl::vertex_triangle_adjacency(V.rows(), F, VF, VFi);
  std::vector<Eigen::VectorXi> Is(num_components);
  std::vector<
    CGAL::AABB_tree<
      CGAL::AABB_traits<
        Kernel, 
        CGAL::AABB_triangle_primitive<
          Kernel, std::vector<
            Kernel::Triangle_3 >::iterator > > > > trees(num_components);
  std::vector< std::vector<Kernel::Triangle_3 > > 
    triangle_lists(num_components);
  std::vector<std::vector<bool> > in_Is(num_components);

  // Find outer facets, their orientations and cells for each component
  Eigen::VectorXi outer_facets(num_components);
  Eigen::VectorXi outer_facet_orientation(num_components);
  Eigen::VectorXi outer_cells(num_components);
  for (size_t i=0; i<num_components; i++)
  {
    Is[i].resize(components[i].size());
    std::copy(components[i].begin(), components[i].end(),Is[i].data());
    bool flipped;
    igl::copyleft::cgal::outer_facet(V, F, Is[i], outer_facets[i], flipped);
    outer_facet_orientation[i] = flipped?1:0;
    outer_cells[i] = raw_cells(P[outer_facets[i]], outer_facet_orientation[i]);
  }
#ifdef EXTRACT_CELLS_DEBUG
  log_time("outer_facet_per_component");
#endif

  // Compute barycenter of a triangle in mesh (V,F)
  //
  // Inputs:
  //   fid  index into F
  // Returns row-vector of barycenter coordinates
  const auto get_triangle_center = [&V,&F](const size_t fid) 
  {
    return ((V.row(F(fid,0))+V.row(F(fid,1))+V.row(F(fid,2)))/3.0).eval();
  };
  std::vector<std::vector<size_t> > nested_cells(num_raw_cells);
  std::vector<std::vector<size_t> > ambient_cells(num_raw_cells);
  std::vector<std::vector<size_t> > ambient_comps(num_components);
  // Only bother if there's more than one component
  if(num_components > 1) 
  {
    // construct bounding boxes for each component
    DerivedV bbox_min(num_components, 3);
    DerivedV bbox_max(num_components, 3);
    // Assuming our mesh (in exact numbers) fits in the range of double.
    bbox_min.setConstant(std::numeric_limits<double>::max());
    bbox_max.setConstant(std::numeric_limits<double>::min());
    // Loop over faces
    for (size_t i=0; i<num_faces; i++)
    {
      // component of this face
      const auto comp_id = C[i];
      const auto& f = F.row(i);
      for (size_t j=0; j<3; j++) 
      {
        for(size_t d=0;d<3;d++)
        {
          bbox_min(comp_id,d) = std::min(bbox_min(comp_id,d), V(f[j],d));
          bbox_max(comp_id,d) = std::max(bbox_max(comp_id,d), V(f[j],d));
        }
      }
    }
    // Return true if box of component ci intersects that of cj
    const auto bbox_intersects = [&bbox_max,&bbox_min](size_t ci, size_t cj)
    {
      return !(
        bbox_max(ci,0) < bbox_min(cj,0) ||
        bbox_max(ci,1) < bbox_min(cj,1) ||
        bbox_max(ci,2) < bbox_min(cj,2) ||
        bbox_max(cj,0) < bbox_min(ci,0) ||
        bbox_max(cj,1) < bbox_min(ci,1) ||
        bbox_max(cj,2) < bbox_min(ci,2));
    };
    
    // Loop over components. This section is O(m²)
    for (size_t i=0; i<num_components; i++)
    {
      // List of components that could overlap with component i
      std::vector<size_t> candidate_comps;
      candidate_comps.reserve(num_components);
      // Loop over components
      for (size_t j=0; j<num_components; j++) 
      {
        if (i == j) continue;
        if (bbox_intersects(i,j)) candidate_comps.push_back(j);
      }

      const size_t num_candidate_comps = candidate_comps.size();
      if (num_candidate_comps == 0) continue;

      // Build aabb tree for this component.
      submesh_aabb_tree(V,F,Is[i],trees[i],triangle_lists[i],in_Is[i]);

      // Get query points on each candidate component: barycenter of
      // outer-facet 
      DerivedV queries(num_candidate_comps, 3);
      for (size_t j=0; j<num_candidate_comps; j++)
      {
        const size_t index = candidate_comps[j];
        queries.row(j) = get_triangle_center(outer_facets[index]);
      }

      // Gather closest facets in ith component to each query point and their
      // orientations
      const auto& I = Is[i];
      const auto& tree = trees[i];
      const auto& in_I = in_Is[i];
      const auto& triangles = triangle_lists[i];

      Eigen::VectorXi closest_facets, closest_facet_orientations;
      closest_facet(
        V,
        F, 
        I, 
        queries,
        uE2E, 
        EMAP, 
        VF,
        VFi,
        tree,
        triangles,
        in_I,
        closest_facets, 
        closest_facet_orientations);
      // Loop over all candidates
      for (size_t j=0; j<num_candidate_comps; j++)
      {
        const size_t index = candidate_comps[j];
        const size_t closest_patch = P[closest_facets[j]];
        const size_t closest_patch_side = closest_facet_orientations[j] ? 0:1;
        // The cell id of the closest patch
        const size_t ambient_cell =
          raw_cells(closest_patch,closest_patch_side);
        if (ambient_cell != (size_t)outer_cells[i])
        {
          // ---> component index inside component i, because the cell of the
          // closest facet on i to component index is **not** the same as the
          // "outer cell" of component i: component index is **not** outside of
          // component i (therefore it's inside).
          nested_cells[ambient_cell].push_back(outer_cells[index]);
          ambient_cells[outer_cells[index]].push_back(ambient_cell);
          ambient_comps[index].push_back(i);
        }
      }
    }
  }

#ifdef EXTRACT_CELLS_DEBUG
    log_time("nested_relationship");
#endif

    const size_t INVALID = std::numeric_limits<size_t>::max();
    const size_t INFINITE_CELL = num_raw_cells;
    std::vector<size_t> embedded_cells(num_raw_cells, INVALID);
    for (size_t i=0; i<num_components; i++) {
        const size_t outer_cell = outer_cells[i];
        const auto& ambient_comps_i = ambient_comps[i];
        const auto& ambient_cells_i = ambient_cells[outer_cell];
        const size_t num_ambient_comps = ambient_comps_i.size();
        assert(num_ambient_comps == ambient_cells_i.size());
        if (num_ambient_comps > 0) {
            size_t embedded_comp = INVALID;
            size_t embedded_cell = INVALID;
            for (size_t j=0; j<num_ambient_comps; j++) {
                if (ambient_comps[ambient_comps_i[j]].size() ==
                        num_ambient_comps-1) {
                    embedded_comp = ambient_comps_i[j];
                    embedded_cell = ambient_cells_i[j];
                    break;
                }
            }
            assert(embedded_comp != INVALID);
            assert(embedded_cell != INVALID);
            embedded_cells[outer_cell] = embedded_cell;
        } else {
            embedded_cells[outer_cell] = INFINITE_CELL;
        }
    }
    for (size_t i=0; i<num_patches; i++) {
        if (embedded_cells[raw_cells(i,0)] != INVALID) {
            raw_cells(i,0) = embedded_cells[raw_cells(i, 0)];
        }
        if (embedded_cells[raw_cells(i,1)] != INVALID) {
            raw_cells(i,1) = embedded_cells[raw_cells(i, 1)];
        }
    }

    size_t count = 0;
    std::vector<size_t> mapped_indices(num_raw_cells+1, INVALID);
    // Always map infinite cell to index 0.
    mapped_indices[INFINITE_CELL] = count;
    count++;

    for (size_t i=0; i<num_patches; i++) {
        const size_t old_positive_cell_id = raw_cells(i, 0);
        const size_t old_negative_cell_id = raw_cells(i, 1);
        size_t positive_cell_id, negative_cell_id;
        if (mapped_indices[old_positive_cell_id] == INVALID) {
            mapped_indices[old_positive_cell_id] = count;
            positive_cell_id = count;
            count++;
        } else {
            positive_cell_id = mapped_indices[old_positive_cell_id];
        }
        if (mapped_indices[old_negative_cell_id] == INVALID) {
            mapped_indices[old_negative_cell_id] = count;
            negative_cell_id = count;
            count++;
        } else {
            negative_cell_id = mapped_indices[old_negative_cell_id];
        }
        raw_cells(i, 0) = positive_cell_id;
        raw_cells(i, 1) = negative_cell_id;
    }
    cells = raw_cells;
#ifdef EXTRACT_CELLS_DEBUG
    log_time("finalize");
#endif
    return count;
}

template<
  typename DerivedV,
  typename DerivedF,
  typename DerivedP,
  typename DeriveduE,
  typename uE2EType,
  typename DerivedEMAP,
  typename DerivedC>
IGL_INLINE size_t igl::copyleft::cgal::extract_cells_single_component(
  const Eigen::PlainObjectBase<DerivedV>& V,
  const Eigen::PlainObjectBase<DerivedF>& F,
  const Eigen::PlainObjectBase<DerivedP>& P,
  const Eigen::PlainObjectBase<DeriveduE>& uE,
  const std::vector<std::vector<uE2EType> >& uE2E,
  const Eigen::PlainObjectBase<DerivedEMAP>& EMAP,
  Eigen::PlainObjectBase<DerivedC>& cells)
{
  const size_t num_faces = F.rows();
  // Input:
  //   index  index into #F*3 list of undirect edges
  // Returns index into face
  const auto edge_index_to_face_index = [&num_faces](size_t index)
  {
    return index % num_faces;
  };
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
  std::vector<std::map<size_t, size_t> > patch_adj(num_patches);
  for (size_t i=0; i<num_unique_edges; i++) {
    const size_t s = uE(i,0);
    const size_t d = uE(i,1);
    const auto adj_faces = uE2E[i];
    const size_t num_adj_faces = adj_faces.size();
    if (num_adj_faces > 2) {
      for (size_t j=0; j<num_adj_faces; j++) {
        const size_t patch_j = P[edge_index_to_face_index(adj_faces[j])];
        for (size_t k=j+1; k<num_adj_faces; k++) {
          const size_t patch_k = P[edge_index_to_face_index(adj_faces[k])];
          if (patch_adj[patch_j].find(patch_k) == patch_adj[patch_j].end()) {
            patch_adj[patch_j].insert({patch_k, i});
          }
          if (patch_adj[patch_k].find(patch_j) == patch_adj[patch_k].end()) {
            patch_adj[patch_k].insert({patch_j, i});
          }
        }
      }
    }
  }


  const int INVALID = std::numeric_limits<int>::max();
  std::vector<size_t> cell_labels(num_patches * 2);
  for (size_t i=0; i<num_patches; i++) cell_labels[i] = i;
  std::vector<std::set<size_t> > equivalent_cells(num_patches*2);
  std::vector<bool> processed(num_unique_edges, false);

  size_t label_count=0;
  for (size_t i=0; i<num_patches; i++) {
    for (const auto& entry : patch_adj[i]) {
      const size_t neighbor_patch = entry.first;
      const size_t uei = entry.second;
      if (processed[uei]) continue;
      processed[uei] = true;

      const auto& adj_faces = uE2E[uei];
      const size_t num_adj_faces = adj_faces.size();
      assert(num_adj_faces > 2);

      const size_t s = uE(uei,0);
      const size_t d = uE(uei,1);

      std::vector<int> signed_adj_faces;
      for (auto ej : adj_faces)
      {
        const size_t fid = edge_index_to_face_index(ej);
        bool cons = is_consistent(fid, s, d);
        signed_adj_faces.push_back((fid+1)*(cons ? 1:-1));
      }
      {
        // Sort adjacent faces cyclically around {s,d}
        Eigen::VectorXi order;
        // order[f] will reveal the order of face f in signed_adj_faces
        order_facets_around_edge(V, F, s, d, signed_adj_faces, order);
        for (size_t j=0; j<num_adj_faces; j++) {
          const size_t curr_idx = j;
          const size_t next_idx = (j+1)%num_adj_faces;
          const size_t curr_patch_idx =
            P[edge_index_to_face_index(adj_faces[order[curr_idx]])];
          const size_t next_patch_idx =
            P[edge_index_to_face_index(adj_faces[order[next_idx]])];
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

  assert((cells.array() != INVALID).all());
  return count;
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template unsigned long igl::copyleft::cgal::extract_cells<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, unsigned long, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 1, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<std::vector<unsigned long, std::allocator<unsigned long> >, std::allocator<std::vector<unsigned long, std::allocator<unsigned long> > > > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
// generated by autoexplicit.sh
template unsigned long igl::copyleft::cgal::extract_cells<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, unsigned long, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 1, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<std::vector<unsigned long, std::allocator<unsigned long> >, std::allocator<std::vector<unsigned long, std::allocator<unsigned long> > > > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
template unsigned long igl::copyleft::cgal::extract_cells<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, unsigned long, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<std::vector<unsigned long, std::allocator<unsigned long> >, std::allocator<std::vector<unsigned long, std::allocator<unsigned long> > > > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template unsigned long igl::copyleft::cgal::extract_cells<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#ifdef WIN32
template unsigned __int64 igl::copyleft::cgal::extract_cells<class Eigen::Matrix<class CGAL::Lazy_exact_nt<class CGAL::Gmpq>, -1, -1, 0, -1, -1>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>, class Eigen::Matrix<int, -1, 1, 0, -1, 1>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>, unsigned __int64, class Eigen::Matrix<int, -1, 1, 0, -1, 1>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>>(class Eigen::PlainObjectBase<class Eigen::Matrix<class CGAL::Lazy_exact_nt<class CGAL::Gmpq>, -1, -1, 0, -1, -1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, 1, 0, -1, 1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> const &, class std::vector<class std::vector<unsigned __int64, class std::allocator<unsigned __int64>>, class std::allocator<class std::vector<unsigned __int64, class std::allocator<unsigned __int64>>>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, 1, 0, -1, 1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> &);
template unsigned __int64 igl::copyleft::cgal::extract_cells<class Eigen::Matrix<class CGAL::Lazy_exact_nt<class CGAL::Gmpq>, -1, -1, 1, -1, -1>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>, class Eigen::Matrix<int, -1, 1, 0, -1, 1>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>, unsigned __int64, class Eigen::Matrix<int, -1, 1, 0, -1, 1>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>>(class Eigen::PlainObjectBase<class Eigen::Matrix<class CGAL::Lazy_exact_nt<class CGAL::Gmpq>, -1, -1, 1, -1, -1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, 1, 0, -1, 1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> const &, class std::vector<class std::vector<unsigned __int64, class std::allocator<unsigned __int64>>, class std::allocator<class std::vector<unsigned __int64, class std::allocator<unsigned __int64>>>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, 1, 0, -1, 1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> &);
template unsigned __int64 igl::copyleft::cgal::extract_cells<class Eigen::Matrix<class CGAL::Lazy_exact_nt<class CGAL::Gmpq>, -1, -1, 1, -1, -1>, class Eigen::Matrix<int, -1, 3, 1, -1, 3>, class Eigen::Matrix<int, -1, 1, 0, -1, 1>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>, unsigned __int64, class Eigen::Matrix<int, -1, 1, 0, -1, 1>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>>(class Eigen::PlainObjectBase<class Eigen::Matrix<class CGAL::Lazy_exact_nt<class CGAL::Gmpq>, -1, -1, 1, -1, -1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, 3, 1, -1, 3>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, 1, 0, -1, 1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> const &, class std::vector<class std::vector<unsigned __int64, class std::allocator<unsigned __int64>>, class std::allocator<class std::vector<unsigned __int64, class std::allocator<unsigned __int64>>>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, 1, 0, -1, 1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> &);
#endif
#endif
