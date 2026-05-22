// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Stefan Brugger <stefanbrugger@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MAP_VERTICES_TO_CIRCLE_H
#define IGL_MAP_VERTICES_TO_CIRCLE_H
#include "igl_inline.h"
#include "PI.h"

#include <Eigen/Dense>
#include <vector>

namespace igl
{
  /// Map the vertices whose indices are in a given boundary loop (bnd) on the
  /// unit circle with spacing proportional to the original boundary edge
  /// lengths.
  ///
  /// @param[in] V  #V by dim list of mesh vertex positions
  /// @param[in] b  #W list of vertex ids
  /// @param[out] UV   #W by 2 list of 2D position on the unit circle for the vertices in b
  IGL_INLINE void map_vertices_to_circle(
    const Eigen::MatrixXd& V,
    const Eigen::VectorXi& bnd,
    Eigen::MatrixXd& UV);
}

#ifndef IGL_STATIC_LIBRARY
#  include "map_vertices_to_circle.cpp"
#endif

#endif
