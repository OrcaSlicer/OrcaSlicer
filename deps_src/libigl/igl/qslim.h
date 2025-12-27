// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_QSLIM_H
#define IGL_QSLIM_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{

  // Decimate (simplify) a triangle mesh in nD according to the paper
  // "Simplifying Surfaces with Color and Texture using Quadric Error Metrics"
  // by [Garland and Heckbert, 1987] (technically a followup to qslim). The
  // mesh can have open boundaries but should be edge-manifold.
  //
  // Inputs:
  //   V  #V by dim list of vertex positions. Assumes that vertices w
  //   F  #F by 3 list of triangle indices into V
  //   max_m  desired number of output faces
  // Outputs:
  //   U  #U by dim list of output vertex posistions (can be same ref as V)
  //   G  #G by 3 list of output face indices into U (can be same ref as G)
  //   J  #G list of indices into F of birth face
  //   I  #U list of indices into V of birth vertices
  IGL_INLINE bool qslim(
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const size_t max_m,
    Eigen::MatrixXd & U,
    Eigen::MatrixXi & G,
    Eigen::VectorXi & J,
    Eigen::VectorXi & I);
}
#ifndef IGL_STATIC_LIBRARY
#  include "qslim.cpp"
#endif
#endif
