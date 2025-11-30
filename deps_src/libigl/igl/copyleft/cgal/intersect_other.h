// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_INTERSECT_OTHER_H
#define IGL_COPYLEFT_CGAL_INTERSECT_OTHER_H
#include "../../igl_inline.h"
#include "RemeshSelfIntersectionsParam.h"

#include <Eigen/Dense>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Given a triangle mesh (VA,FA) and another mesh (VB,FB) find all pairs
      /// of intersecting faces. Note that self-intersections are ignored.
      /// 
      /// @param[in] VA  #V by 3 list of vertex positions
      /// @param[in] FA  #F by 3 list of triangle indices into VA
      /// @param[in] VB  #V by 3 list of vertex positions
      /// @param[in] FB  #F by 3 list of triangle indices into VB
      /// @param[in] params   whether to detect only and then whether to only
      ///   find first intersection
      /// @param[out] IF  #intersecting face pairs by 2 list of intersecting
      ///   face pairs, indexing FA and FB
      /// @param[out] VVAB  #VVAB by 3 list of vertex positions
      /// @param[out] FFAB  #FFAB by 3 list of triangle indices into VVA
      /// @param[out] JAB  #FFAB list of indices into [FA;FB] denoting birth triangle
      /// @param[out] IMAB  #VVAB list of indices stitching duplicates
      ///   (resulting from mesh intersections) together
      /// @return true on success
      template <
        typename DerivedVA,
        typename DerivedFA,
        typename DerivedVB,
        typename DerivedFB,
        typename DerivedIF,
        typename DerivedVVAB,
        typename DerivedFFAB,
        typename DerivedJAB,
        typename DerivedIMAB>
      IGL_INLINE bool intersect_other(
        const Eigen::MatrixBase<DerivedVA> & VA,
        const Eigen::MatrixBase<DerivedFA> & FA,
        const Eigen::MatrixBase<DerivedVB> & VB,
        const Eigen::MatrixBase<DerivedFB> & FB,
        const RemeshSelfIntersectionsParam & params,
        Eigen::PlainObjectBase<DerivedIF> & IF,
        Eigen::PlainObjectBase<DerivedVVAB> & VVAB,
        Eigen::PlainObjectBase<DerivedFFAB> & FFAB,
        Eigen::PlainObjectBase<DerivedJAB>  & JAB,
        Eigen::PlainObjectBase<DerivedIMAB> & IMAB);
      /// \overload
      /// @param[in] first_only  whether to only find first intersection
      template <
        typename DerivedVA,
        typename DerivedFA,
        typename DerivedVB,
        typename DerivedFB,
        typename DerivedIF>
      IGL_INLINE bool intersect_other(
        const Eigen::MatrixBase<DerivedVA> & VA,
        const Eigen::MatrixBase<DerivedFA> & FA,
        const Eigen::MatrixBase<DerivedVB> & VB,
        const Eigen::MatrixBase<DerivedFB> & FB,
        const bool first_only,
        Eigen::PlainObjectBase<DerivedIF> & IF);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "intersect_other.cpp"
#endif
  
#endif

