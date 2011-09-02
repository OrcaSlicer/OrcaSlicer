// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SWEPT_VOLUME_SIGNED_DISTANCE_H
#define IGL_SWEPT_VOLUME_SIGNED_DISTANCE_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <functional>
namespace igl
{
  // Compute the signed distance to a sweep surface of a mesh under-going
  // an arbitrary motion V(t) discretely sampled at `steps`-many moments in
  // time at a grid.
  //
  // Inputs:
  //   V  #V by 3 list of mesh positions in reference pose
  //   F  #F by 3 list of triangle indices [0,n)
  //   transform  function handle so that transform(t) returns the rigid
  //     transformation at time t∈[0,1]
  //   steps  number of time steps: steps=3 --> t∈{0,0.5,1}
  //   GV  #GV by 3 list of evaluation point grid positions
  //   res  3-long resolution of GV grid
  //   h  edge-length of grid
  //   isolevel  isolevel to "focus" on; grid positions far enough away from
  //     isolevel (based on h) will get approximate values). Set
  //     isolevel=infinity to get good values everywhere (slow and
  //     unnecessary if just trying to extract isolevel-level set).
  //   S0  #GV initial values (will take minimum with these), can be same
  //     as S)
  // Outputs:
  //   S  #GV list of signed distances
  IGL_INLINE void swept_volume_signed_distance(
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const std::function<Eigen::Affine3d(const double t)> & transform,
    const size_t & steps,
    const Eigen::MatrixXd & GV,
    const Eigen::RowVector3i & res,
    const double h,
    const double isolevel,
    const Eigen::VectorXd & S0,
    Eigen::VectorXd & S);
  IGL_INLINE void swept_volume_signed_distance(
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const std::function<Eigen::Affine3d(const double t)> & transform,
    const size_t & steps,
    const Eigen::MatrixXd & GV,
    const Eigen::RowVector3i & res,
    const double h,
    const double isolevel,
    Eigen::VectorXd & S);
  }

#ifndef IGL_STATIC_LIBRARY
#  include "swept_volume_signed_distance.cpp"
#endif 

#endif
