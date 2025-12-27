// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_POINT_SOLID_SIGNED_SQUARED_DISTANCE_H
#define IGL_COPYLEFT_CGAL_POINT_SOLID_SIGNED_SQUARED_DISTANCE_H
#include "../../igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // POINT_SOLID_SIGNED_SQUARED_DISTANCE Given a set of points (Q) and the
      // boundary mesh (VB,FB) of a solid (as defined in [Zhou et al. 2016],
      // determine the signed squared distance for each point q in Q so that d(q,B) is
      // negative if inside and positive if outside.
      //
      // Inputs:
      //   Q  #Q by 3 list of query point positions
      //   VB  #VB by 3 list of mesh vertex positions of B
      //   FB  #FB by 3 list of mesh triangle indices into VB
      // Outputs:
      //   D
      template <
        typename DerivedQ,
        typename DerivedVB,
        typename DerivedFB,
        typename DerivedD>
      IGL_INLINE void point_solid_signed_squared_distance(
        const Eigen::PlainObjectBase<DerivedQ> & Q,
        const Eigen::PlainObjectBase<DerivedVB> & VB,
        const Eigen::PlainObjectBase<DerivedFB> & FB,
        Eigen::PlainObjectBase<DerivedD> & D);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "point_solid_signed_squared_distance.cpp"
#endif

#endif
