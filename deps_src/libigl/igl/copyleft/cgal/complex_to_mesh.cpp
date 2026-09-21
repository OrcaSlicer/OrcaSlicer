// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "complex_to_mesh.h"

#include "../../centroid.h"
#include "../../remove_unreferenced.h"

#include <CGAL/Surface_mesh_default_triangulation_3.h>
#include <CGAL/Delaunay_triangulation_cell_base_with_circumcenter_3.h>
#include <set>
#include <stack>

template <typename Tr, typename DerivedV, typename DerivedF>
IGL_INLINE bool igl::copyleft::cgal::complex_to_mesh(
  const CGAL::Complex_2_in_triangulation_3<Tr> & c2t3,
  Eigen::PlainObjectBase<DerivedV> & V, 
  Eigen::PlainObjectBase<DerivedF> & F)
{
  using namespace Eigen;
  // CGAL/IO/Complex_2_in_triangulation_3_file_writer.h
  using CGAL::Surface_mesher::number_of_facets_on_surface;

  typedef typename CGAL::Complex_2_in_triangulation_3<Tr> C2t3;
  typedef typename Tr::Finite_facets_iterator Finite_facets_iterator;
  typedef typename Tr::Finite_vertices_iterator Finite_vertices_iterator;
  typedef typename Tr::Facet Facet;
  typedef typename Tr::Edge Edge;
  typedef typename Tr::Vertex_handle Vertex_handle;

  // Header.
  const Tr& tr = c2t3.triangulation();

  bool success = true;

  const int n = tr.number_of_vertices();
  const int m = c2t3.number_of_facets();

  assert(m == number_of_facets_on_surface(tr));
  
  // Finite vertices coordinates.
  std::map<Vertex_handle, int> v2i;
  V.resize(n,3);
  {
    int v = 0;
    for(Finite_vertices_iterator vit = tr.finite_vertices_begin();
        vit != tr.finite_vertices_end();
        ++vit)
    {
      V(v,0) = vit->point().x(); 
      V(v,1) = vit->point().y(); 
      V(v,2) = vit->point().z(); 
      v2i[vit] = v++;
    }
  }

  {
    Finite_facets_iterator fit = tr.finite_facets_begin();
    std::set<Facet> oriented_set;
    std::stack<Facet> stack;

    while ((int)oriented_set.size() != m) 
    {
      while ( fit->first->is_facet_on_surface(fit->second) == false ||
        oriented_set.find(*fit) != oriented_set.end() ||
        oriented_set.find(c2t3.opposite_facet(*fit)) !=
        oriented_set.end() ) 
      {
        ++fit;
      }
      oriented_set.insert(*fit);
      stack.push(*fit);
      while(! stack.empty() )
      {
        Facet f = stack.top();
        stack.pop();
        for(int ih = 0 ; ih < 3 ; ++ih)
        {
          const int i1  = tr.vertex_triple_index(f.second, tr. cw(ih));
          const int i2  = tr.vertex_triple_index(f.second, tr.ccw(ih));

          const typename C2t3::Face_status face_status
            = c2t3.face_status(Edge(f.first, i1, i2));
          if(face_status == C2t3::REGULAR) 
          {
            Facet fn = c2t3.neighbor(f, ih);
            if (oriented_set.find(fn) == oriented_set.end())
            {
              if(oriented_set.find(c2t3.opposite_facet(fn)) == oriented_set.end())
              {
                oriented_set.insert(fn);
                stack.push(fn);
              }else {
                success = false; // non-orientable
              }
            }
          }else if(face_status != C2t3::BOUNDARY)
          {
            success = false; // non manifold, thus non-orientable
          }
        } // end "for each neighbor of f"
      } // end "stack non empty"
    } // end "oriented_set not full"

    F.resize(m,3);
    int f = 0;
    for(typename std::set<Facet>::const_iterator fit = 
        oriented_set.begin();
        fit != oriented_set.end();
        ++fit)
    {
      const typename Tr::Cell_handle cell = fit->first;
      const int& index = fit->second;
      const int index1 = v2i[cell->vertex(tr.vertex_triple_index(index, 0))];
      const int index2 = v2i[cell->vertex(tr.vertex_triple_index(index, 1))];
      const int index3 = v2i[cell->vertex(tr.vertex_triple_index(index, 2))];
      // This order is flipped
      F(f,0) = index1;
      F(f,1) = index2;
      F(f,2) = index3;
      f++;
    }
    assert(f == m);
  } // end if(facets must be oriented)

  // This CGAL code seems to randomly assign the global orientation
  // Flip based on the signed volume.
  Eigen::Vector3d cen;
  double vol;
  igl::centroid(V,F,cen,vol);
  if(vol < 0)
  {
    // Flip
    F = F.rowwise().reverse().eval();
  }

  // CGAL code somehow can end up with unreferenced vertices
  {
    VectorXi _1;
    remove_unreferenced( MatrixXd(V), MatrixXi(F), V,F,_1);
  }

  return success;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::copyleft::cgal::complex_to_mesh<CGAL::Delaunay_triangulation_3<CGAL::Robust_circumcenter_traits_3<CGAL::Epick>, CGAL::Triangulation_data_structure_3<CGAL::Surface_mesh_vertex_base_3<CGAL::Robust_circumcenter_traits_3<CGAL::Epick>, CGAL::Triangulation_vertex_base_3<CGAL::Robust_circumcenter_traits_3<CGAL::Epick>, CGAL::Triangulation_ds_vertex_base_3<void> > >, CGAL::Delaunay_triangulation_cell_base_with_circumcenter_3<CGAL::Robust_circumcenter_traits_3<CGAL::Epick>, CGAL::Surface_mesh_cell_base_3<CGAL::Robust_circumcenter_traits_3<CGAL::Epick>, CGAL::Triangulation_cell_base_3<CGAL::Robust_circumcenter_traits_3<CGAL::Epick>, CGAL::Triangulation_ds_cell_base_3<void> > > >, CGAL::Sequential_tag>, CGAL::Default, CGAL::Default>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(CGAL::Complex_2_in_triangulation_3<CGAL::Delaunay_triangulation_3<CGAL::Robust_circumcenter_traits_3<CGAL::Epick>, CGAL::Triangulation_data_structure_3<CGAL::Surface_mesh_vertex_base_3<CGAL::Robust_circumcenter_traits_3<CGAL::Epick>, CGAL::Triangulation_vertex_base_3<CGAL::Robust_circumcenter_traits_3<CGAL::Epick>, CGAL::Triangulation_ds_vertex_base_3<void> > >, CGAL::Delaunay_triangulation_cell_base_with_circumcenter_3<CGAL::Robust_circumcenter_traits_3<CGAL::Epick>, CGAL::Surface_mesh_cell_base_3<CGAL::Robust_circumcenter_traits_3<CGAL::Epick>, CGAL::Triangulation_cell_base_3<CGAL::Robust_circumcenter_traits_3<CGAL::Epick>, CGAL::Triangulation_ds_cell_base_3<void> > > >, CGAL::Sequential_tag>, CGAL::Default, CGAL::Default>, void> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
