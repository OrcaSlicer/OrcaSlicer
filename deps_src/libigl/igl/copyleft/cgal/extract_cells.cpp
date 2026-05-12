// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
//
#include "extract_cells.h"
#include "extract_cells_single_component.h"
#include "closest_facet.h"
#include "outer_facet.h"
#include "submesh_aabb_tree.h"
#include "../../extract_manifold_patches.h"
#include "../../facet_components.h"
#include "../../IGL_ASSERT.h"
#include "../../parallel_for.h"
#include "../../get_seconds.h"
#include "../../PlainMatrix.h"
#include "../../triangle_triangle_adjacency.h"
#include "../../unique_edge_map.h"
#include "../../C_STR.h"
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

//#define EXTRACT_CELLS_TIMING

template<
  typename DerivedV,
  typename DerivedF,
  typename DerivedC >
IGL_INLINE size_t igl::copyleft::cgal::extract_cells(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedF>& F,
  Eigen::PlainObjectBase<DerivedC>& cells)
{
  using Index = typename DerivedF::Scalar;
  static_assert(
    std::is_same<Index, typename DerivedC::Scalar>::value,
    "Index type mismatch");
  using MatrixXI = Eigen::Matrix<Index, Eigen::Dynamic, Eigen::Dynamic>;
  using VectorXI = Eigen::Matrix<Index, Eigen::Dynamic, 1>;
  const size_t num_faces = F.rows();
  // Construct edge adjacency
  MatrixXI E, uE;
  VectorXI EMAP;
  VectorXI uEC,uEE;
  igl::unique_edge_map(F, E, uE, EMAP, uEC, uEE);
  // Cluster into manifold patches
  VectorXI P;
  igl::extract_manifold_patches(F, EMAP, uEC, uEE, P);
  // Extract cells
  DerivedC per_patch_cells;
  const size_t ncells = extract_cells(V,F,P,uE,EMAP,uEC,uEE,per_patch_cells);
  // Distribute per-patch cell information to each face
  cells.resize(num_faces, 2);
  for (size_t i=0; i<num_faces; i++)
  {
    cells.row(i) = per_patch_cells.row(P[i]);
  }
  return ncells;
}


template<
  typename DerivedV,
  typename DerivedF,
  typename DerivedP,
  typename DeriveduE,
  typename DerivedEMAP,
  typename DeriveduEC,
  typename DeriveduEE,
  typename DerivedC >
IGL_INLINE size_t igl::copyleft::cgal::extract_cells(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedF>& F,
  const Eigen::MatrixBase<DerivedP>& P,
  const Eigen::MatrixBase<DeriveduE>& uE,
  const Eigen::MatrixBase<DerivedEMAP>& EMAP,
  const Eigen::MatrixBase<DeriveduEC>& uEC,
  const Eigen::MatrixBase<DeriveduEE>& uEE,
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

#ifdef EXTRACT_CELLS_TIMING
  const auto & tictoc = []() -> double
  {
    static double t_start = igl::get_seconds();
    double diff = igl::get_seconds()-t_start;
    t_start += diff;
    return diff;
  };
  const auto log_time = [&](const std::string& label) -> void {
    printf("%50s: %0.5lf\n",
      C_STR("extract_cells." << label),tictoc());
  };
  tictoc();
#else
  // no-op
  const auto log_time = [](const std::string){};
#endif
  const size_t num_faces = F.rows();
  typedef typename DerivedF::Scalar Index;
  using VectorXI = Eigen::Matrix<Index, Eigen::Dynamic, 1>;
  assert(P.size() > 0);
  const size_t num_patches = P.maxCoeff()+1;

  // Extract all cells...
  DerivedC raw_cells;
  const int num_raw_cells =
    extract_cells_single_component(V,F,P,uE,uEC,uEE,raw_cells);
  log_time("extract_cells_single_component");

  // Compute triangle-triangle adjacency data-structure
  std::vector<std::vector<std::vector<Index > > > TT,_1;
  igl::triangle_triangle_adjacency(EMAP, uEC, uEE, false, TT, _1);
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
  std::vector<VectorXI> Is(num_components);
  std::vector<
    CGAL::AABB_tree<
      CGAL::AABB_traits<
        Kernel,
        CGAL::AABB_triangle_primitive<
          Kernel, std::vector<
            Kernel::Triangle_3 >::iterator > > > > trees(num_components);
  std::vector< std::vector<Kernel::Triangle_3 > >
    triangle_lists(num_components);
  // O(num_components * num_faces) 
  // In general, extract_cells appears to have O(num_components * num_faces)
  // performance. This could be painfully tested by a processing a cloud of
  // tetrahedra.
  std::vector<std::vector<bool> > in_Is(num_components);

  // Find outer facets, their orientations and cells for each component
  VectorXI outer_facets(num_components);
  VectorXI outer_facet_orientation(num_components);
  VectorXI outer_cells(num_components);
  igl::parallel_for(num_components,[&](size_t i)
  {
    Is[i].resize(components[i].size());
    std::copy(components[i].begin(), components[i].end(),Is[i].data());
    bool flipped;
    igl::copyleft::cgal::outer_facet(V, F, Is[i], outer_facets[i], flipped);
    outer_facet_orientation[i] = flipped?1:0;
    outer_cells[i] = raw_cells(P[outer_facets[i]], outer_facet_orientation[i]);
  },1000);
#ifdef EXTRACT_CELLS_TIMING
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
    PlainMatrix<DerivedV,Eigen::Dynamic,3> bbox_min(num_components, 3);
    PlainMatrix<DerivedV,Eigen::Dynamic,3> bbox_max(num_components, 3);
    // Assuming our mesh (in exact numbers) fits in the range of double.
    bbox_min.setConstant(std::numeric_limits<double>::max());
    bbox_max.setConstant(std::numeric_limits<double>::lowest());
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
      PlainMatrix<DerivedV,Eigen::Dynamic,3> queries(num_candidate_comps, 3);
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

      VectorXI closest_facets, closest_facet_orientations;
      closest_facet(
        V,
        F,
        I,
        queries,
        EMAP,
        uEC,
        uEE,
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

#ifdef EXTRACT_CELLS_TIMING
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
            IGL_ASSERT(embedded_comp != INVALID);
            IGL_ASSERT(embedded_cell != INVALID);
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
#ifdef EXTRACT_CELLS_TIMING
    log_time("finalize");
#endif
    return count;
}

#ifdef IGL_STATIC_LIBRARY
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
// Explicit template instantiation
// generated by autoexplicit.sh
template size_t igl::copyleft::cgal::extract_cells<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template size_t igl::copyleft::cgal::extract_cells<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template size_t igl::copyleft::cgal::extract_cells<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#ifdef WIN32
#endif
#endif
