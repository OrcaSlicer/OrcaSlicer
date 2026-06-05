// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Qingan Zhou <qnzhou@gmail.com>
// Copyright (C) 2021 Alec Jacobson <jacobson@cs.toronto.edu>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_OUTER_VERTEX_H
#define IGL_COPYLEFT_CGAL_OUTER_VERTEX_H
#include "../../igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Find a vertex that is reachable from infinite without crossing any faces.
      /// Such vertex is called "outer vertex."
      ///
      /// \pre The input mesh must have all self-intersection resolved and
      /// no duplicated vertices.  See cgal::remesh_self_intersections.h for how to
      /// obtain such input.
      ///
      /// @param[in] V  #V by 3 list of vertex positions
      /// @param[in] F  #F by 3 list of triangle indices into V
      /// @param[in] I  #I list of facets to consider
      /// @param[out] v_index  index of outer vertex
      /// @param[out] A  #A list of facets incident to the outer vertex
      template <
          typename DerivedV,
          typename DerivedF,
          typename DerivedI,
          typename IndexType,
          typename DerivedA
          >
      IGL_INLINE void outer_vertex(
              const Eigen::MatrixBase<DerivedV> & V,
              const Eigen::MatrixBase<DerivedF> & F,
              const Eigen::MatrixBase<DerivedI> & I,
              IndexType & v_index,
              Eigen::PlainObjectBase<DerivedA> & A);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "outer_vertex.cpp"
#endif
#endif

