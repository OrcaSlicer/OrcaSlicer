// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2020 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_POLYGONS_TO_TRIANGLES_H
#define IGL_POLYGONS_TO_TRIANGLES_H

#include "../igl_inline.h"
#include <Eigen/Core>
#include <vector>

namespace igl
{
  namespace predicates
  {
    /// Given a polygon mesh, trivially triangulate each polygon with a fan. This
    /// purely combinatorial triangulation will work well for convex/flat polygons
    /// and degrade otherwise.
    ///
    /// @param[in] V  #V by dim list of vertex positions
    /// @param[in] I  #I vectorized list of polygon corner indices into rows of some matrix V
    /// @param[in] C  #polygons+1 list of cumulative polygon sizes so that C(i+1)-C(i) =
    ///     size of the ith polygon, and so I(C(i)) through I(C(i+1)-1) are the
    ///     indices of the ith polygon
    /// @param[out] F  #F by 3 list of triangle indices into rows of V
    /// @param[out] J  #F list of indices into 0:#P-1 of corresponding polygon
    ///
    template <
      typename DerivedV,
      typename DerivedI,
      typename DerivedC,
      typename DerivedF,
      typename DerivedJ>
    IGL_INLINE void polygons_to_triangles(
      const Eigen::MatrixBase<DerivedV> & V,
      const Eigen::MatrixBase<DerivedI> & I,
      const Eigen::MatrixBase<DerivedC> & C,
      Eigen::PlainObjectBase<DerivedF> & F,
      Eigen::PlainObjectBase<DerivedJ> & J);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "polygons_to_triangles.cpp"
#endif

#endif
