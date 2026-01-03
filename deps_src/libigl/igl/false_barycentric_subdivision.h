// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ADD_BARYCENTER_H
#define IGL_ADD_BARYCENTER_H
#include "igl_inline.h"

#include <Eigen/Dense>

namespace igl
{
  /// Refine the mesh by adding the barycenter of each face
  ///
  /// @param[in] V       #V by 3 coordinates of the vertices
  /// @param[in] F       #F by 3 list of mesh faces (must be triangles)
  /// @param[out] VD      #V + #F by 3 coordinate of the vertices of the dual mesh
  ///           The added vertices are added at the end of VD (should not be
  ///           same references as (V,F)
  /// @param[out] FD      #F*3 by 3 faces of the dual mesh
  template <typename DerivedV, typename DerivedF, typename DerivedVD, typename DerivedFD>
  IGL_INLINE void false_barycentric_subdivision(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedVD> & VD,
    Eigen::PlainObjectBase<DerivedFD> & FD);

}

#ifndef IGL_STATIC_LIBRARY
#  include "false_barycentric_subdivision.cpp"
#endif

#endif
