// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_AVERAGE_ONTO_FACES_H
#define IGL_AVERAGE_ONTO_FACES_H
#include "igl_inline.h"

#include <Eigen/Dense>
namespace igl 
{
  // average_onto_vertices 
  // Move a scalar field defined on faces to vertices by averaging
  //
  // Input:
  // V,F: mesh
  // S: scalar field defined on vertices, Vx1
  // 
  // Output:
  // SV: scalar field defined on faces
  template <typename T, typename I>
  IGL_INLINE void average_onto_faces(
    const Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic> &V,
    const Eigen::Matrix<I, Eigen::Dynamic, Eigen::Dynamic> &F,
    const Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic> &S,
    Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic> &SF);
}

#ifndef IGL_STATIC_LIBRARY
#  include "average_onto_faces.cpp"
#endif

#endif
