// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2021 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MARCH_CUBE_H
#define IGL_MARCH_CUBE_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <unordered_map>
#include <cstdint>
namespace igl
{
  /// Process a single cube of a marching cubes grid.
  ///
  /// @param[in] GV  #GV by 3 list of grid vertex positions
  /// @param[in] cS  list of 8 scalar field values at grid corners
  /// @param[in] cI  list of 8 indices of corners into rows of GV
  /// @param[in] isovalue  level-set value being extracted (often 0)
  /// @param[in,out] V  #V by 3 current list of output mesh vertex positions
  /// @param[in,out] n  current number of mesh vertices (i.e., occupied rows in V)
  /// @param[in,out] F  #F by 3 current list of output mesh triangle indices into rows of V
  /// @param[in,out] m  current number of mesh triangles (i.e., occupied rows in F)
  /// @param[in,out] E2V  current edge (GV_i,GV_j) to vertex (V_k) map
  ///
  /// Side-effects: V,n,F,m,E2V are updated to contain new vertices and faces of
  /// any constructed mesh elements
  ///
  template <
    typename DerivedGV,
    typename Scalar,
    typename Index,
    typename DerivedV,
    typename DerivedF>
  IGL_INLINE void march_cube(
    const DerivedGV & GV,
    const Eigen::Matrix<Scalar,8,1> & cS,
    const Eigen::Matrix<Index,8,1> & cI,
    const Scalar & isovalue,
    Eigen::PlainObjectBase<DerivedV> &V,
    Index & n,
    Eigen::PlainObjectBase<DerivedF> &F,
    Index & m,
    std::unordered_map<std::int64_t,int> & E2V);
}

#ifndef IGL_STATIC_LIBRARY
#  include "march_cube.cpp"
#endif

#endif
