// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EDGES_H
#define IGL_EDGES_H
#include "igl_inline.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>

namespace igl
{
  /// Constructs a list of unique edges represented in a given mesh (V,F)
  ///
  /// @param[in] F  #F by (3|4) list of mesh simplex indices
  /// @param[out] E #E by 2 list of edges in no particular order
  ///
  /// \see adjacency_matrix
  template <typename DerivedF, typename DerivedE>
  IGL_INLINE void edges(
    const Eigen::MatrixBase<DerivedF> & F, 
    Eigen::PlainObjectBase<DerivedE> & E);
  /// Constructs a list of unique edges represented in a given polygon mesh.
  ///
  /// @param[in] I  #I vectorized list of polygon corner indices into rows of some matrix V
  /// @param[in] C  #polygons+1 list of cumulative polygon sizes so that C(i+1)-C(i) =
  ///     size of the ith polygon, and so I(C(i)) through I(C(i+1)-1) are the
  ///     indices of the ith polygon
  /// @param[out] E #E by 2 list of edges in no particular order
  template <typename DerivedI, typename DerivedC, typename DerivedE>
  IGL_INLINE void edges(
    const Eigen::MatrixBase<DerivedI> & I,
    const Eigen::MatrixBase<DerivedC> & C,
    Eigen::PlainObjectBase<DerivedE> & E);
  /// Constructs a list of unique edges represented in a given adjacency matrix.
  ///
  /// @param[in] A  #V by #V symmetric adjacency matrix
  /// @param[out] E  #E by 2 list of edges in no particular order
  template <typename T, typename DerivedE>
  IGL_INLINE void edges(
    const Eigen::SparseMatrix<T> & A,
    Eigen::PlainObjectBase<DerivedE> & E);
}

#ifndef IGL_STATIC_LIBRARY
#  include "edges.cpp"
#endif

#endif
