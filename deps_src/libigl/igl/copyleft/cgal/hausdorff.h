// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_HAUSDORFF_H
#define IGL_COPYLEFT_CGAL_HAUSDORFF_H
#include "../../igl_inline.h"

#include <Eigen/Dense>
#include "CGAL_includes.hpp"
#include <vector>

namespace igl 
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Compute lower and upper bounds (l,u) on the Hausdorff distance between a triangle
      /// (V) and a pointset (e.g., mesh, triangle soup) given by a distance function
      /// handle (dist_to_B).
      ///
      /// @param[in] V   3 by 3 list of corner positions so that V.row(i) is the position of the
      ///     ith corner
      /// @param[in] treeB  CGAL's AABB tree containing triangle soup (VB,FB)
      /// @param[in] TB  list of CGAL triangles in order of FB (for determining which was found
      ///     in computation)
      /// @param[out] l  lower bound on Hausdorff distance 
      /// @param[out] u  upper bound on Hausdorff distance
      ///
      template <
        typename DerivedV,
        typename Kernel,
        typename Scalar>
      IGL_INLINE void hausdorff(
        const Eigen::MatrixBase<DerivedV>& V,
        const CGAL::AABB_tree<
          CGAL::AABB_traits<Kernel, 
            CGAL::AABB_triangle_primitive<Kernel, 
              typename std::vector<CGAL::Triangle_3<Kernel> >::iterator
            >
          >
        > & treeB,
        const std::vector<CGAL::Triangle_3<Kernel> > & TB,
        Scalar & l,
        Scalar & u);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "hausdorff.cpp"
#endif

#endif


