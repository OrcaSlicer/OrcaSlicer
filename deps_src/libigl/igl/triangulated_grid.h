// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2018 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_TRIANGULATEGRID_H
#define IGL_TRIANGULATEGRID_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Create a regular grid of elements (only 2D supported, currently) Vertex
  /// position order is compatible with `igl::grid`
  ///
  /// @param[in] nx  number of vertices in the x direction
  /// @param[in] ny  number of vertices in the y direction
  /// @param[out] GV  nx*ny by 2 list of mesh vertex positions.
  /// @param[out] GF  2*(nx-1)*(ny-1) by 3  list of triangle indices
  ///
  /// \see grid, quad_grid
  template <
    typename XType,
    typename YType,
    typename DerivedGV,
    typename DerivedGF>
  IGL_INLINE void triangulated_grid(
    const XType & nx,
    const YType & ny,
    Eigen::PlainObjectBase<DerivedGV> & GV,
    Eigen::PlainObjectBase<DerivedGF> & GF);
  /// \overload
  template <
    typename XType,
    typename YType,
    typename DerivedGF>
  IGL_INLINE void triangulated_grid(
    const XType & nx,
    const YType & ny,
    Eigen::PlainObjectBase<DerivedGF> & GF);
}
#ifndef IGL_STATIC_LIBRARY
#  include "triangulated_grid.cpp"
#endif
#endif 

