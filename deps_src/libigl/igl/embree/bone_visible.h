// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EMBREE_BONE_VISIBLE_H
#define IGL_EMBREE_BONE_VISIBLE_H
#include <igl/igl_inline.h>
#include <Eigen/Core>
#include "EmbreeIntersector.h"
namespace igl
{
  namespace embree
  {
    //
    // BONE_VISIBLE  test whether vertices of mesh are "visible" to a given bone,
    // where "visible" is defined as in [Baran & Popovic 07]. Instead of checking
    // whether each point can see *any* of the bone, we just check if each point
    // can see its own projection onto the bone segment. In other words, we project
    // each vertex v onto the bone, projv. Then we check if there are any
    // intersections between the line segment (projv-->v) and the mesh.
    //
    // [flag] = bone_visible(V,F,s,d);
    //
    // Input:
    //    V  #V by 3 list of vertex positions
    //    F  #F by 3 list of triangle indices
    //    s  row vector of position of start end point of bone
    //    d  row vector of position of dest end point of bone
    // Output:
    //    flag  #V by 1 list of bools (true) visible, (false) obstructed
    //
    // Note: This checks for hits along the segment which are facing in *any*
    // direction from the ray.
    //
    template <
      typename DerivedV, 
      typename DerivedF, 
      typename DerivedSD,
      typename Derivedflag>
    IGL_INLINE void bone_visible(
      const Eigen::PlainObjectBase<DerivedV> & V,
      const Eigen::PlainObjectBase<DerivedF> & F,
      const Eigen::PlainObjectBase<DerivedSD> & s,
      const Eigen::PlainObjectBase<DerivedSD> & d,
      Eigen::PlainObjectBase<Derivedflag>  & flag);
    // Inputs:
    //  ei  EmbreeIntersector for mesh (V,F) should be double sided
    template <
      typename DerivedV, 
      typename DerivedF, 
      typename DerivedSD,
      typename Derivedflag>
    IGL_INLINE void bone_visible(
      const Eigen::PlainObjectBase<DerivedV> & V,
      const Eigen::PlainObjectBase<DerivedF> & F,
      const EmbreeIntersector & ei,
      const Eigen::PlainObjectBase<DerivedSD> & s,
      const Eigen::PlainObjectBase<DerivedSD> & d,
      Eigen::PlainObjectBase<Derivedflag>  & flag);
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "bone_visible.cpp"
#endif
#endif
