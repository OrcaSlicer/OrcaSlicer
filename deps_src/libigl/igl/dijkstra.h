// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Olga Diamanti <olga.diam@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_DIJKSTRA
#define IGL_DIJKSTRA
#include "igl_inline.h"

#include <Eigen/Core>
#include <vector>
#include <set>

namespace igl {

  /// Dijkstra's algorithm for vertex-weighted shortest paths, with multiple targets.
  /// Adapted from http://rosettacode.org/wiki/Dijkstra%27s_algorithm .
  ///
  /// @param[in] source           index of source vertex
  /// @param[in] targets          target vector set
  /// @param[in] VV               #V list of lists of incident vertices (adjacency list), e.g.
  ///                             as returned by igl::adjacency_list
  /// @param[in] weights          #V list of scalar vertex weights
  /// @param[out] min_distance     #V by 1 list of the minimum distances from source to all vertices
  /// @param[out] previous         #V by 1 list of the previous visited vertices (for each vertex) - used for backtracking
  ///
  template <typename IndexType, typename DerivedD, typename DerivedP>
  IGL_INLINE int dijkstra(
    const IndexType &source,
    const std::set<IndexType> &targets,
    const std::vector<std::vector<IndexType> >& VV,
    const std::vector<double>& weights,
    Eigen::PlainObjectBase<DerivedD> &min_distance,
    Eigen::PlainObjectBase<DerivedP> &previous);
  /// \overload
  template <typename IndexType, typename DerivedV,
  typename DerivedD, typename DerivedP>
  IGL_INLINE int dijkstra(
    const Eigen::MatrixBase<DerivedV> &V,
    const std::vector<std::vector<IndexType> >& VV,
    const IndexType &source,
    const std::set<IndexType> &targets,
    Eigen::PlainObjectBase<DerivedD> &min_distance,
    Eigen::PlainObjectBase<DerivedP> &previous);
  /// \overload
  template <typename IndexType, typename DerivedD, typename DerivedP>
  IGL_INLINE int dijkstra(
    const IndexType &source,
    const std::set<IndexType> &targets,
    const std::vector<std::vector<IndexType> >& VV,
    Eigen::PlainObjectBase<DerivedD> &min_distance,
    Eigen::PlainObjectBase<DerivedP> &previous);
  /// Backtracking after Dijkstra's algorithm, to find shortest path.
  ///
  /// @param[in] vertex           vertex to which we want the shortest path (from same source as above)
  /// @param[in] previous         #V by 1 list of the previous visited vertices (for each vertex) - result of Dijkstra's algorithm
  /// @param[out] path             #P by 1 list of vertex indices in the shortest path from vertex to source
  template <typename IndexType, typename DerivedP>
  IGL_INLINE void dijkstra(
    const IndexType &vertex,
    const Eigen::MatrixBase<DerivedP> &previous,
    std::vector<IndexType> &path);
}

#ifndef IGL_STATIC_LIBRARY
#include "dijkstra.cpp"
#endif


#endif /* defined(IGL_DIJKSTRA) */
