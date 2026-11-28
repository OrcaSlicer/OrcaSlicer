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
      /// Extract feature edges based on dihedral angle.
      /// Here, dihedral angle is defined as the angle between surface
      /// __normals__ as described in
      /// http://mathworld.wolfram.com/DihedralAngle.html
      ///
      /// Non-manifold and boundary edges are automatically considered as
      /// features.
      ///
      /// @param[in] V   #V by 3 array of vertices.
      /// @param[in] F   #F by 3 array of faces.
      /// @param[in] tol Edges with dihedral angle larger than this are considered
      ///       as features.  Angle is measured in radian.
      /// @param[out] feature_edges: #E by 2 array of edges.  Each edge satisfies at
      ///      least one of the following criteria:
      ///      * Edge has dihedral angle larger than tol.
      ///      * Edge is boundary.
      ///      * Edge is non-manifold (i.e. it has more than 2 adjacent
      ///        faces).
      template <
        typename DerivedV,
        typename DerivedF,
        typename Derivedfeature_edges>
      IGL_INLINE void extract_feature(
            const Eigen::MatrixBase<DerivedV>& V,
            const Eigen::MatrixBase<DerivedF>& F,
            const double tol,
            Eigen::PlainObjectBase<Derivedfeature_edges>& feature_edges);
      // \overload
      // @param[in] uE   #uE by 2 array of undirected edges.
      // @param[in] uE2E #uE list of lists mapping undirected edges to all
      //   corresponding directed edges.
      template <
        typename DerivedV,
        typename DerivedF,
        typename DeriveduE,
        typename Derivedfeature_edges
        >
      IGL_INLINE void extract_feature(
            const Eigen::MatrixBase<DerivedV>& V,
            const Eigen::MatrixBase<DerivedF>& F,
            const double tol,
            const Eigen::MatrixBase<DeriveduE>& uE,
            const std::vector<std::vector<typename DeriveduE::Scalar> >& uE2E,
            Eigen::PlainObjectBase<Derivedfeature_edges>& feature_edges);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "extract_feature.cpp"
#endif
#endif
