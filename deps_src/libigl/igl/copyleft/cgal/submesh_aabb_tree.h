// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//
#ifndef IGL_COPYLET_CGAL_SUBMESH_AABB_TREE_H
#define IGL_COPYLET_CGAL_SUBMESH_AABB_TREE_H

#include "../../igl_inline.h"
#include <Eigen/Core>
#include <vector>

#include <CGAL/AABB_tree.h>
#include <CGAL/AABB_traits.h>
#include <CGAL/AABB_triangle_primitive.h>
#include <CGAL/intersections.h>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Build an AABB tree for a submesh indicated by a face selection list I
      /// of a full mesh (V,F)
      ///
      /// @param[in] V  #V by 3 array of vertices.
      /// @param[in] F  #F by 3 array of faces.
      /// @param[in] I  #I list of triangle indices to consider.
      /// @param[out] tree  aabb containing triangles of (V,F(I,:))
      /// @param[out] triangles  #I list of cgal triangles
      /// @param[out] in_I  #F list of whether in submesh
      template<
        typename DerivedV,
        typename DerivedF,
        typename DerivedI,
        typename Kernel>
      IGL_INLINE void submesh_aabb_tree(
        const Eigen::MatrixBase<DerivedV>& V,
        const Eigen::MatrixBase<DerivedF>& F,
        const Eigen::MatrixBase<DerivedI>& I,
        CGAL::AABB_tree<
          CGAL::AABB_traits<
            Kernel, 
            CGAL::AABB_triangle_primitive<
              Kernel, typename std::vector<
                typename Kernel::Triangle_3 >::iterator > > > & tree,
        std::vector<typename Kernel::Triangle_3 > & triangles,
        std::vector<bool> & in_I);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "submesh_aabb_tree.cpp"
#endif

#endif
