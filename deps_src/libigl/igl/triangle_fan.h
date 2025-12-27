// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_TRIANGLE_FAN_H
#define IGL_TRIANGLE_FAN_H
#include "igl_inline.h"
#include <Eigen/Dense>
namespace igl
{
  // Given a list of faces tessellate all of the "exterior" edges forming another
  // list of 
  //
  // Inputs:
  //   E  #E by simplex_size-1  list of exterior edges (see exterior_edges.h)
  // Outputs:
  //   cap  #cap by simplex_size  list of "faces" tessellating the boundary edges
  IGL_INLINE void triangle_fan(
    const Eigen::MatrixXi & E,
    Eigen::MatrixXi & cap);
  // In-line version
  IGL_INLINE Eigen::MatrixXi triangle_fan( const Eigen::MatrixXi & E);
}
#ifndef IGL_STATIC_LIBRARY
#  include "triangle_fan.cpp"
#endif
#endif
