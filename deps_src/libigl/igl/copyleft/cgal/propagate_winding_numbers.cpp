// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//
#include "propagate_winding_numbers.h"
#include "../../extract_manifold_patches.h"
#include "../../extract_non_manifold_edge_curves.h"
#include "../../facet_components.h"
#include "../../unique_edge_map.h"
#include "../../piecewise_constant_winding_number.h"
#include "../../writeOBJ.h"
#include "../../writePLY.h"
#include "../../get_seconds.h"
#include "../../LinSpaced.h"
#include "order_facets_around_edge.h"
#include "outer_facet.h"
#include "closest_facet.h"
#include "assign.h"
#include "extract_cells.h"
#include "cell_adjacency.h"

#include <stdexcept>
#include <limits>
#include <vector>
#include <tuple>
#include <queue>

//#define PROPAGATE_WINDING_NUMBER_TIMING

template<
  typename DerivedV,
  typename DerivedF,
  typename DerivedL,
  typename DerivedW>
IGL_INLINE bool igl::copyleft::cgal::propagate_winding_numbers(
    const Eigen::PlainObjectBase<DerivedV>& V,
    const Eigen::PlainObjectBase<DerivedF>& F,
    const Eigen::PlainObjectBase<DerivedL>& labels,
    Eigen::PlainObjectBase<DerivedW>& W) 
{
#ifdef PROPAGATE_WINDING_NUMBER_TIMING
  const auto & tictoc = []() -> double
  {
    static double t_start = igl::get_seconds();
    double diff = igl::get_seconds()-t_start;
    t_start += diff;
    return diff;
  };
  const auto log_time = [&](const std::string& label) -> void {
    std::cout << "propagate_winding_num." << label << ": "
      << tictoc() << std::endl;
  };
  tictoc();
#endif

  Eigen::MatrixXi E, uE;
  Eigen::VectorXi EMAP;
  std::vector<std::vector<size_t> > uE2E;
  igl::unique_edge_map(F, E, uE, EMAP, uE2E);

  Eigen::VectorXi P;
  const size_t num_patches = igl::extract_manifold_patches(F, EMAP, uE2E, P);

  DerivedW per_patch_cells;
  const size_t num_cells =
    igl::copyleft::cgal::extract_cells(
        V, F, P, E, uE, uE2E, EMAP, per_patch_cells);
#ifdef PROPAGATE_WINDING_NUMBER_TIMING
  log_time("cell_extraction");
#endif

  return propagate_winding_numbers(V, F,
          uE, uE2E,
          num_patches, P,
          num_cells, per_patch_cells,
          labels, W);
}


template<
  typename DerivedV,
  typename DerivedF,
  typename DeriveduE,
  typename uE2EType,
  typename DerivedP,
  typename DerivedC,
  typename DerivedL,
  typename DerivedW>
IGL_INLINE bool igl::copyleft::cgal::propagate_winding_numbers(
    const Eigen::PlainObjectBase<DerivedV>& V,
    const Eigen::PlainObjectBase<DerivedF>& F,
    const Eigen::PlainObjectBase<DeriveduE>& uE,
    const std::vector<std::vector<uE2EType> >& uE2E,
    const size_t num_patches,
    const Eigen::PlainObjectBase<DerivedP>& P,
    const size_t num_cells,
    const Eigen::PlainObjectBase<DerivedC>& C,
    const Eigen::PlainObjectBase<DerivedL>& labels,
    Eigen::PlainObjectBase<DerivedW>& W)
{
#ifdef PROPAGATE_WINDING_NUMBER_TIMING
  const auto & tictoc = []() -> double
  {
    static double t_start = igl::get_seconds();
    double diff = igl::get_seconds()-t_start;
    t_start += diff;
    return diff;
  };
  const auto log_time = [&](const std::string& label) -> void {
    std::cout << "propagate_winding_num." << label << ": "
      << tictoc() << std::endl;
  };
  tictoc();
#endif

  bool valid = true;
  // https://github.com/libigl/libigl/issues/674
  if (!igl::piecewise_constant_winding_number(F, uE, uE2E)) 
  {
    assert(false && "Input mesh is not PWN");
    std::cerr << "Input mesh is not PWN!" << std::endl;
    valid = false;
  }

  const size_t num_faces = F.rows();
  typedef std::tuple<typename DerivedC::Scalar, bool, size_t> CellConnection;
  std::vector<std::set<CellConnection> > cell_adj;
  igl::copyleft::cgal::cell_adjacency(C, num_cells, cell_adj);
#ifdef PROPAGATE_WINDING_NUMBER_TIMING
  log_time("cell_connectivity");
#endif

  auto save_cell = [&](const std::string& filename, size_t cell_id) -> void{
    std::vector<size_t> faces;
    for (size_t i=0; i<num_patches; i++) {
      if ((C.row(i).array() == cell_id).any()) {
        for (size_t j=0; j<num_faces; j++) {
          if ((size_t)P[j] == i) {
            faces.push_back(j);
          }
        }
      }
    }
    Eigen::MatrixXi cell_faces(faces.size(), 3);
    for (size_t i=0; i<faces.size(); i++) {
      cell_faces.row(i) = F.row(faces[i]);
    }
    Eigen::MatrixXd vertices;
    assign(V,vertices);
    writePLY(filename, vertices, cell_faces);
  };

#ifndef NDEBUG
  {
    // Check for odd cycle.
    Eigen::VectorXi cell_labels(num_cells);
    cell_labels.setZero();
    Eigen::VectorXi parents(num_cells);
    parents.setConstant(-1);
    auto trace_parents = [&](size_t idx) -> std::list<size_t> {
      std::list<size_t> path;
      path.push_back(idx);
      while ((size_t)parents[path.back()] != path.back()) {
        path.push_back(parents[path.back()]);
      }
      return path;
    };
    for (size_t i=0; i<num_cells; i++) {
      if (cell_labels[i] == 0) {
        cell_labels[i] = 1;
        std::queue<size_t> Q;
        Q.push(i);
        parents[i] = i;
        while (!Q.empty()) {
          size_t curr_idx = Q.front();
          Q.pop();
          int curr_label = cell_labels[curr_idx];
          for (const auto& neighbor : cell_adj[curr_idx]) {
            if (cell_labels[std::get<0>(neighbor)] == 0) 
            {
              cell_labels[std::get<0>(neighbor)] = curr_label * -1;
              Q.push(std::get<0>(neighbor));
              parents[std::get<0>(neighbor)] = curr_idx;
            } else 
            {
              if (cell_labels[std::get<0>(neighbor)] != curr_label * -1) 
              {
                std::cerr << "Odd cell cycle detected!" << std::endl;
                auto path = trace_parents(curr_idx);
                path.reverse();
                auto path2 = trace_parents(std::get<0>(neighbor));
                path.insert(path.end(), path2.begin(), path2.end());
                for (auto cell_id : path) 
                {
                  std::cout << cell_id << " ";
                  std::stringstream filename;
                  filename << "cell_" << cell_id << ".ply";
                  save_cell(filename.str(), cell_id);
                }
                std::cout << std::endl;
                valid = false;
              }
              // Do not fail when odd cycle is detected because the resulting
              // integer winding number field, although inconsistent, may still
              // be used if the problem region is local and embedded within a
              // valid volume.
              //assert(cell_labels[std::get<0>(neighbor)] == curr_label * -1);
            }
          }
        }
      }
    }
#ifdef PROPAGATE_WINDING_NUMBER_TIMING
    log_time("odd_cycle_check");
#endif
  }
#endif

  size_t outer_facet;
  bool flipped;
  Eigen::VectorXi I = igl::LinSpaced<Eigen::VectorXi>(num_faces, 0, num_faces-1);
  igl::copyleft::cgal::outer_facet(V, F, I, outer_facet, flipped);
#ifdef PROPAGATE_WINDING_NUMBER_TIMING
  log_time("outer_facet");
#endif

  const size_t outer_patch = P[outer_facet];
  const size_t infinity_cell = C(outer_patch, flipped?1:0);

  Eigen::VectorXi patch_labels(num_patches);
  const int INVALID = std::numeric_limits<int>::max();
  patch_labels.setConstant(INVALID);
  for (size_t i=0; i<num_faces; i++) {
    if (patch_labels[P[i]] == INVALID) {
      patch_labels[P[i]] = labels[i];
    } else {
      assert(patch_labels[P[i]] == labels[i]);
    }
  }
  assert((patch_labels.array() != INVALID).all());
  const size_t num_labels = patch_labels.maxCoeff()+1;

  Eigen::MatrixXi per_cell_W(num_cells, num_labels);
  per_cell_W.setConstant(INVALID);
  per_cell_W.row(infinity_cell).setZero();
  std::queue<size_t> Q;
  Q.push(infinity_cell);
  while (!Q.empty()) {
    size_t curr_cell = Q.front();
    Q.pop();
    for (const auto& neighbor : cell_adj[curr_cell]) {
      size_t neighbor_cell, patch_idx;
      bool direction;
      std::tie(neighbor_cell, direction, patch_idx) = neighbor;
      if ((per_cell_W.row(neighbor_cell).array() == INVALID).any()) {
        per_cell_W.row(neighbor_cell) = per_cell_W.row(curr_cell);
        for (size_t i=0; i<num_labels; i++) {
          int inc = (patch_labels[patch_idx] == (int)i) ?
            (direction ? -1:1) :0;
          per_cell_W(neighbor_cell, i) =
            per_cell_W(curr_cell, i) + inc;
        }
        Q.push(neighbor_cell);
      } else {
#ifndef NDEBUG
        // Checking for winding number consistency.
        // This check would inevitably fail for meshes that contain open
        // boundary or non-orientable.  However, the inconsistent winding number
        // field would still be useful in some cases such as when problem region
        // is local and embedded within the volume.  This, unfortunately, is the
        // best we can do because the problem of computing integer winding
        // number is ill-defined for open and non-orientable surfaces.
        for (size_t i=0; i<num_labels; i++) {
          if ((int)i == patch_labels[patch_idx]) {
            int inc = direction ? -1:1;
            //assert(per_cell_W(neighbor_cell, i) ==
            //    per_cell_W(curr_cell, i) + inc);
          } else {
            //assert(per_cell_W(neighbor_cell, i) ==
            //    per_cell_W(curr_cell, i));
          }
        }
#endif
      }
    }
  }
#ifdef PROPAGATE_WINDING_NUMBER_TIMING
  log_time("propagate_winding_number");
#endif

  W.resize(num_faces, num_labels*2);
  for (size_t i=0; i<num_faces; i++) 
  {
    const size_t patch = P[i];
    const size_t positive_cell = C(patch, 0);
    const size_t negative_cell = C(patch, 1);
    for (size_t j=0; j<num_labels; j++) {
      W(i,j*2  ) = per_cell_W(positive_cell, j);
      W(i,j*2+1) = per_cell_W(negative_cell, j);
    }
  }
#ifdef PROPAGATE_WINDING_NUMBER_TIMING
  log_time("store_result");
#endif
  return valid;
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template bool igl::copyleft::cgal::propagate_winding_numbers<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, unsigned long, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 1, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<std::vector<unsigned long, std::allocator<unsigned long> >, std::allocator<std::vector<unsigned long, std::allocator<unsigned long> > > > const&, unsigned long, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, unsigned long, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
// generated by autoexplicit.sh
template bool igl::copyleft::cgal::propagate_winding_numbers<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, unsigned long, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 1, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<std::vector<unsigned long, std::allocator<unsigned long> >, std::allocator<std::vector<unsigned long, std::allocator<unsigned long> > > > const&, unsigned long, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, unsigned long, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::copyleft::cgal::propagate_winding_numbers<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#ifdef WIN32
template bool igl::copyleft::cgal::propagate_winding_numbers<class Eigen::Matrix<class CGAL::Lazy_exact_nt<class CGAL::Gmpq>, -1, -1, 1, -1, -1>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>, unsigned __int64, class Eigen::Matrix<int, -1, 1, 0, -1, 1>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>, class Eigen::Matrix<int, -1, 1, 0, -1, 1>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>>(class Eigen::PlainObjectBase<class Eigen::Matrix<class CGAL::Lazy_exact_nt<class CGAL::Gmpq>, -1, -1, 1, -1, -1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> const &, class std::vector<class std::vector<unsigned __int64, class std::allocator<unsigned __int64>>, class std::allocator<class std::vector<unsigned __int64, class std::allocator<unsigned __int64>>>> const &, unsigned __int64, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, 1, 0, -1, 1>> const &, unsigned __int64, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, 1, 0, -1, 1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> &);
template bool igl::copyleft::cgal::propagate_winding_numbers<class Eigen::Matrix<class CGAL::Lazy_exact_nt<class CGAL::Gmpq>, -1, -1, 1, -1, -1>, class Eigen::Matrix<int, -1, 3, 1, -1, 3>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>, unsigned __int64, class Eigen::Matrix<int, -1, 1, 0, -1, 1>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>, class Eigen::Matrix<int, -1, 1, 0, -1, 1>, class Eigen::Matrix<int, -1, -1, 0, -1, -1>>(class Eigen::PlainObjectBase<class Eigen::Matrix<class CGAL::Lazy_exact_nt<class CGAL::Gmpq>, -1, -1, 1, -1, -1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, 3, 1, -1, 3>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> const &, class std::vector<class std::vector<unsigned __int64, class std::allocator<unsigned __int64>>, class std::allocator<class std::vector<unsigned __int64, class std::allocator<unsigned __int64>>>> const &, unsigned __int64, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, 1, 0, -1, 1>> const &, unsigned __int64, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, 1, 0, -1, 1>> const &, class Eigen::PlainObjectBase<class Eigen::Matrix<int, -1, -1, 0, -1, -1>> &);
#endif
#endif
