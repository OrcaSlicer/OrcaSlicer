// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "projected_delaunay.h"
#include "../../REDRUM.h"
#include <iostream>
#include <cassert>

#if CGAL_VERSION_NR < 1040611000
#  warning "CGAL Version < 4.6.1 may result in crashes. Please upgrade CGAL"
#endif

template <typename Kernel>
IGL_INLINE void igl::copyleft::cgal::projected_delaunay(
  const CGAL::Triangle_3<Kernel> & A,
  const std::vector<CGAL::Object> & A_objects_3,
  CGAL::Constrained_triangulation_plus_2<
    CGAL::Constrained_Delaunay_triangulation_2<
      Kernel,
      CGAL::Triangulation_data_structure_2<
        CGAL::Triangulation_vertex_base_2<Kernel>,
        CGAL::Constrained_triangulation_face_base_2<Kernel> >,
      CGAL::Exact_intersections_tag> > & cdt)
{
  using namespace std;
  // 3D Primitives
  typedef CGAL::Point_3<Kernel>    Point_3;
  typedef CGAL::Segment_3<Kernel>  Segment_3; 
  typedef CGAL::Triangle_3<Kernel> Triangle_3; 
  typedef CGAL::Plane_3<Kernel>    Plane_3;
  //typedef CGAL::Tetrahedron_3<Kernel> Tetrahedron_3; 
  typedef CGAL::Point_2<Kernel>    Point_2;
  //typedef CGAL::Segment_2<Kernel>  Segment_2; 
  //typedef CGAL::Triangle_2<Kernel> Triangle_2; 
  typedef CGAL::Triangulation_vertex_base_2<Kernel>  TVB_2;
  typedef CGAL::Constrained_triangulation_face_base_2<Kernel> CTFB_2;
  typedef CGAL::Triangulation_data_structure_2<TVB_2,CTFB_2> TDS_2;
  typedef CGAL::Exact_intersections_tag Itag;
  typedef CGAL::Constrained_Delaunay_triangulation_2<Kernel,TDS_2,Itag> 
    CDT_2;
  typedef CGAL::Constrained_triangulation_plus_2<CDT_2> CDT_plus_2;

  // http://www.cgal.org/Manual/3.2/doc_html/cgal_manual/Triangulation_2/Chapter_main.html#Section_2D_Triangulations_Constrained_Plus
  // Plane of triangle A
  Plane_3 P(A.vertex(0),A.vertex(1),A.vertex(2));
  // Insert triangle into vertices
  typename CDT_plus_2::Vertex_handle corners[3];
  typedef size_t Index;
  for(Index i = 0;i<3;i++)
  {
    const Point_3 & p3 = A.vertex(i);
    const Point_2 & p2 = P.to_2d(p3);
    typename CDT_plus_2::Vertex_handle corner = cdt.insert(p2);
    corners[i] = corner;
  }
  // Insert triangle edges as constraints
  for(Index i = 0;i<3;i++)
  {
    cdt.insert_constraint( corners[(i+1)%3], corners[(i+2)%3]);
  }
  // Insert constraints for intersection objects
  for( const auto & obj : A_objects_3)
  {
    if(const Segment_3 *iseg = CGAL::object_cast<Segment_3 >(&obj))
    {
      // Add segment constraint
      cdt.insert_constraint(P.to_2d(iseg->vertex(0)),P.to_2d(iseg->vertex(1)));
    }else if(const Point_3 *ipoint = CGAL::object_cast<Point_3 >(&obj))
    {
      // Add point
      cdt.insert(P.to_2d(*ipoint));
    } else if(const Triangle_3 *itri = CGAL::object_cast<Triangle_3 >(&obj))
    {
      // Add 3 segment constraints
      cdt.insert_constraint(P.to_2d(itri->vertex(0)),P.to_2d(itri->vertex(1)));
      cdt.insert_constraint(P.to_2d(itri->vertex(1)),P.to_2d(itri->vertex(2)));
      cdt.insert_constraint(P.to_2d(itri->vertex(2)),P.to_2d(itri->vertex(0)));
    } else if(const std::vector<Point_3 > *polyp = 
        CGAL::object_cast< std::vector<Point_3 > >(&obj))
    {
      //cerr<<REDRUM("Poly...")<<endl;
      const std::vector<Point_3 > & poly = *polyp;
      const Index m = poly.size();
      assert(m>=2);
      for(Index p = 0;p<m;p++)
      {
        const Index np = (p+1)%m;
        cdt.insert_constraint(P.to_2d(poly[p]),P.to_2d(poly[np]));
      }
    }else
    {
      cerr<<REDRUM("What is this object?!")<<endl;
      assert(false);
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::copyleft::cgal::projected_delaunay<CGAL::Epeck>(CGAL::Triangle_3<CGAL::Epeck> const&, std::vector<CGAL::Object, std::allocator<CGAL::Object> > const&, CGAL::Constrained_triangulation_plus_2<CGAL::Constrained_Delaunay_triangulation_2<CGAL::Epeck, CGAL::Triangulation_data_structure_2<CGAL::Triangulation_vertex_base_2<CGAL::Epeck, CGAL::Triangulation_ds_vertex_base_2<void> >, CGAL::Constrained_triangulation_face_base_2<CGAL::Epeck, CGAL::Triangulation_face_base_2<CGAL::Epeck, CGAL::Triangulation_ds_face_base_2<void> > > >, CGAL::Exact_intersections_tag> >&);
template void igl::copyleft::cgal::projected_delaunay<CGAL::Epick>(CGAL::Triangle_3<CGAL::Epick> const&, std::vector<CGAL::Object, std::allocator<CGAL::Object> > const&, CGAL::Constrained_triangulation_plus_2<CGAL::Constrained_Delaunay_triangulation_2<CGAL::Epick, CGAL::Triangulation_data_structure_2<CGAL::Triangulation_vertex_base_2<CGAL::Epick, CGAL::Triangulation_ds_vertex_base_2<void> >, CGAL::Constrained_triangulation_face_base_2<CGAL::Epick, CGAL::Triangulation_face_base_2<CGAL::Epick, CGAL::Triangulation_ds_face_base_2<void> > > >, CGAL::Exact_intersections_tag> >&);
#endif
