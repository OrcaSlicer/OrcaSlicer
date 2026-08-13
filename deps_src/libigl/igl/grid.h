// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// Copyright (C) 2018 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_GRID_H
#define IGL_GRID_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Construct vertices of a regular grid, suitable for input to
  /// `igl::marching_cubes`
  ///
  /// @param[in] res  #res list of number of vertices along each dimension filling a unit
  ///     #res-cube
  /// @param[out] GV  res.array().prod() by #res list of mesh vertex positions.
  ///   
  /// \see triangulated_grid, quad_grid
  template <
    typename Derivedres,
    typename DerivedGV>
  IGL_INLINE void grid(
    const Eigen::MatrixBase<Derivedres> & res, 
    Eigen::PlainObjectBase<DerivedGV> & GV);
}
#ifndef IGL_STATIC_LIBRARY
#  include "grid.cpp"
#endif
#endif 
