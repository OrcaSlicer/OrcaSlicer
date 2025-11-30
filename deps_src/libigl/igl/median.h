// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MEDIAN_H
#define IGL_MEDIAN_H
#include "igl_inline.h"
#include <Eigen/Dense>
namespace igl
{
  /// Compute the median of an eigen vector
  ///
  /// @param[in] V  #V list of unsorted values
  /// @param[out] m  median of those values
  /// @return true on success, false on failure
  template <typename DerivedV, typename mType>
  IGL_INLINE bool median(
    const Eigen::MatrixBase<DerivedV> & V, mType & m);
}

#ifndef IGL_STATIC_LIBRARY
#  include "median.cpp"
#endif

#endif
