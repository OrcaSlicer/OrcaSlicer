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
  // Project each triangle to the plane
  //
  // [U,UF,I] = project_isometrically_to_plane(V,F)
  //
  // Inputs:
  //   V  #V by 3 list of vertex positions
  //   F  #F by 3 list of mesh indices
  // Outputs:
  //   U  #F*3 by 2 list of triangle positions
  //   UF  #F by 3 list of mesh indices into U
  //   I  #V by #F such that I(i,j) = 1 implies U(j,:) corresponds to V(i,:)
  //
  template <
    typename DerivedV, 
    typename DerivedF,
    typename DerivedU,
    typename DerivedUF,
    typename Scalar>
  IGL_INLINE void project_isometrically_to_plane(
    const Eigen::PlainObjectBase<DerivedV> & V, 
    const Eigen::PlainObjectBase<DerivedF> & F, 
    Eigen::PlainObjectBase<DerivedU> & U,
    Eigen::PlainObjectBase<DerivedUF> & UF, 
    Eigen::SparseMatrix<Scalar>& I);
}

#ifndef IGL_STATIC_LIBRARY
#  include "project_isometrically_to_plane.cpp"
#endif

#endif

