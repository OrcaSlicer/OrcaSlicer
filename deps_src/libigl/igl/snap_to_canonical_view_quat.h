// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SNAP_TO_CANONICAL_VIEW_QUAT_H
#define IGL_SNAP_TO_CANONICAL_VIEW_QUAT_H
#include "igl_inline.h"
#include <Eigen/Geometry>
// A Quaternion, q, is defined here as an arrays of four scalars (x,y,z,w),
// such that q = x*i + y*j + z*k + w
namespace igl
{
  // Snap the quaternion q to the nearest canonical view quaternion
  // Input:
  //   q  quaternion to be snapped (also see Outputs)
  //   threshold  (optional) threshold:
  //     1.0 --> snap any input
  //     0.5 --> snap inputs somewhat close to canonical views
  //     0.0 --> snap no input
  // Output:
  //   q  quaternion possibly set to nearest canonical view
  // Return:
  //   true only if q was snapped to the nearest canonical view
  template <typename Q_type>
  IGL_INLINE bool snap_to_canonical_view_quat(
    const Q_type* q,
    const Q_type threshold,
    Q_type* s);

  template <typename Scalarq, typename Scalars>
  IGL_INLINE bool snap_to_canonical_view_quat(
    const Eigen::Quaternion<Scalarq> & q,
    const double threshold,
    Eigen::Quaternion<Scalars> & s);
}

#ifndef IGL_STATIC_LIBRARY
#  include "snap_to_canonical_view_quat.cpp"
#endif

#endif
