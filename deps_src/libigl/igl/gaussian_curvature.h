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
  /// Compute the 2π minus the sum of interior angles at each vertex. For
  /// interior vertices of a manifold mesh this corresponds to the local
  /// integral gaussian curvature ("angle deficit", without averaging by local
  /// area). For boundary vertices, this quantity is not so meaninful. FWIW,
  /// adding π to the output for boundary vertices would produce local integral
  /// geodesic curvature along the boundary curve.
  ///
  /// @param[in] V  #V by 3 eigen Matrix of mesh vertex 3D positions
  /// @param[in] F  #F by 3 eigen Matrix of face (triangle) indices
  /// @param[out] K  #V by 1 eigen Matrix of discrete gaussian curvature values
  ///
  template <typename DerivedV, typename DerivedF, typename DerivedK>
  IGL_INLINE void gaussian_curvature(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedK> & K);
}

#ifndef IGL_STATIC_LIBRARY
#  include "gaussian_curvature.cpp"
#endif

#endif

