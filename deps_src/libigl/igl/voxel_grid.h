// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_VOXEL_GRID_H
#define IGL_VOXEL_GRID_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Geometry>
namespace igl
{
  // Construct the cell center positions of a regular voxel grid (lattice) made
  // of perfectly square voxels.
  // 
  // Inputs:
  //   box  bounding box to enclose by grid
  //   s  number of cell centers on largest side (including 2*pad_count)
  //   pad_count  number of cells beyond box
  // Outputs:
  //   GV  side(0)*side(1)*side(2) by 3 list of cell center positions
  //   side  3-long list of dimension of voxel grid
  template <
    typename Scalar,
    typename DerivedGV,
    typename Derivedside>
  IGL_INLINE void voxel_grid(
    const Eigen::AlignedBox<Scalar,3> & box, 
    const int s,
    const int pad_count,
    Eigen::PlainObjectBase<DerivedGV> & GV,
    Eigen::PlainObjectBase<Derivedside> & side);
}
#ifndef IGL_STATIC_LIBRARY
#  include "voxel_grid.cpp"
#endif
#endif
