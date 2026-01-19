// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_COPYLEFT_CGAL_EXTRACT_FEATURE_H
#define IGL_COPYLEFT_CGAL_EXTRACT_FEATURE_H
#include "../../igl_inline.h"
#include <Eigen/Core>
#include <vector>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // Extract feature edges based on dihedral angle.
      // Here, dihedral angle is defined as the angle between surface
      // __normals__ as described in
      // http://mathworld.wolfram.com/DihedralAngle.html
      //
      // Non-manifold and boundary edges are automatically considered as
      // features.
      //
      // Inputs:
      //   V   #V by 3 array of vertices.
      //   F   #F by 3 array of faces.
      //   tol Edges with dihedral angle larger than this are considered
      //       as features.  Angle is measured in radian.
      //
      // Output:
      //   feature_edges: #E by 2 array of edges.  Each edge satisfies at
      //      least one of the following criteria:
      //
      //      * Edge has dihedral angle larger than tol.
      //      * Edge is boundary.
      //      * Edge is non-manifold (i.e. it has more than 2 adjacent
      //        faces).
      template <
        typename DerivedV,
        typename DerivedF,
        typename DerivedE>
      IGL_INLINE void extract_feature(
            const Eigen::PlainObjectBase<DerivedV>& V,
            const Eigen::PlainObjectBase<DerivedF>& F,
            const double tol,
            Eigen::PlainObjectBase<DerivedE>& feature_edges);

      // Inputs:
      //   V    #V by 3 array of vertices.
      //   F    #F by 3 array of faces.
      //   tol  Edges with dihedral angle larger than this are considered
      //        as features.  Angle is measured in radian.
      //   E    #E by 2 array of directed edges.
      //   uE   #uE by 2 array of undirected edges.
      //   uE2E #uE list of lists mapping undirected edges to all corresponding
      //        directed edges.
      //
      // Output:
      //   feature_edges: #E by 2 array of edges.  Each edge satisfies at
      //      least one of the following criteria:
      //
      //      * Edge has dihedral angle larger than tol.
      //      * Edge is boundary.
      //      * Edge is non-manifold (i.e. it has more than 2 adjacent
      //        faces).
      template <
        typename DerivedV,
        typename DerivedF,
        typename DerivedE>
      IGL_INLINE void extract_feature(
            const Eigen::PlainObjectBase<DerivedV>& V,
            const Eigen::PlainObjectBase<DerivedF>& F,
            const double tol,
            const Eigen::PlainObjectBase<DerivedE>& E,
            const Eigen::PlainObjectBase<DerivedE>& uE,
            const std::vector<std::vector<typename DerivedE::Scalar> >& uE2E,
            Eigen::PlainObjectBase<DerivedE>& feature_edges);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "extract_feature.cpp"
#endif
#endif
