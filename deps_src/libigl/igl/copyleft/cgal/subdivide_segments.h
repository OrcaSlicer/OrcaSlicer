// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_SUBDIVIDE_SEGMENTS_H
#define IGL_COPYLEFT_CGAL_SUBDIVIDE_SEGMENTS_H
#include "../../igl_inline.h"
#include <Eigen/Core>
#include <CGAL/Segment_2.h>
#include <CGAL/Point_2.h>
#include <vector>
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // Insert steiner points to subdivide a given set of line segments
      // 
      // Inputs:
      //   V  #V by 2 list of vertex positions
      //   E  #E by 2 list of segment indices into V
      //   steiner  #E list of lists of unsorted steiner points (including
      //     endpoints) along the #E original segments
      // Outputs:
      //   VI  #VI by 2 list of output vertex positions, copies of V are always
      //     the first #V vertices
      //   EI  #EI by 2 list of segment indices into V, #EI ≥ #E
      //   J  #EI list of indices into E revealing "parent segments"
      //   IM  #VI list of indices into VV of unique vertices.
      template <
        typename DerivedV, 
        typename DerivedE,
        typename Kernel, 
        typename DerivedVI, 
        typename DerivedEI,
        typename DerivedJ,
        typename DerivedIM>
      IGL_INLINE void subdivide_segments(
        const Eigen::PlainObjectBase<DerivedV> & V,
        const Eigen::PlainObjectBase<DerivedE> & E,
        const std::vector<std::vector<CGAL::Point_2<Kernel> > > & steiner,
        Eigen::PlainObjectBase<DerivedVI> & VI,
        Eigen::PlainObjectBase<DerivedEI> & EI,
        Eigen::PlainObjectBase<DerivedJ> & J,
        Eigen::PlainObjectBase<DerivedIM> & IM);
    }
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "subdivide_segments.cpp"
#endif
#endif
