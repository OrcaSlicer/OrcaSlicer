// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_PROJECTED_DELAUNAY_H
#define IGL_COPYLEFT_CGAL_PROJECTED_DELAUNAY_H
#include "../../igl_inline.h"
#include "CGAL_includes.hpp"
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // Compute 2D delaunay triangulation of a given 3d triangle and a list of
      // intersection objects (points,segments,triangles). CGAL uses an affine
      // projection rather than an isometric projection, so we're not guaranteed
      // that the 2D delaunay triangulation here will be a delaunay triangulation
      // in 3D.
      //
      // Inputs:
      //   A  triangle in 3D
      //   A_objects_3  updated list of intersection objects for A
      // Outputs:
      //   cdt  Contrained delaunay triangulation in projected 2D plane
      template <typename Kernel>
      IGL_INLINE void projected_delaunay(
        const CGAL::Triangle_3<Kernel> & A,
        const std::vector<CGAL::Object> & A_objects_3,
        CGAL::Constrained_triangulation_plus_2<
          CGAL::Constrained_Delaunay_triangulation_2<
            Kernel,
            CGAL::Triangulation_data_structure_2<
              CGAL::Triangulation_vertex_base_2<Kernel>,
              CGAL::Constrained_triangulation_face_base_2<Kernel> >,
            CGAL::Exact_intersections_tag> > & cdt);
    }
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "projected_delaunay.cpp"
#endif
#endif
