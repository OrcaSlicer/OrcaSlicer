// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Alec Jacobson
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "insert_into_cdt.h"
#include <CGAL/Point_3.h>
#include <CGAL/Segment_3.h>
#include <CGAL/Triangle_3.h>

template <typename Kernel>
IGL_INLINE void igl::copyleft::cgal::insert_into_cdt(
  const CGAL::Object & obj,
  const CGAL::Plane_3<Kernel> & P,
  CGAL::Constrained_triangulation_plus_2<
    CGAL::Constrained_Delaunay_triangulation_2<
      Kernel,
      CGAL::Triangulation_data_structure_2<
        CGAL::Triangulation_vertex_base_2<Kernel>,
        CGAL::Constrained_triangulation_face_base_2< Kernel>
      >,
      CGAL::Exact_intersections_tag
    >
  >
  & cdt)
{
  typedef CGAL::Point_3<Kernel>    Point_3;
  typedef CGAL::Segment_3<Kernel>  Segment_3;
  typedef CGAL::Triangle_3<Kernel> Triangle_3;

  if(const Segment_3 *iseg = CGAL::object_cast<Segment_3 >(&obj))
  {
    // Add segment constraint
    cdt.insert_constraint( P.to_2d(iseg->vertex(0)),P.to_2d(iseg->vertex(1)));
  }else if(const Point_3 *ipoint = CGAL::object_cast<Point_3 >(&obj))
  {
    // Add point
    cdt.insert(P.to_2d(*ipoint));
  } else if(const Triangle_3 *itri = CGAL::object_cast<Triangle_3 >(&obj))
  {
    // Add 3 segment constraints
    cdt.insert_constraint( P.to_2d(itri->vertex(0)),P.to_2d(itri->vertex(1)));
    cdt.insert_constraint( P.to_2d(itri->vertex(1)),P.to_2d(itri->vertex(2)));
    cdt.insert_constraint( P.to_2d(itri->vertex(2)),P.to_2d(itri->vertex(0)));
  } else if(const std::vector<Point_3 > *polyp =
      CGAL::object_cast< std::vector<Point_3 > >(&obj))
  {
    const std::vector<Point_3 > & poly = *polyp;
    const size_t m = poly.size();
    assert(m>=2);
    for(size_t p = 0;p<m;p++)
    {
      const size_t np = (p+1)%m;
      cdt.insert_constraint(P.to_2d(poly[p]),P.to_2d(poly[np]));
    }
  }else {
    throw std::runtime_error("Unknown intersection object!");
  }
}

#ifdef IGL_STATIC_LIBRARY
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
// Explicit template instantiation
template void igl::copyleft::cgal::insert_into_cdt<CGAL::Epick>(CGAL::Object const&, CGAL::Plane_3<CGAL::Epick> const&, CGAL::Constrained_triangulation_plus_2<CGAL::Constrained_Delaunay_triangulation_2<CGAL::Epick, CGAL::Triangulation_data_structure_2<CGAL::Triangulation_vertex_base_2<CGAL::Epick, CGAL::Triangulation_ds_vertex_base_2<void> >, CGAL::Constrained_triangulation_face_base_2<CGAL::Epick, CGAL::Triangulation_face_base_2<CGAL::Epick, CGAL::Triangulation_ds_face_base_2<void> > > >, CGAL::Exact_intersections_tag> >&);
template void igl::copyleft::cgal::insert_into_cdt<CGAL::Epeck>(CGAL::Object const&, CGAL::Plane_3<CGAL::Epeck> const&, CGAL::Constrained_triangulation_plus_2<CGAL::Constrained_Delaunay_triangulation_2<CGAL::Epeck, CGAL::Triangulation_data_structure_2<CGAL::Triangulation_vertex_base_2<CGAL::Epeck, CGAL::Triangulation_ds_vertex_base_2<void> >, CGAL::Constrained_triangulation_face_base_2<CGAL::Epeck, CGAL::Triangulation_face_base_2<CGAL::Epeck, CGAL::Triangulation_ds_face_base_2<void> > > >, CGAL::Exact_intersections_tag> >&);
#endif
