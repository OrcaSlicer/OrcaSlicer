// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "triangle_triangle_squared_distance.h"
#include "point_triangle_squared_distance.h"
#include <CGAL/Vector_3.h>
#include <CGAL/Segment_3.h>
#include <CGAL/intersections.h>

template < typename Kernel>
IGL_INLINE bool igl::copyleft::cgal::triangle_triangle_squared_distance(
  const CGAL::Triangle_3<Kernel> & T1,
  const CGAL::Triangle_3<Kernel> & T2,
  CGAL::Point_3<Kernel> & P1,
  CGAL::Point_3<Kernel> & P2,
  typename Kernel::FT & d)
{
  typedef CGAL::Point_3<Kernel> Point_3;
  typedef CGAL::Triangle_3<Kernel> Triangle_3;
  typedef CGAL::Segment_3<Kernel> Segment_3;
  typedef typename Kernel::FT EScalar;
  assert(!T1.is_degenerate());
  assert(!T2.is_degenerate());

  bool unique = true;
  if(CGAL::do_intersect(T1,T2))
  {
    // intersecting triangles have zero (squared) distance
    CGAL::Object result = CGAL::intersection(T1,T2);
    // Some point on the intersection result
    CGAL::Point_3<Kernel> Q;
    if(const Point_3 * p = CGAL::object_cast<Point_3 >(&result))
    {
      Q = *p;
    }else if(const Segment_3 * s = CGAL::object_cast<Segment_3 >(&result))
    {
      unique = false;
      Q = s->source();
    }else if(const Triangle_3 *itri = CGAL::object_cast<Triangle_3 >(&result))
    {
      Q = s->vertex(0);
      unique = false;
    }else if(const std::vector<Point_3 > *polyp = 
      CGAL::object_cast< std::vector<Point_3 > >(&result))
    {
      assert(polyp->size() > 0 && "intersection poly should not be empty");
      Q = polyp[0];
      unique = false;
    }else
    {
      assert(false && "Unknown intersection result");
    }
    P1 = Q;
    P2 = Q;
    d = 0;
    return unique;
  }
  // triangles do not intersect: the points of closest approach must be on the
  // boundary of one of the triangles
  d = std::numeric_limits<double>::infinity();
  const auto & vertices_face = [&unique](
    const Triangle_3 & T1,
    const Triangle_3 & T2,
    Point_3 & P1,
    Point_3 & P2,
    EScalar & d)
  {
    for(int i = 0;i<3;i++)
    {
      const Point_3 vi = T1.vertex(i);
      Point_3 P2i;
      EScalar di;
      point_triangle_squared_distance(vi,T2,P2i,di);
      if(di<d)
      {
        d = di;
        P1 = vi;
        P2 = P2i;
        unique = true;
      }else if(d == di)
      {
        // edge of T1 floating parallel above T2
        unique = false;
      }
    }
  };
  vertices_face(T1,T2,P1,P2,d);
  vertices_face(T2,T1,P1,P2,d);
  for(int i = 0;i<3;i++)
  {
    const Segment_3 s1( T1.vertex(i+1), T1.vertex(i+2));
    for(int j = 0;j<3;j++)
    {
      const Segment_3 s2( T2.vertex(i+1), T2.vertex(i+2));
      Point_3 P1ij;
      Point_3 P2ij;
      EScalar dij;
      bool uniqueij = segment_segment_squared_distance(s1,s2,P1ij,P2ij,dij);
      if(dij < d)
      {
        P1 = P1ij;
        P2 = P2ij;
        d = dij;
        unique = uniqueij;
      }
    }
  }
  return unique;
}
