// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Qingan Zhou <qnzhou@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_DELAUNAY_TRIANGULATION_H
#define IGL_DELAUNAY_TRIANGULATION_H

#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  /// Given a set of points in 2D, return a Delaunay triangulation of these
  /// points.
  ///
  /// @param[in] V  #V by 2 list of vertex positions
  /// @param[in] orient2D  A functor such that orient2D(pa, pb, pc) returns
  ///               1 if pa,pb,pc forms a conterclockwise triangle.
  ///              -1 if pa,pb,pc forms a clockwise triangle.
  ///               0 if pa,pb,pc are collinear.
  ///              where the argument pa,pb,pc are of type Scalar[2].
  /// @param[in] incircle  A functor such that incircle(pa, pb, pc, pd) returns
  ///               1 if pd is on the positive size of circumcirle of (pa,pb,pc)
  ///              -1 if pd is on the positive size of circumcirle of (pa,pb,pc)
  ///               0 if pd is cocircular with pa, pb, pc.
  /// @param[out] F  #F by 3 of faces in Delaunay triangulation.
  template<
    typename DerivedV,
    typename Orient2D,
    typename InCircle,
    typename DerivedF
    >
  IGL_INLINE void delaunay_triangulation(
      const Eigen::MatrixBase<DerivedV>& V,
      Orient2D orient2D,
      InCircle incircle,
      Eigen::PlainObjectBase<DerivedF>& F);
}

#ifndef IGL_STATIC_LIBRARY
#  include "delaunay_triangulation.cpp"
#endif
#endif
