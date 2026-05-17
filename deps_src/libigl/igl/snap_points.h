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
  /// Snap list of points C to closest of another list of points V
  ///
  /// @param[in] C  #C by dim list of query point positions
  /// @param[in] V  #V by dim list of data point positions
  /// @param[out] I  #C list of indices into V of closest points to C
  /// @param[out] minD  #C list of squared (^p) distances to closest points
  /// @param[out] VI  #C by dim list of new point positions, VI = V(I,:)
  template <
    typename DerivedC,
    typename DerivedV,
    typename DerivedI,
    typename DerivedminD,
    typename DerivedVI>
  IGL_INLINE void snap_points(
    const Eigen::MatrixBase<DerivedC > & C,
    const Eigen::MatrixBase<DerivedV > & V,
    Eigen::PlainObjectBase<DerivedI > & I,
    Eigen::PlainObjectBase<DerivedminD > & minD,
    Eigen::PlainObjectBase<DerivedVI > & VI);
  /// \overload
  template <
    typename DerivedC,
    typename DerivedV,
    typename DerivedI,
    typename DerivedminD>
  IGL_INLINE void snap_points(
    const Eigen::MatrixBase<DerivedC > & C,
    const Eigen::MatrixBase<DerivedV > & V,
    Eigen::PlainObjectBase<DerivedI > & I,
    Eigen::PlainObjectBase<DerivedminD > & minD);
  /// \overload
  template <
    typename DerivedC,
    typename DerivedV,
    typename DerivedI >
  IGL_INLINE void snap_points(
    const Eigen::MatrixBase<DerivedC > & C,
    const Eigen::MatrixBase<DerivedV > & V,
    Eigen::PlainObjectBase<DerivedI > & I);
}

#ifndef IGL_STATIC_LIBRARY
#  include "snap_points.cpp"
#endif

#endif




