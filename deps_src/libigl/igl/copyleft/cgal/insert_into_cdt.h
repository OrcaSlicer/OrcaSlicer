// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Alec Jacobson
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_INSERT_INTO_CDT_H
#define IGL_COPYLEFT_CGAL_INSERT_INTO_CDT_H
#include "../../igl_inline.h"

#include <CGAL/double.h> // Workaround https://github.com/CGAL/cgal/issues/2182 with CGAL 4.10-1
#include <CGAL/Plane_3.h>
#include <CGAL/Constrained_Delaunay_triangulation_2.h>
#include <CGAL/Constrained_triangulation_plus_2.h>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // Given a current 2D constrained Delaunay triangulation (cdt), insert a
      // 3D "object" (e.g., resulting from intersecting two triangles) into the
      // cdt, by projecting it via the given plane (P) and adding appropriate
      // constraints.
      //
      // Inputs:
      //   obj  CGAL::Object representing a vertex, segment, or (convex)
      //     polygon. All vertices should lie on the plane P. If not, then this
      //     adds the _projection_ of this object to the cdt and that might not
      //     be what you wanted to do.
      //   P  plane obj lies on and upon which the cdt is being performed
      //   cdt  current CDT, see output
      // Outputs:
      //   cdt  CDT updated to contain constraints for the given object
      //
      template <typename Kernel>
      IGL_INLINE void insert_into_cdt(
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
        & cdt);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "insert_into_cdt.cpp"
#endif
#endif
