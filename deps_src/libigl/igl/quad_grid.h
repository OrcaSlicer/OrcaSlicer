// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2019 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_QUAD_GRID_H
#define IGL_QUAD_GRID_H

#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  // Generate a quad mesh over a regular grid.
  //
  // @param[in] nx  number of vertices in the x direction
  // @param[in] ny  number of vertices in the y direction
  // @param[out] V  nx*ny by 2 list of vertex positions
  // @param[out] Q  (nx-1)*(ny-1) by 4 list of quad indices into V
  // @param[out] E  (nx-1)*ny+(ny-1)*nx by 2 list of undirected quad edge indices into V
  //
  // \see grid, triangulated_grid
  template<
    typename DerivedV,
    typename DerivedQ,
    typename DerivedE>
  IGL_INLINE void quad_grid(
    const int nx,
    const int ny,
    Eigen::PlainObjectBase<DerivedV> & V,
    Eigen::PlainObjectBase<DerivedQ> & Q,
    Eigen::PlainObjectBase<DerivedE> & E);
  /// \overload
  template<
    typename DerivedQ,
    typename DerivedE>
  IGL_INLINE void quad_grid(
    const int nx,
    const int ny,
    Eigen::PlainObjectBase<DerivedQ> & Q,
    Eigen::PlainObjectBase<DerivedE> & E);
}

#ifndef IGL_STATIC_LIBRARY
#  include "quad_grid.cpp"
#endif
#endif
