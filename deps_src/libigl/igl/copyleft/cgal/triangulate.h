// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2021 Alec Jacobson
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_TRIANGULATE_H
#define IGL_COPYLEFT_CGAL_TRIANGULATE_H

#include "../../igl_inline.h"
#include <string>
#include <Eigen/Core>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Triangulate the interior of a polygon using CGAL 
      ///
      /// @param[in] V #V by 2 list of 2D vertex positions
      /// @param[in] E #E by 2 list of vertex ids forming unoriented edges of the boundary of the polygon
      /// @param[in] H #H by 2 coordinates of points contained inside holes of the polygon
      /// @param[in] retain_convex_hull  whether to retain convex hull {true} or trim away
      ///     all faces reachable from infinite by traversing across
      ///     non-constrained edges {false}.  {true → "c" flag in `triangle`}
      /// @param[out] V2  #V2 by 2  coordinates of the vertives of the generated triangulation
      /// @param[out] F2  #F2 by 3  list of indices forming the faces of the generated triangulation
      ///
      /// \see igl::triangle::triangulate
      template <
        typename Kernel,
        typename DerivedV,
        typename DerivedE,
        typename DerivedH,
        typename DerivedV2,
        typename DerivedF2>
      IGL_INLINE void triangulate(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedE> & E,
        const Eigen::MatrixBase<DerivedH> & H,
        const bool retain_convex_hull,
        Eigen::PlainObjectBase<DerivedV2> & V2,
        Eigen::PlainObjectBase<DerivedF2> & F2);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "triangulate.cpp"
#endif

#endif
