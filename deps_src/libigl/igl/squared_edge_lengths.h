// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SQUARED_EDGE_LENGTHS_H
#define IGL_SQUARED_EDGE_LENGTHS_H
#include "igl_inline.h"

#include <Eigen/Dense>

namespace igl
{
  /// Constructs a list of squared lengths of edges opposite each index in a face
  /// (triangle/tet) list
  ///
  /// @tparam DerivedV derived from vertex positions matrix type: i.e. MatrixXd
  /// @tparam DerivedF derived from face indices matrix type: i.e. MatrixXi
  /// @tparam DerivedL derived from edge lengths matrix type: i.e. MatrixXd
  /// @param[in] V  eigen matrix #V by 3
  /// @param[in] F  #F by (2|3|4) list of mesh edges, triangles or tets
  /// @param[out] L  #F by {1|3|6} list of edge lengths squared
  ///     for edges, column of lengths
  ///     for triangles, columns correspond to edges [1,2],[2,0],[0,1]
  ///     for tets, columns correspond to edges
  ///     [3 0],[3 1],[3 2],[1 2],[2 0],[0 1]
  ///
  template <typename DerivedV, typename DerivedF, typename DerivedL>
  IGL_INLINE void squared_edge_lengths(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedL>& L);
}

#ifndef IGL_STATIC_LIBRARY
#  include "squared_edge_lengths.cpp"
#endif

#endif

