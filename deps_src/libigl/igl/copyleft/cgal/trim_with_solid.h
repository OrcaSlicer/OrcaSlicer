// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_TRIM_WITH_SOLID_H
#define IGL_COPYLEFT_CGAL_TRIM_WITH_SOLID_H

#include "../../igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // TRIM_WITH_SOLID Given an arbitrary mesh (VA,FA) and the boundary mesh
      // (VB,FB) of a solid (as defined in [Zhou et al. 2016]), Resolve intersections
      // between A and B subdividing faces of A so that intersections with B exists
      // only along edges and vertices (and coplanar faces). Then determine whether
      // each of these faces is inside or outside of B. This can be used to extract
      // the part of A inside or outside of B.
      //
      // Inputs:
      //   VA  #VA by 3 list of mesh vertex positions of A
      //   FA  #FA by 3 list of mesh triangle indices into VA
      //   VB  #VB by 3 list of mesh vertex positions of B
      //   FB  #FB by 3 list of mesh triangle indices into VB
      // Outputs:
      //   V  #V by 3 list of mesh vertex positions of output
      //   F  #F by 3 list of mesh triangle indices into V
      //   D  #F list of bools whether face is inside B
      //   J  #F list of indices into FA revealing birth parent
      //
      template <
        typename DerivedVA,
        typename DerivedFA,
        typename DerivedVB,
        typename DerivedFB,
        typename DerivedV,
        typename DerivedF,
        typename DerivedD,
        typename DerivedJ>
      IGL_INLINE void trim_with_solid(
        const Eigen::PlainObjectBase<DerivedVA> & VA,
        const Eigen::PlainObjectBase<DerivedFA> & FA,
        const Eigen::PlainObjectBase<DerivedVB> & VB,
        const Eigen::PlainObjectBase<DerivedFB> & FB,
        Eigen::PlainObjectBase<DerivedV> & Vd,
        Eigen::PlainObjectBase<DerivedF> & F,
        Eigen::PlainObjectBase<DerivedD> & D,
        Eigen::PlainObjectBase<DerivedJ> & J);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "trim_with_solid.cpp"
#endif
#endif
