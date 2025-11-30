// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2018 Francis Williams <francis@fwilliams.info>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SPARSE_VOXEL_GRID_H
#define IGL_SPARSE_VOXEL_GRID_H

#include "igl_inline.h"
#include <Eigen/Core>

namespace igl 
{
  /// Given a point, p0, on an isosurface, construct a shell of epsilon sized cubes surrounding the surface.
  /// These cubes can be used as the input to marching cubes.
  ///
  /// @param[in] p0  A 3D point on the isosurface surface defined by scalarFunc(x) = 0
  /// @param[in] scalarFunc  A scalar function from R^3 to R -- points which map to 0 lie
  ///   on the surface, points which are negative lie inside the surface,
  ///   and points which are positive lie outside the surface
  /// @param[in] eps  The edge length of the cubes surrounding the surface
  /// @param[in] expected_number_of_cubes  This pre-allocates internal data structures to speed things up
  /// @param[out] CS  #CV by 1 list of scalar values at the cube vertices
  /// @param[out] CV  #CV by 3 list of cube vertex positions
  /// @param[out] CI  #CI by 8 list of cube indices into rows of CS and CV. Each row
  ///   represents 8 corners of cube in y-x-z binary counting order.
  ///
  template <
    typename DerivedP0, 
    typename Func, 
    typename DerivedS, 
    typename DerivedV, 
    typename DerivedI>
  IGL_INLINE void sparse_voxel_grid(
    const Eigen::MatrixBase<DerivedP0>& p0,
    const Func& scalarFunc,
    const double eps,
    const int expected_number_of_cubes,
    Eigen::PlainObjectBase<DerivedS>& CS,
    Eigen::PlainObjectBase<DerivedV>& CV,
    Eigen::PlainObjectBase<DerivedI>& CI);
}

#ifndef IGL_STATIC_LIBRARY
#    include "sparse_voxel_grid.cpp"
#endif

#endif
