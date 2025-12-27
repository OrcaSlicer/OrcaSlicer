// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COLUMN_TO_QUATS_H
#define IGL_COLUMN_TO_QUATS_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <Eigen/StdVector>
#include <vector>
namespace igl
{
  // "Columnize" a list of quaternions (q1x,q1y,q1z,q1w,q2x,q2y,q2z,q2w,...)
  //
  // Inputs:
  //   Q  n*4-long list of coefficients
  // Outputs:
  //   vQ  n-long list of quaternions
  // Returns false if n%4!=0
  IGL_INLINE bool column_to_quats(
    const Eigen::VectorXd & Q,
    std::vector<
      Eigen::Quaterniond,Eigen::aligned_allocator<Eigen::Quaterniond> > & vQ);
}

#ifndef IGL_STATIC_LIBRARY
#  include "column_to_quats.cpp"
#endif

#endif
