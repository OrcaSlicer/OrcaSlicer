// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_COMPLEX_TO_MESH_H
#define IGL_COPYLEFT_CGAL_COMPLEX_TO_MESH_H
#include "../../igl_inline.h"

#include <Eigen/Dense>
#include <CGAL/Complex_2_in_triangulation_3.h>

namespace igl 
{
  namespace copyleft
  {
    namespace cgal
    {
      // Templates:
      //   Tr  CGAL triangulation type, e.g.
      //     CGAL::Surface_mesh_default_triangulation_3
      // Inputs
      //   c2t3  2-complex (surface) living in a 3d triangulation (e.g. result of
      //     CGAL::make_surface_mesh)
      // Outputs:
      //   V  #V by 3 list of vertex positions
      //   F  #F by 3 list of triangle indices
      // Returns true iff conversion was successful, failure can ok if CGAL code
      // can't figure out ordering.
      //
      template <typename Tr, typename DerivedV, typename DerivedF>
      IGL_INLINE bool complex_to_mesh(
        const CGAL::Complex_2_in_triangulation_3<Tr> & c2t3,
        Eigen::PlainObjectBase<DerivedV> & V, 
        Eigen::PlainObjectBase<DerivedF> & F);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "complex_to_mesh.cpp"
#endif

#endif

