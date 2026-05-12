// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MVC_H
#define IGL_MVC_H

#include "igl_inline.h"
#include <Eigen/Dense>

namespace igl 
{
  /// Mean value coordinates for a polygon
  /// 
  /// @param[in] V  #V x dim list of vertex positions (dim = 2 or dim = 3)
  /// @param[in] C  #C x dim list of polygon vertex positions in counter-clockwise order
  ///     (dim = 2 or dim = 3)
  /// 
  /// @param[out] W  weights, #V by #C matrix of weights
  /// 
  /// \bug implementation is listed as "Broken"
  IGL_INLINE void mvc(
    const Eigen::MatrixXd &V, 
    const Eigen::MatrixXd &C, 
    Eigen::MatrixXd &W);
}

#ifndef IGL_STATIC_LIBRARY
#  include "mvc.cpp"
#endif

#endif
