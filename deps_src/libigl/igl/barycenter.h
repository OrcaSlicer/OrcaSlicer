// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_BARYCENTER_H
#define IGL_BARYCENTER_H
#include "igl_inline.h"
#include <Eigen/Dense>
namespace igl
{
  // Computes the barycenter of every simplex
  //
  // Inputs:
  //   V  #V x dim matrix of vertex coordinates
  //   F  #F x simplex_size  matrix of indices of simplex corners into V
  // Output:
  //   BC  #F x dim matrix of 3d vertices
  //
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedBC>
  IGL_INLINE void barycenter(
      const Eigen::MatrixBase<DerivedV> & V,
      const Eigen::MatrixBase<DerivedF> & F,
      Eigen::PlainObjectBase<DerivedBC> & BC);
}

#ifndef IGL_STATIC_LIBRARY
#  include "barycenter.cpp"
#endif

#endif
