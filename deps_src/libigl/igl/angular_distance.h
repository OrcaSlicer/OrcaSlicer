// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ANGULAR_DISTANCE_H
#define IGL_ANGULAR_DISTANCE_H
#include "igl_inline.h"
#include <Eigen/Geometry>
namespace igl
{
  // The "angular distance" between two unit quaternions is the angle of the
  // smallest rotation (treated as an Axis and Angle) that takes A to B.
  //
  // Inputs:
  //   A  unit quaternion
  //   B  unit quaternion
  // Returns angular distance
  IGL_INLINE double angular_distance(
    const Eigen::Quaterniond & A,
    const Eigen::Quaterniond & B);
}

#ifndef IGL_STATIC_LIBRARY
#include "angular_distance.cpp"
#endif

#endif
