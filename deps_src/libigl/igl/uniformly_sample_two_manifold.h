// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_UNIFORMLY_SAMPLE_TWO_MANIFOLD_H
#define IGL_UNIFORMLY_SAMPLE_TWO_MANIFOLD_H
#include "igl_inline.h"
#include <Eigen/Dense>
namespace igl
{
  // UNIFORMLY_SAMPLE_TWO_MANIFOLD Attempt to sample a mesh uniformly by
  // furthest point relaxation as described in "Fast Automatic Skinning
  // Transformations"
  //
  // [Jacobson et al. 12] Section 3.3.
  //
  // Inputs:
  //   W  #W by dim positions of mesh in weight space
  //   F  #F by 3 indices of triangles
  //   k  number of samplse
  //   push  factor by which corners should be pushed away
  // Outputs
  //   WS  k by dim locations in weights space
  //
  IGL_INLINE void uniformly_sample_two_manifold(
    const Eigen::MatrixXd & W,
    const Eigen::MatrixXi & F, 
    const int k, 
    const double push,
    Eigen::MatrixXd & WS);
  // Find uniform sampling up to placing samples on mesh vertices
  IGL_INLINE void uniformly_sample_two_manifold_at_vertices(
    const Eigen::MatrixXd & OW,
    const int k, 
    const double push,
    Eigen::VectorXi & S);
}
#ifndef IGL_STATIC_LIBRARY
#  include "uniformly_sample_two_manifold.cpp"
#endif
#endif
