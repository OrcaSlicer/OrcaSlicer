// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_POINTS_INSIDE_COMPONENTS
#define IGL_COPYLEFT_CGAL_POINTS_INSIDE_COMPONENTS

#include "../../igl_inline.h"
#include <Eigen/Core>
#include <vector>

namespace igl 
{
  namespace copyleft
  {
    namespace cgal {
      /// Determine if queries points P are inside of connected facet component
      /// (V, F, I), where I indicates a subset of facets that forms the
      /// component.
      ///
      /// \pre The input mesh must be a closed, self-intersection free,
      /// non-degenerated surface.  Queries points must be either inside or
      /// outside of the mesh (i.e. not on the surface of the mesh).
      ///
      /// @param[in] V  #V by 3 array of vertex positions.
      /// @param[in] F  #F by 3 array of triangles.
      /// @param[in] I  #I list of triangle indices to consider.
      /// @param[in] P  #P by 3 array of query points.
      /// @param[out] inside  #P list of booleans that is true iff the
      ///   corresponding query point is inside of the mesh.
      template<
        typename DerivedV,
        typename DerivedF,
        typename DerivedI,
        typename DerivedP,
        typename DerivedB>
      IGL_INLINE void points_inside_component(
        const Eigen::MatrixBase<DerivedV>& V,
        const Eigen::MatrixBase<DerivedF>& F,
        const Eigen::MatrixBase<DerivedI>& I,
        const Eigen::MatrixBase<DerivedP>& P,
        Eigen::PlainObjectBase<DerivedB>& inside);
      /// \overload
      template<
        typename DerivedV,
        typename DerivedF,
        typename DerivedP,
        typename DerivedB>
      IGL_INLINE void points_inside_component(
          const Eigen::MatrixBase<DerivedV>& V,
          const Eigen::MatrixBase<DerivedF>& F,
          const Eigen::MatrixBase<DerivedP>& P,
          Eigen::PlainObjectBase<DerivedB>& inside);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#include "points_inside_component.cpp"
#endif
#endif
