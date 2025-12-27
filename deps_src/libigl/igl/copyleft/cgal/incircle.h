// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Qingan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_COPYLEFT_CGAL_INCIRCLE_H
#define IGL_COPYLEFT_CGAL_INCIRCLE_H

#include "../../igl_inline.h"

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // Inputs:
      //   pa,pb,pc,pd  2D points.
      // Output:
      //   1 if pd is inside of the oriented circle formed by pa,pb,pc.
      //   0 if pd is co-circular with pa,pb,pc.
      //  -1 if pd is outside of the oriented circle formed by pa,pb,pc.
      template <typename Scalar>
      IGL_INLINE short incircle(
          const Scalar pa[2],
          const Scalar pb[2],
          const Scalar pc[2],
          const Scalar pd[2]);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "incircle.cpp"
#endif
#endif
