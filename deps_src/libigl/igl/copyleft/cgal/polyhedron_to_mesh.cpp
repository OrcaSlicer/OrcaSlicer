// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "polyhedron_to_mesh.h"
#include <CGAL/Polyhedron_3.h>

template <
  typename Polyhedron,
  typename DerivedV,
  typename DerivedF>
IGL_INLINE void igl::copyleft::cgal::polyhedron_to_mesh(
  const Polyhedron & poly,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedF> & F)
{
  using namespace std;
  V.resize(poly.size_of_vertices(),3);
  F.resize(poly.size_of_facets(),3);
  typedef typename Polyhedron::Vertex_const_iterator Vertex_iterator;
  std::map<Vertex_iterator,size_t> vertex_to_index;
  {
    size_t v = 0;
    for(
      typename Polyhedron::Vertex_const_iterator p = poly.vertices_begin();
      p != poly.vertices_end();
      p++)
    {
      V(v,0) = p->point().x();
      V(v,1) = p->point().y();
      V(v,2) = p->point().z();
      vertex_to_index[p] = v;
      v++;
    }
  }
  {
    size_t f = 0;
    for(
      typename Polyhedron::Facet_const_iterator facet = poly.facets_begin();
      facet != poly.facets_end();
      ++facet)
    {
      typename Polyhedron::Halfedge_around_facet_const_circulator he = 
        facet->facet_begin();
      // Facets in polyhedral surfaces are at least triangles.
      assert(CGAL::circulator_size(he) == 3 && "Facets should be triangles");
      size_t c = 0;
      do {
        //// This is stooopidly slow
        // F(f,c) = std::distance(poly.vertices_begin(), he->vertex());
        F(f,c) = vertex_to_index[he->vertex()];
        c++;
      } while ( ++he != facet->facet_begin());
      f++;
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#include <CGAL/Simple_cartesian.h>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Polyhedron_items_with_id_3.h>
template void igl::copyleft::cgal::polyhedron_to_mesh<CGAL::Polyhedron_3<CGAL::Epick, CGAL::Polyhedron_items_3, CGAL::HalfedgeDS_default, std::allocator<int> >, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(CGAL::Polyhedron_3<CGAL::Epick, CGAL::Polyhedron_items_3, CGAL::HalfedgeDS_default, std::allocator<int> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::copyleft::cgal::polyhedron_to_mesh<CGAL::Polyhedron_3<CGAL::Simple_cartesian<double>,CGAL::Polyhedron_items_with_id_3, CGAL::HalfedgeDS_default, std::allocator<int> >, Eigen::Matrix<double, -1, -1, 0,-1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(CGAL::Polyhedron_3<CGAL::Simple_cartesian<double>,CGAL::Polyhedron_items_with_id_3, CGAL::HalfedgeDS_default, std::allocator<int> > const&,Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0,-1, -1> >&);
#endif
