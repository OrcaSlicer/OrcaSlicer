// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EPS_H
#define IGL_EPS_H
#include "igl_inline.h"
namespace igl
{
  /// Standard value for double epsilon
  const double DOUBLE_EPS    = 1.0e-14;
  /// Standard value for double epsilon²
  const double DOUBLE_EPS_SQ = 1.0e-28;
  /// Standard value for single epsilon
  const float FLOAT_EPS    = 1.0e-7f;
  /// Standard value for single epsilon²
  const float FLOAT_EPS_SQ = 1.0e-14f;
  /// Function returning EPS for corresponding type
  template <typename S_type> IGL_INLINE S_type EPS();
  /// Function returning EPS_SQ for corresponding type
  template <typename S_type> IGL_INLINE S_type EPS_SQ();
  // Template specializations for float and double
  template <> IGL_INLINE float EPS<float>();
  template <> IGL_INLINE double EPS<double>();
  template <> IGL_INLINE float EPS_SQ<float>();
  template <> IGL_INLINE double EPS_SQ<double>();
}

#ifndef IGL_STATIC_LIBRARY
#  include "EPS.cpp"
#endif

#endif
