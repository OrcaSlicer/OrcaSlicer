// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_POLYHEDRON_TO_MESH_H
#define IGL_COPYLEFT_CGAL_POLYHEDRON_TO_MESH_H
#include "../../igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // Convert a CGAL Polyhedron to a mesh (V,F)
      //
      // Templates:
      //   Polyhedron  CGAL Polyhedron type (e.g. Polyhedron_3)
      // Inputs:
      //   poly  cgal polyhedron
      // Outputs:
      //   V  #V by 3 list of vertex positions
      //   F  #F by 3 list of triangle indices
      template <
        typename Polyhedron,
        typename DerivedV,
        typename DerivedF>
      IGL_INLINE void polyhedron_to_mesh(
        const Polyhedron & poly,
        Eigen::PlainObjectBase<DerivedV> & V,
        Eigen::PlainObjectBase<DerivedF> & F);
    }
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "polyhedron_to_mesh.cpp"
#endif

#endif
