// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_CIRCUMRADIUS_H
#define IGL_CIRCUMRADIUS_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  // Compute the circumradius of each triangle in a mesh (V,F)
  //
  // Inputs:
  //   V  #V by dim list of mesh vertex positions
  //   F  #F by 3 list of triangle indices into V
  // Outputs:
  //   R  #F list of circumradii
  //
  template <
    typename DerivedV, 
    typename DerivedF,
    typename DerivedR>
  IGL_INLINE void circumradius(
    const Eigen::PlainObjectBase<DerivedV> & V, 
    const Eigen::PlainObjectBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedR> & R);
}
#ifndef IGL_STATIC_LIBRARY
#  include "circumradius.cpp"
#endif
#endif
