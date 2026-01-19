// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "point_triangle_squared_distance.h"
#include <CGAL/Segment_3.h>
template < typename Kernel>
IGL_INLINE void point_triangle_squared_distance(
  const CGAL::Point_3<Kernel> & P1,
  const CGAL::Triangle_3<Kernel> & T2,
  CGAL::Point_3<Kernel> & P2,
  typename Kernel::FT & d)
{
  assert(!T2.is_degenerate());
  if(T2.has_on(P1))
  {
    P2 = P1;
    d = 0;
    return;
  }
  const auto proj_1 = T2.supporting_plane().projection(P2);
  if(T2.has_on(proj_1))
  {
    P2 = proj_1;
    d = (proj_1-P1).squared_length();
    return;
  }
  // closest point must be on the boundary
  bool first = true;
  // loop over edges
  for(int i=0;i<3;i++)
  {
    CGAL::Point_3<Kernel> P2i;
    typename Kernel::FT di;
    const CGAL::Segment_3<Kernel> si( T2.vertex(i+1), T2.vertex(i+2));
    point_segment_squared_distance(P1,si,P2i,di);
    if(first || di < d)
    {
      first = false;
      d = di;
      P2 = P2i;
    }
  }
}
