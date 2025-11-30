// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2017 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_CONVEX_HULL_H
#define IGL_COPYLEFT_CGAL_CONVEX_HULL_H
#include "../../igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Given a set of points (V), compute the convex hull as a triangle mesh (W,G)
      /// 
      /// @param[in] V  #V by 3 list of input points
      /// @param[out] W  #W by 3 list of convex hull points
      /// @param[out] G  #G by 3 list of triangle indices into W
      template <
        typename DerivedV,
        typename DerivedW,
        typename DerivedG>
      IGL_INLINE void convex_hull(
        const Eigen::MatrixBase<DerivedV> & V,
        Eigen::PlainObjectBase<DerivedW> & W,
        Eigen::PlainObjectBase<DerivedG> & G);
      /// \overload
      template <
        typename DerivedV,
        typename DerivedF>
      IGL_INLINE void convex_hull(
        const Eigen::MatrixBase<DerivedV> & V,
        Eigen::PlainObjectBase<DerivedF> & F);
    }
  }
}
  
#ifndef IGL_STATIC_LIBRARY
#  include "convex_hull.cpp"
#endif

#endif 
