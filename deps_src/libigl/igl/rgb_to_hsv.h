// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_RGB_TO_HSV_H
#define IGL_RGB_TO_HSV_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  // Convert RGB to HSV
  //
  // Inputs:
  //   r  red value ([0,1]) 
  //   g  green value ([0,1])
  //   b  blue value ([0,1])
  // Outputs:
  //   h  hue value (degrees: [0,360])
  //   s  saturation value ([0,1])
  //   v  value value ([0,1])
  template <typename R,typename H>
  IGL_INLINE void rgb_to_hsv(const R * rgb, H * hsv);
  template <typename DerivedR,typename DerivedH>
  IGL_INLINE void rgb_to_hsv(
    const Eigen::PlainObjectBase<DerivedR> & R,
    Eigen::PlainObjectBase<DerivedH> & H);
};

#ifndef IGL_STATIC_LIBRARY
#  include "rgb_to_hsv.cpp"
#endif

#endif

