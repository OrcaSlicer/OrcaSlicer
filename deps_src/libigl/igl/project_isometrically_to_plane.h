// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PROJECT_ISOMETRICALLY_TO_PLANE_H
#define IGL_PROJECT_ISOMETRICALLY_TO_PLANE_H
#include "igl_inline.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>


namespace igl
{
  /// Project each triangle to the plane
  ///
  /// @param[in] V  #V by 3 list of vertex positions
  /// @param[in] F  #F by 3 list of mesh indices
  /// @param[out] U  #F*3 by 2 list of triangle positions
  /// @param[out] UF  #F by 3 list of mesh indices into U
  /// @param[out] I  #V by #F*3 such that I(i,j) = 1 implies U(j,:) corresponds to V(i,:)
  ///
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedU,
    typename DerivedUF,
    typename Scalar>
  IGL_INLINE void project_isometrically_to_plane(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedU> & U,
    Eigen::PlainObjectBase<DerivedUF> & UF,
    Eigen::SparseMatrix<Scalar>& I);
}

#ifndef IGL_STATIC_LIBRARY
#  include "project_isometrically_to_plane.cpp"
#endif

#endif

