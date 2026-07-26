// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Francisca Gil Ureta <gilureta@cs.nyu.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SEGMENT_SEGMENT_INTERSECT_H
#define IGL_SEGMENT_SEGMENT_INTERSECT_H

#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Determine whether two line segments A,B intersect
  ///
  /// A: p + t*r :  t \in [0,1]
  /// B: q + u*s :  u \in [0,1]
  ///
  /// @param[in] p  3-vector origin of segment A
  /// @param[in] r  3-vector direction of segment A
  /// @param[in] q  3-vector origin of segment B
  /// @param[in] s  3-vector direction of segment B
  /// @param[out] t  scalar point of intersection along segment A, t \in [0,1]
  /// @param[out] u  scalar point of intersection along segment B, u \in [0,1]
  /// @param[in] eps precision
  /// @return true if intersection
  template<typename DerivedSource, typename DerivedDir>
  IGL_INLINE bool segment_segment_intersect(
    const Eigen::MatrixBase <DerivedSource> &p,
    const Eigen::MatrixBase <DerivedDir> &r,
    const Eigen::MatrixBase <DerivedSource> &q,
    const Eigen::MatrixBase <DerivedDir> &s,
    double &t,
    double &u,
    double eps = 1e-6);
}

#ifndef IGL_STATIC_LIBRARY
#   include "segment_segment_intersect.cpp"
#endif
#endif
