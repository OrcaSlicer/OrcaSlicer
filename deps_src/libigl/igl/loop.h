// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Oded Stein <oded.stein@columbia.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_LOOP_H
#define IGL_LOOP_H

#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Sparse>

namespace igl
{
  /// Given the triangle mesh [V, F], where n_verts = V.rows(), computes
  /// newV and a sparse matrix S s.t. [newV, newF] is the subdivided mesh where
  /// newV = S*V.
  ///
  /// @param[in] n_verts  an integer (number of mesh vertices)
  /// @param[in] F  an m by 3 matrix of integers of triangle faces
  /// @param[out] S  a sparse matrix (will become the subdivision matrix)
  /// @param[out] newF  a matrix containing the new faces
  template <
    typename DerivedF,
    typename SType,
    typename DerivedNF>
  IGL_INLINE void loop(
    const int n_verts,
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::SparseMatrix<SType>& S,
    Eigen::PlainObjectBase<DerivedNF> & NF);
  /// Given the triangle mesh [V, F], computes number_of_subdivs steps of loop subdivision and outputs the new mesh [newV, newF]
  ///
  /// @param[in] V an n by 3 matrix of vertices
  /// @param[in] F an m by 3 matrix of integers of triangle faces
  /// @param[out] NV a matrix containing the new vertices
  /// @param[out] NF a matrix containing the new faces
  /// @param[in] number_of_subdivs an integer that specifies how many subdivision steps to do
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedNV,
    typename DerivedNF>
  IGL_INLINE void loop(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedNV>& NV,
    Eigen::PlainObjectBase<DerivedNF>& NF,
    const int number_of_subdivs = 1);
}

#ifndef IGL_STATIC_LIBRARY
#  include "loop.cpp"
#endif

#endif
