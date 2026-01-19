// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_POINT_SEGMENT_SQUARED_DISTANCE_H
#define IGL_COPYLEFT_CGAL_POINT_SEGMENT_SQUARED_DISTANCE_H
#include "../../igl_inline.h"
#include <CGAL/Segment_3.h>
#include <CGAL/Point_3.h>
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // Given a point P1 and segment S2 find the points on each of closest
      // approach and the squared distance thereof.
      // 
      // Inputs:
      //   P1  point
      //   S2  segment
      // Outputs:
      //   P2  point on S2 closest to P1
      //   d  distance betwee P1 and S2
      template < typename Kernel>
      IGL_INLINE void point_segment_squared_distance(
          const CGAL::Point_3<Kernel> & P1,
          const CGAL::Segment_3<Kernel> & S2,
          CGAL::Point_3<Kernel> & P2,
          typename Kernel::FT & d
          );

    }
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "point_segment_squared_distance.cpp"
#endif

#endif

