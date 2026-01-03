// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PER_VERTEX_ATTRIBUTE_SMOOTHING_H
#define IGL_PER_VERTEX_ATTRIBUTE_SMOOTHING_H
#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  /// Smooth vertex attributes using uniform Laplacian
  ///
  /// @param[in] Ain  #V by #A eigen Matrix of mesh vertex attributes (each vertex has #A attributes)
  /// @param[in] F    #F by 3 eigne Matrix of face (triangle) indices
  /// @param[out] Aout #V by #A eigen Matrix of mesh vertex attributes
  template <typename DerivedV, typename DerivedF>
  IGL_INLINE void per_vertex_attribute_smoothing(
    const Eigen::MatrixBase<DerivedV>& Ain,
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedV> & Aout);
}

#ifndef IGL_STATIC_LIBRARY
#  include "per_vertex_attribute_smoothing.cpp"
#endif

#endif
