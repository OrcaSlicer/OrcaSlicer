// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_GAUSSIAN_CURVATURE_H
#define IGL_GAUSSIAN_CURVATURE_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  // Compute discrete local integral gaussian curvature (angle deficit, without
  // averaging by local area).
  //
  // Inputs:
  //   V  #V by 3 eigen Matrix of mesh vertex 3D positions
  //   F  #F by 3 eigen Matrix of face (triangle) indices
  // Output:
  //   K  #V by 1 eigen Matrix of discrete gaussian curvature values
  //
  template <typename DerivedV, typename DerivedF, typename DerivedK>
  IGL_INLINE void gaussian_curvature(
    const Eigen::PlainObjectBase<DerivedV>& V,
    const Eigen::PlainObjectBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedK> & K);
}

#ifndef IGL_STATIC_LIBRARY
#  include "gaussian_curvature.cpp"
#endif

#endif

