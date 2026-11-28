// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Qingan Zhou <qnzhou@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_COPYLEFT_CGAL_ORIENT_2D_H
#define IGL_COPYLEFT_CGAL_ORIENT_2D_H

#include "../../igl_inline.h"

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Tests whether a point is above, on, or below a line.
      ///
      /// @param[in] pa 2D point on plane
      /// @param[in] pb 2D point on plane
      /// @param[in] pc 2D point to test
      ///  @return 1 if pa,pb,pc,pd forms a triangle of positive area.
      ///   0 if pa,pb,pc,pd are coplanar.
      ///  -1 if pa,pb,pc,pd forms a tet of negative area.
      template <typename Scalar>
      IGL_INLINE short orient2D(
          const Scalar *pa,
          const Scalar *pb,
          const Scalar *pc);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "orient2D.cpp"
#endif
#endif
