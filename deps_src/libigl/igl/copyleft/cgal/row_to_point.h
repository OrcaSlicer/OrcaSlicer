// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_ROW_TO_POINT_H
#define IGL_COPYLEFT_CGAL_ROW_TO_POINT_H
#include "../../igl_inline.h"
#include <Eigen/Core>
#include <CGAL/Point_2.h>
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // Extract a row from V and treat as a 2D cgal point (only first two
      // columns of V are used).
      // 
      // Inputs:
      //   V  #V by 2 list of vertex positions
      //   i  row index
      // Returns 2D cgal point
      template <
        typename Kernel,
        typename DerivedV>
      IGL_INLINE CGAL::Point_2<Kernel> row_to_point(
        const Eigen::PlainObjectBase<DerivedV> & V,
        const typename DerivedV::Index & i);
    }
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "row_to_point.cpp"
#endif
#endif
