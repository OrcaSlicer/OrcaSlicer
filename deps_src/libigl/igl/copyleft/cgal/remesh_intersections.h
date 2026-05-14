// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//
#ifndef IGL_COPYLEFT_CGAL_REMESH_INTERSECTIONS_H
#define IGL_COPYLEFT_CGAL_REMESH_INTERSECTIONS_H

#include "../../igl_inline.h"
#include <Eigen/Dense>
#include "CGAL_includes.hpp"

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Remesh faces according to results of intersection detection and
      /// construction (e.g. from `igl::copyleft::cgal::intersect_other` or
      /// `igl::copyleft::cgal::SelfIntersectMesh`)
      ///
      /// @param[in] V  #V by 3 list of vertex positions
      /// @param[in] F  #F by 3 list of triangle indices into V
      /// @param[in] T  #F list of cgal triangles
      /// @param[in] offending #offending map taking face indices into F to pairs of order
      ///     of first finding and list of intersection objects from all
      ///     intersections
      /// @param[in] stitch_all  if true, merge all vertices with the same coordinate.
      /// @param[out] VV  #VV by 3 list of vertex positions, if stitch_all = false then
      ///     first #V vertices will always be V
      /// @param[out] FF  #FF by 3 list of triangle indices into V
      /// @param[out] IF  #intersecting face pairs by 2  list of intersecting face pairs,
      ///     indexing F
      /// @param[out] J  #FF list of indices into F denoting birth triangle
      /// @param[out] IM  if stitch_all = true   #VV list from 0 to #VV-1
      ///       elseif stitch_all = false  #VV list of indices into VV of unique vertices.
      ///
      template <
        typename DerivedV,
        typename DerivedF,
        typename Kernel,
        typename DerivedVV,
        typename DerivedFF,
        typename DerivedJ,
        typename DerivedIM>
      IGL_INLINE void remesh_intersections(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedF> & F,
        const std::vector<CGAL::Triangle_3<Kernel> > & T,
        const std::map<
          typename DerivedF::Index,
            std::vector<
            std::pair<typename DerivedF::Index, CGAL::Object> > > & offending,
        bool stitch_all,
        bool slow_and_more_precise_rounding,
        Eigen::PlainObjectBase<DerivedVV> & VV,
        Eigen::PlainObjectBase<DerivedFF> & FF,
        Eigen::PlainObjectBase<DerivedJ> & J,
        Eigen::PlainObjectBase<DerivedIM> & IM);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "remesh_intersections.cpp"
#endif

#endif
