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
      /// Test whether point is in a given circle
      ///
      /// @param[in] pa 2D point on sphere
      /// @param[in] pb 2D point on sphere
      /// @param[in] pc 2D point on sphere
      /// @param[in] pd 2D point to test
      /// @return 1 if pd is inside of the oriented circle formed by pa,pb,pc.
      ///   0 if pd is co-circular with pa,pb,pc.
      ///  -1 if pd is outside of the oriented circle formed by pa,pb,pc.
      template <typename Scalar>
      IGL_INLINE short incircle(
          const Scalar *pa,
          const Scalar *pb,
          const Scalar *pc,
          const Scalar *pd);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "incircle.cpp"
#endif
#endif
