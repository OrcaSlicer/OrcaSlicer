// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_FRAME_FIELD_DEFORMER_H
#define IGL_FRAME_FIELD_DEFORMER_H
#include "igl_inline.h"

#include <Eigen/Dense>

namespace igl
{
  // Deform a mesh to transform the given per-face frame field to be as close
  // as possible to a cross field, in the least square sense.
  //
  // Inputs:
  //   V       #V by 3 coordinates of the vertices
  //   F       #F by 3 list of mesh faces (must be triangles)
  //   FF1     #F by 3 first representative vector of the frame field
  //   FF2     #F by 3 second representative vector of the frame field
  //   lambda  laplacian regularization parameter 0=no regularization 1=full regularization
  //
  // Outputs:
  //   V_d     #F by 3 deformed, first representative vector
  //   V_d     #F by 3 deformed, first representative vector
  //   V_d     #F by 3 deformed, first representative vector
  //
  IGL_INLINE void frame_field_deformer(
    const Eigen::MatrixXd& V,
    const Eigen::MatrixXi& F,
    const Eigen::MatrixXd& FF1,
    const Eigen::MatrixXd& FF2,
    Eigen::MatrixXd&       V_d,
    Eigen::MatrixXd&       FF1_d,
    Eigen::MatrixXd&       FF2_d,
    const int              iterations = 50,
    const double           lambda = 0.1,
    const bool             perturb_initial_guess = true);

}

#ifndef IGL_STATIC_LIBRARY
#  include "frame_field_deformer.cpp"
#endif

#endif
