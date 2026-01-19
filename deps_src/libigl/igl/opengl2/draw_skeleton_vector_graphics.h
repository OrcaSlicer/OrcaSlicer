// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL2_DRAW_SKELETON_VECTOR_GRAPHICS_H
#define IGL_OPENGL2_DRAW_SKELETON_VECTOR_GRAPHICS_H
#include "../igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  namespace opengl2
  {
    // Draw a skeleton with a 2D vector graphcis style à la BBW, STBS, Monotonic,
    // FAST papers.
    //
    // Inputs:
    //   C  #C by dim list of joint positions
    //   BE #BE by 2 list of bone edge indices into C
    //  point_color  color of points
    //  line_color  color of lines
    IGL_INLINE void draw_skeleton_vector_graphics(
      const Eigen::MatrixXd & C,
      const Eigen::MatrixXi & BE,
      const float * point_color,
      const float * line_color);
    // Use default colors (originally from BBW paper)
    IGL_INLINE void draw_skeleton_vector_graphics(
      const Eigen::MatrixXd & C,
      const Eigen::MatrixXi & BE);
    //   T  #BE*(dim+1) by dim  matrix of stacked transposed bone transformations
    template <typename DerivedC, typename DerivedBE, typename DerivedT>
    IGL_INLINE void draw_skeleton_vector_graphics(
      const Eigen::PlainObjectBase<DerivedC> & C,
      const Eigen::PlainObjectBase<DerivedBE> & BE,
      const Eigen::PlainObjectBase<DerivedT> & T,
      const float * point_color,
      const float * line_color);
    template <typename DerivedC, typename DerivedBE, typename DerivedT>
    IGL_INLINE void draw_skeleton_vector_graphics(
      const Eigen::PlainObjectBase<DerivedC> & C,
      const Eigen::PlainObjectBase<DerivedBE> & BE,
      const Eigen::PlainObjectBase<DerivedT> & T);
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "draw_skeleton_vector_graphics.cpp"
#endif
#endif
