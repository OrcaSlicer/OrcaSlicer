// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
//                    Qingan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_LEXICOGRAPHIC_TRIANGULATION_H
#define IGL_LEXICOGRAPHIC_TRIANGULATION_H
#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  /// Given a set of points in 2D, return a lexicographic triangulation of these
  /// points.
  ///
  /// @param[in] P  #P by 2 list of vertex positions
  /// @param[in] orient2D  A functor such that orient2D(pa, pb, pc) returns
  ///               1 if pa,pb,pc forms a conterclockwise triangle.
  ///              -1 if pa,pb,pc forms a clockwise triangle.
  ///               0 if pa,pb,pc are collinear.
  ///              where the argument pa,pb,pc are of type Scalar[2].
  /// @param[out] F  #F by 3 of faces in lexicographic triangulation.
  template<
    typename DerivedP,
    typename Orient2D,
    typename DerivedF
    >
  IGL_INLINE void lexicographic_triangulation(
      const Eigen::MatrixBase<DerivedP>& P,
      Orient2D orient2D,
      Eigen::PlainObjectBase<DerivedF>& F);
}




#ifndef IGL_STATIC_LIBRARY
#  include "lexicographic_triangulation.cpp"
#endif
#endif
