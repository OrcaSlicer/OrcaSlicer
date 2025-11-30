// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_IS_PLANAR_H
#define IGL_IS_PLANAR_H
#include "igl_inline.h"

#include <Eigen/Dense>

namespace igl
{
  /// Determine if a set of points lies on the XY plane
  ///
  /// @param[in] V  #V by dim list of vertex positions
  /// @return true if a mesh has constant value of 0 in z coordinate
  ///
  /// \bug Doesn't determine if vertex is flat if it doesn't lie on the
  /// XY plane.
  IGL_INLINE bool is_planar(const Eigen::MatrixXd & V);
}

#ifndef IGL_STATIC_LIBRARY
#  include "is_planar.cpp"
#endif
#endif
