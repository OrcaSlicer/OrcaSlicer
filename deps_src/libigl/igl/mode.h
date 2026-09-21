// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MODE_H
#define IGL_MODE_H
#include "igl_inline.h"
#include <Eigen/Dense>
namespace igl
{
  /// Takes mode of coefficients in a matrix along a given dimension
  ///
  /// @tparam T  should be a eigen matrix primitive type like int or double
  /// @param[in] X  m by n original matrix
  /// @param[in] d  dension along which to take mode, m or n
  /// @param[out] M  vector containing mode along dension d, if d==1 then this will be a
  ///     n-long vector if d==2 then this will be a m-long vector
  template <typename T>
  IGL_INLINE void mode(
    const Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> & X,
    const int d, 
    Eigen::Matrix<T,Eigen::Dynamic,1> & M);
}

#ifndef IGL_STATIC_LIBRARY
#  include "mode.cpp"
#endif

#endif
