// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_RANDOM_QUATERNION_H
#define IGL_RANDOM_QUATERNION_H
#include "igl_inline.h"
#include <Eigen/Geometry>
namespace igl
{
  // Return a random quaternion via uniform sampling of the 4-sphere
  template <typename Scalar>
  IGL_INLINE Eigen::Quaternion<Scalar> random_quaternion();
}
#ifndef IGL_STATIC_LIBRARY
#include "random_quaternion.cpp"
#endif
#endif
