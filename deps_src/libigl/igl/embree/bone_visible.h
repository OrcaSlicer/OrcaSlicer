// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EMBREE_BONE_VISIBLE_H
#define IGL_EMBREE_BONE_VISIBLE_H
#include "../igl_inline.h"
#include <Eigen/Core>
#include "EmbreeIntersector.h"
namespace igl
{
  namespace embree
  {
    /// Test whether vertices of mesh are "visible" to a given bone,
    /// where "visible" is defined as in [Baran & Popovic 07]. Instead of checking
    /// whether each point can see *any* of the bone, we just check if each point
    /// can see its own projection onto the bone segment. In other words, we project
    /// each vertex v onto the bone, projv. Then we check if there are any
    /// intersections between the line segment (projv-->v) and the mesh.
    ///
    /// @param[in] V  #V by 3 list of vertex positions
    /// @param[in] F  #F by 3 list of triangle indices
    /// @param[in] s  row vector of position of start end point of bone
    /// @param[in] d  row vector of position of dest end point of bone
    /// @param[out] flag  #V by 1 list of bools (true) visible, (false) obstructed
    ///
    /// \note This checks for hits along the segment which are facing in *any*
    /// direction from the ray.
    template <
      typename DerivedV, 
      typename DerivedF, 
      typename DerivedSD,
      typename Derivedflag>
    IGL_INLINE void bone_visible(
      const Eigen::MatrixBase<DerivedV> & V,
      const Eigen::MatrixBase<DerivedF> & F,
      const Eigen::MatrixBase<DerivedSD> & s,
      const Eigen::MatrixBase<DerivedSD> & d,
      Eigen::PlainObjectBase<Derivedflag>  & flag);
    /// \overload
    /// @param[in] ei  EmbreeIntersector for mesh (V,F) should be double sided
    template <
      typename DerivedV, 
      typename DerivedF, 
      typename DerivedSD,
      typename Derivedflag>
    IGL_INLINE void bone_visible(
      const Eigen::MatrixBase<DerivedV> & V,
      const Eigen::MatrixBase<DerivedF> & F,
      const EmbreeIntersector & ei,
      const Eigen::MatrixBase<DerivedSD> & s,
      const Eigen::MatrixBase<DerivedSD> & d,
      Eigen::PlainObjectBase<Derivedflag>  & flag);
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "bone_visible.cpp"
#endif
#endif
