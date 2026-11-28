// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MASSMATRIX_INTRINSIC_H
#define IGL_MASSMATRIX_INTRINSIC_H
#include "igl_inline.h"
#include "massmatrix.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>

namespace igl 
{
  /// Constructs the mass (area) matrix for a given mesh (V,F).
  ///
  /// @param[in] l  #l by simplex_size list of mesh edge lengths
  /// @param[in] F  #F by simplex_size list of mesh elements (triangles or tetrahedra)
  /// @param[in] type  one of the following ints:
  ///     MASSMATRIX_TYPE_BARYCENTRIC  barycentric
  ///     MASSMATRIX_TYPE_VORONOI voronoi-hybrid {default}
  ///     MASSMATRIX_TYPE_FULL full
  /// @param[out] M  #V by #V mass matrix
  ///
  /// \see massmatrix
  ///
  template <typename Derivedl, typename DerivedF, typename Scalar>
  IGL_INLINE void massmatrix_intrinsic(
    const Eigen::MatrixBase<Derivedl> & l, 
    const Eigen::MatrixBase<DerivedF> & F, 
    const MassMatrixType type,
    Eigen::SparseMatrix<Scalar>& M);
  /// \overload
  /// @param[in] n  number of vertices (>= F.maxCoeff()+1)
  template <typename Derivedl, typename DerivedF, typename Scalar>
  IGL_INLINE void massmatrix_intrinsic(
    const Eigen::MatrixBase<Derivedl> & l, 
    const Eigen::MatrixBase<DerivedF> & F, 
    const MassMatrixType type,
    const int n,
    Eigen::SparseMatrix<Scalar>& M);
}

#ifndef IGL_STATIC_LIBRARY
#  include "massmatrix_intrinsic.cpp"
#endif

#endif


