// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Qingan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_COPYLEFT_CGAL_INSPHERE_H
#define IGL_COPYLEFT_CGAL_INSPHERE_H

#include "../../igl_inline.h"

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // Inputs:
      //   pa,pb,pc,pd,pe  3D points.
      // Output:
      //   1 if pe is inside of the oriented sphere formed by pa,pb,pc,pd.
      //   0 if pe is co-spherical with pa,pb,pc,pd.
      //  -1 if pe is outside of the oriented sphere formed by pa,pb,pc,pd.
      template <typename Scalar>
      IGL_INLINE short insphere(
          const Scalar pa[3],
          const Scalar pb[3],
          const Scalar pc[3],
          const Scalar pd[3],
          const Scalar pe[3]);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "insphere.cpp"
#endif
#endif
