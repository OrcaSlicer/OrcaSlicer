// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_MESH_TO_POLYHEDRON_H
#define IGL_COPYLEFT_CGAL_MESH_TO_POLYHEDRON_H
#include "../../igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Convert a mesh (V,F) to a CGAL Polyhedron
      ///
      /// @tparam Polyhedron  CGAL Polyhedron type (e.g. Polyhedron_3)
      /// @param[in] V  #V by 3 list of vertex positions
      /// @param[in] F  #F by 3 list of triangle indices
      /// @param[out] poly  cgal polyhedron
      /// @return true only if (V,F) can be converted to a valid polyhedron
      /// (i.e. if (V,F) is vertex and edge manifold).
      template <
        typename DerivedV,
        typename DerivedF,
        typename Polyhedron>
      IGL_INLINE bool mesh_to_polyhedron(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedF> & F,
        Polyhedron & poly);
    }
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "mesh_to_polyhedron.cpp"
#endif

#endif
