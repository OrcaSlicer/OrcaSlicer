// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SNAP_TO_FIXED_UP_H
#define IGL_SNAP_TO_FIXED_UP_H

#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Geometry>

namespace igl
{
  // Snap an arbitrary rotation to a rotation resulting from a rotation about
  // the y-axis then the x-axis (maintaining fixed up like
  // two_axis_valuator_fixed_up.)
  //
  // Inputs:
  //   q  General rotation as quaternion
  // Outputs:
  //   s the resulting rotation (as quaternion)
  //
  // See also: two_axis_valuator_fixed_up
  template <typename Qtype>
  IGL_INLINE void snap_to_fixed_up(
    const Eigen::Quaternion<Qtype> & q,
    Eigen::Quaternion<Qtype> & s);
}

#ifndef IGL_STATIC_LIBRARY
#  include "snap_to_fixed_up.cpp"
#endif

#endif


