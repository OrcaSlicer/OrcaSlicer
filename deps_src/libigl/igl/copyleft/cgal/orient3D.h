// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Qingan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_COPYLEFT_CGAL_ORIENT_3D_H
#define IGL_COPYLEFT_CGAL_ORIENT_3D_H

#include "../../igl_inline.h"

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // Inputs:
      //   pa,pb,pc,pd  3D points.
      // Output:
      //   1 if pa,pb,pc,pd forms a tet of positive volume.
      //   0 if pa,pb,pc,pd are coplanar.
      //  -1 if pa,pb,pc,pd forms a tet of negative volume.
      template <typename Scalar>
      IGL_INLINE short orient3D(
          const Scalar pa[3],
          const Scalar pb[3],
          const Scalar pc[3],
          const Scalar pd[3]);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "orient3D.cpp"
#endif
#endif
