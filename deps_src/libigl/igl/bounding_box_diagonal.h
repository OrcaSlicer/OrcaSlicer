// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_BOUNDING_BOX_DIAGONAL_H
#define IGL_BOUNDING_BOX_DIAGONAL_H
#include "igl_inline.h"
#include <Eigen/Dense>
namespace igl
{
  // Compute the length of the diagonal of a given meshes axis-aligned bounding
  // box
  //
  // Inputs:
  //   V  #V by 3 list of vertex positions
  //   F  #F by 3 list of triangle indices into V
  // Returns length of bounding box diagonal
  IGL_INLINE double bounding_box_diagonal( const Eigen::MatrixXd & V);
}

#ifndef IGL_STATIC_LIBRARY
#  include "bounding_box_diagonal.cpp"
#endif

#endif
