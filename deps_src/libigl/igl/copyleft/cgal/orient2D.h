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
      // Inputs:
      //   pa,pb,pc   2D points.
      // Output:
      //   1 if pa,pb,pc are counterclockwise oriented.
      //   0 if pa,pb,pc are counterclockwise oriented.
      //  -1 if pa,pb,pc are clockwise oriented.
      template <typename Scalar>
      IGL_INLINE short orient2D(
          const Scalar pa[2],
          const Scalar pb[2],
          const Scalar pc[2]);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "orient2D.cpp"
#endif
#endif
