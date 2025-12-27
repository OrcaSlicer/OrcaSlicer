// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SNAP_POINTS_H
#define IGL_SNAP_POINTS_H

#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  // SNAP_POINTS snap list of points C to closest of another list of points V
  //
  // [I,minD,VI] = snap_points(C,V)
  // 
  // Inputs:
  //   C  #C by dim list of query point positions
  //   V  #V by dim list of data point positions
  // Outputs:
  //   I  #C list of indices into V of closest points to C
  //   minD  #C list of squared (^p) distances to closest points
  //   VI  #C by dim list of new point positions, VI = V(I,:)
  template <
    typename DerivedC, 
    typename DerivedV, 
    typename DerivedI, 
    typename DerivedminD, 
    typename DerivedVI>
  IGL_INLINE void snap_points(
    const Eigen::PlainObjectBase<DerivedC > & C,
    const Eigen::PlainObjectBase<DerivedV > & V,
    Eigen::PlainObjectBase<DerivedI > & I,
    Eigen::PlainObjectBase<DerivedminD > & minD,
    Eigen::PlainObjectBase<DerivedVI > & VI);
  template <
    typename DerivedC, 
    typename DerivedV, 
    typename DerivedI, 
    typename DerivedminD>
  IGL_INLINE void snap_points(
    const Eigen::PlainObjectBase<DerivedC > & C,
    const Eigen::PlainObjectBase<DerivedV > & V,
    Eigen::PlainObjectBase<DerivedI > & I,
    Eigen::PlainObjectBase<DerivedminD > & minD);
  template <
    typename DerivedC, 
    typename DerivedV, 
    typename DerivedI >
  IGL_INLINE void snap_points(
    const Eigen::PlainObjectBase<DerivedC > & C,
    const Eigen::PlainObjectBase<DerivedV > & V,
    Eigen::PlainObjectBase<DerivedI > & I);
}

#ifndef IGL_STATIC_LIBRARY
#  include "snap_points.cpp"
#endif

#endif




