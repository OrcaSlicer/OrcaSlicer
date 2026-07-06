// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_UPSAMPLE_H
#define IGL_UPSAMPLE_H
#include "igl_inline.h"

#include <Eigen/Core>
#include <Eigen/Sparse>

// History:
//  changed templates from generic matrices to PlainObjectBase Alec May 7, 2011
namespace igl
{
  /// Subdivide without moving vertices: Given the triangle mesh [V, F],
  /// where n_verts = V.rows(), computes newV and a sparse matrix S s.t.
  /// [newV, newF] is the subdivided mesh where newV = S*V.
  ///
  /// @param[in] n_verts  an integer (number of mesh vertices)
  /// @param[in] F  an m by 3 matrix of integers of triangle faces
  /// @param[out] S  a sparse matrix (will become the subdivision matrix)
  /// @param[out] newF  a matrix containing the new faces
  template <
    typename DerivedF,
    typename SType,
    typename DerivedNF>
  IGL_INLINE void upsample(
    const int n_verts,
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::SparseMatrix<SType>& S,
    Eigen::PlainObjectBase<DerivedNF>& NF);
  /// Subdivide a mesh without moving vertices: loop subdivision but odd
  /// vertices stay put and even vertices are just edge midpoints
  ///
  /// @tparam MatV  matrix for vertex positions, e.g. MatrixXd
  /// @tparam MatF  matrix for vertex positions, e.g. MatrixXi
  /// @param[in] V  #V by dim  mesh vertices
  /// @param[in] F  #F by 3  mesh triangles
  /// @param[out] NV new vertex positions, V is guaranteed to be at top
  /// @param[out] NF new list of face indices
  ///
  /// \note V should not be the same as NV,
  /// \note F should not be the same as NF, use other proto
  ///
  /// \pre assumes (V,F) is edge-manifold.
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedNV,
    typename DerivedNF>
  IGL_INLINE void upsample(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedNV>& NV,
    Eigen::PlainObjectBase<DerivedNF>& NF,
    const int number_of_subdivs = 1);

  // Virtually in place wrapper
  template <
    typename MatV,
    typename MatF>
  IGL_INLINE void upsample(
    MatV& V,
    MatF& F,
    const int number_of_subdivs = 1);
}

#ifndef IGL_STATIC_LIBRARY
#  include "upsample.cpp"
#endif

#endif
