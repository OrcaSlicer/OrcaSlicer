// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_SNAP_ROUNDING_H
#define IGL_COPYLEFT_CGAL_SNAP_ROUNDING_H

#include "../../igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // SNAP_ROUNDING Snap a list of possible intersecting segments with
      // endpoints in any precision to _the_ integer grid.
      //
      // Inputs:
      //   V  #V by 2 list of vertex positions
      //   E  #E by 2 list of segment indices into V
      // Outputs:
      //   VI  #VI by 2 list of output integer vertex positions, rounded copies
      //     of V are always the first #V vertices
      //   EI  #EI by 2 list of segment indices into V, #EI ≥ #E
      //   J  #EI list of indices into E revealing "parent segments"
      template <
        typename DerivedV, 
        typename DerivedE, 
        typename DerivedVI, 
        typename DerivedEI,
        typename DerivedJ>
      IGL_INLINE void snap_rounding(
        const Eigen::PlainObjectBase<DerivedV> & V,
        const Eigen::PlainObjectBase<DerivedE> & E,
        Eigen::PlainObjectBase<DerivedVI> & VI,
        Eigen::PlainObjectBase<DerivedEI> & EI,
        Eigen::PlainObjectBase<DerivedJ> & J);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "snap_rounding.cpp"
#endif

#endif
