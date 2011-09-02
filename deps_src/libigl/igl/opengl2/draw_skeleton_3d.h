// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL2_DRAW_SKELETON_3D_H
#define IGL_OPENGL2_DRAW_SKELETON_3D_H
#include "../igl_inline.h"
#include "../material_colors.h"
#include <Eigen/Core>
namespace igl
{
  namespace opengl2
  {

    // Draw a skeleton
    //
    // Inputs:
    //   C  #C by dim List of joint rest positions
    //   BE  #BE by 2 list of bone edge indices into C
    //   T  #BE*(dim+1) by dim  matrix of stacked transposed bone transformations
    //   color  #BE|1 by 4 list of color
    //   half_bbd  half bounding box diagonal to determine scaling {1.0}
    template <
      typename DerivedC,
      typename DerivedBE,
      typename DerivedT,
      typename Derivedcolor>
    IGL_INLINE void draw_skeleton_3d(
      const Eigen::PlainObjectBase<DerivedC> & C,
      const Eigen::PlainObjectBase<DerivedBE> & BE,
      const Eigen::PlainObjectBase<DerivedT> & T,
      const Eigen::PlainObjectBase<Derivedcolor> & color,
      const double half_bbd=0.5);
    // Default color
    template <typename DerivedC, typename DerivedBE, typename DerivedT>
    IGL_INLINE void draw_skeleton_3d(
      const Eigen::PlainObjectBase<DerivedC> & C,
      const Eigen::PlainObjectBase<DerivedBE> & BE,
      const Eigen::PlainObjectBase<DerivedT> & T);
    template <typename DerivedC, typename DerivedBE>
    IGL_INLINE void draw_skeleton_3d(
      const Eigen::PlainObjectBase<DerivedC> & C,
      const Eigen::PlainObjectBase<DerivedBE> & BE);
  }
}
  
#ifndef IGL_STATIC_LIBRARY
#  include "draw_skeleton_3d.cpp"
#endif
#endif
