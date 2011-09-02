// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Michael Rabinovich
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_FLIP_AVOIDING_LINE_SEARCH_H
#define IGL_FLIP_AVOIDING_LINE_SEARCH_H
#include "igl_inline.h"
#include "PI.h"

#include <Eigen/Dense>

namespace igl
{
  // A bisection line search for a mesh based energy that avoids triangle flips as suggested in
  // 		"Bijective Parameterization with Free Boundaries" (Smith J. and Schaefer S., 2015).
  //
  // The user specifies an initial vertices position (that has no flips) and target one (that my have flipped triangles).
  // This method first computes the largest step in direction of the destination vertices that does not incur flips,
  // and then minimizes a given energy using this maximal step and a bisection linesearch (see igl::line_search).
  //
  // Supports both triangle and tet meshes.
  //
  // Inputs:
  //   F  #F by 3/4 				list of mesh faces or tets
  //   cur_v  						#V by dim list of variables
  //   dst_v  						#V by dim list of target vertices. This mesh may have flipped triangles
  //   energy       			    A function to compute the mesh-based energy (return an energy that is bigger than 0)
  //   cur_energy(OPTIONAL)         The energy at the given point. Helps save redundant computations.
  //							    This is optional. If not specified, the function will compute it.
  // Outputs:
  //		cur_v  						#V by dim list of variables at the new location
  // Returns the energy at the new point
  IGL_INLINE double flip_avoiding_line_search(
    const Eigen::MatrixXi F,
    Eigen::MatrixXd& cur_v,
    Eigen::MatrixXd& dst_v,
    std::function<double(Eigen::MatrixXd&)> energy,
    double cur_energy = -1);

}

#ifndef IGL_STATIC_LIBRARY
#  include "flip_avoiding_line_search.cpp"
#endif

#endif
