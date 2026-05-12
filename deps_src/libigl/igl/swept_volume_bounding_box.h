// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SWEPT_VOLUME_BOUNDING_BOX_H
#define IGL_SWEPT_VOLUME_BOUNDING_BOX_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <functional>
namespace igl
{
  /// Construct an axis-aligned bounding box containing a shape undergoing a
  /// motion sampled at `steps` discrete momements.
  ///
  /// @param[in] n  number of mesh vertices
  /// @param[in] V  function handle so that V(i,t) returns the 3d position of vertex
  ///     i at time t, for t∈[0,1]
  /// @param[in] steps  number of time steps: steps=3 --> t∈{0,0.5,1}
  /// @param[out] box  box containing mesh under motion
  IGL_INLINE void swept_volume_bounding_box(
    const size_t & n,
    const std::function<
      Eigen::RowVector3d(const size_t vi, const double t)> & V,
    const size_t & steps,
    Eigen::AlignedBox3d & box);
}

#ifndef IGL_STATIC_LIBRARY
#  include "swept_volume_bounding_box.cpp"
#endif

#endif 
