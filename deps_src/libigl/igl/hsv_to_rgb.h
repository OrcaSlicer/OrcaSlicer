// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_HSV_TO_RGB_H
#define IGL_HSV_TO_RGB_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Convert RGB to HSV
  ///
  /// @param[in] hsv with
  ///   - h hue value (degrees: [0,360]. Values outside this range will be mapped periodically to [0,360].)
  ///   - s  saturation value ([0,1])
  ///   - v  value value ([0,1])
  /// @param[out] rgb with:
  ///   - r  red value ([0,1]) 
  ///   - g  green value ([0,1])
  ///   - b  blue value ([0,1])
  template <typename T>
  IGL_INLINE void hsv_to_rgb(const T * hsv, T * rgb);
  /// \overload
  template <typename T>
  IGL_INLINE void hsv_to_rgb( 
    const T & h, const T & s, const T & v, 
    T & r, T & g, T & b);
  /// \overload
  ///
  /// @param[in] H #H by 3 list of hsv values
  /// @param[out] R #H by 3 list of rgb values
  template <typename DerivedH, typename DerivedR>
  void hsv_to_rgb(
    const Eigen::MatrixBase<DerivedH> & H,
    Eigen::PlainObjectBase<DerivedR> & R);
};

#ifndef IGL_STATIC_LIBRARY
#  include "hsv_to_rgb.cpp"
#endif

#endif

