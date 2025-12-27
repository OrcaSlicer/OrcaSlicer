// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COMISO_FRAMEFIELD_H
#define IGL_COMISO_FRAMEFIELD_H

#include <igl/igl_inline.h>
#include <igl/PI.h>
#include <Eigen/Dense>
#include <vector>

namespace igl
{
namespace copyleft
{
namespace comiso
{
// Generate a piecewise-constant frame-field field from a sparse set of constraints on faces
// using the algorithm proposed in:
// Frame Fields: Anisotropic and Non-Orthogonal Cross Fields
// Daniele Panozzo, Enrico Puppo, Marco Tarini, Olga Sorkine-Hornung,
// ACM Transactions on Graphics (SIGGRAPH, 2014)
//
// Inputs:
//   V       #V by 3 list of mesh vertex coordinates
//   F       #F by 3 list of mesh faces (must be triangles)
//   b       #B by 1 list of constrained face indices
//   bc1     #B by 3 list of the constrained first representative vector of the frame field (up to permutation and sign)
//   bc2     #B by 3 list of the constrained second representative vector of the frame field (up to permutation and sign)
//
// Outputs:
//   FF1      #F by 3 the first representative vector of the frame field (up to permutation and sign)
//   FF2      #F by 3 the second representative vector of the frame field (up to permutation and sign)
//
// TODO: it now supports only soft constraints, should be extended to support both hard and soft constraints
IGL_INLINE void frame_field(
  const Eigen::MatrixXd& V,
  const Eigen::MatrixXi& F,
  const Eigen::VectorXi& b,
  const Eigen::MatrixXd& bc1,
  const Eigen::MatrixXd& bc2,
  Eigen::MatrixXd& FF1,
  Eigen::MatrixXd& FF2
  );
}
}
}

#ifndef IGL_STATIC_LIBRARY
#  include "frame_field.cpp"
#endif

#endif
