// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_COPYLEFT_CGAL_OUTER_FACET_H
#define IGL_COPYLEFT_CGAL_OUTER_FACET_H
#include "../../igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Find a facet that is reachable from infinity without crossing any faces.
      /// Such facet is called "outer facet."
      ///
      /// \pre The input mesh must have all self-intersection resolved.  I.e
      /// there is no duplicated vertices, no overlapping edge and no intersecting
      /// faces (the only exception is there could be topologically duplicated faces).
      /// See cgal::remesh_self_intersections.h for how to obtain such input.
      ///
      /// This function differ from igl::outer_facet() in the fact this
      /// function is more robust because it does not rely on facet normals.
      ///
      /// @param[in] V  #V by 3 list of vertex positions
      /// @param[in] F  #F by 3 list of triangle indices into V
      /// @param[in] I  #I list of facets to consider
      /// @param[in] N  #N by 3 list of face normals
      /// @param[out] f  Index of the outer facet.
      /// @param[out] flipped  true iff the normal of f points inwards.
      template<
          typename DerivedV,
          typename DerivedF,
          typename DerivedN,
          typename DerivedI,
          typename IndexType
          >
      IGL_INLINE void outer_facet(
              const Eigen::MatrixBase<DerivedV> & V,
              const Eigen::MatrixBase<DerivedF> & F,
              const Eigen::MatrixBase<DerivedN> & N,
              const Eigen::MatrixBase<DerivedI> & I,
              IndexType & f,
              bool & flipped);
      /// \overload
      template<
          typename DerivedV,
          typename DerivedF,
          typename DerivedI,
          typename IndexType
          >
      IGL_INLINE void outer_facet(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedF> & F,
        const Eigen::MatrixBase<DerivedI> & I,
        IndexType & f,
        bool & flipped);
    }

  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "outer_facet.cpp"
#endif
#endif
