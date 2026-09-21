// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_POINT_MESH_SQUARED_DISTANCE_H
#define IGL_POINT_MESH_SQUARED_DISTANCE_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>

namespace igl
{
  /// Compute distances from a set of points P to a triangle mesh (V,F)
  ///
  /// @param[in] P  #P by 3 list of query point positions
  /// @param[in] V  #V by 3 list of vertex positions
  /// @param[in] Ele  #Ele by (3|2|1) list of (triangle|edge|point) indices
  /// @param[out] sqrD  #P list of smallest squared distances
  /// @param[out] I  #P list of primitive indices corresponding to smallest distances
  /// @param[out] C  #P by 3 list of closest points
  ///
  /// \bug This only computes distances to given primitives. So
  /// unreferenced vertices are ignored. However, degenerate primitives are
  /// handled correctly: triangle [1 2 2] is treated as a segment [1 2], and
  /// triangle [1 1 1] is treated as a point. So one _could_ add extra
  /// combinatorially degenerate rows to Ele for all unreferenced vertices to
  /// also get distances to points.
  ///
  /// ##### Example:
  ///
  /// ```cpp
  /// Eigen::MatrixXd V;
  /// Eigen::MatrixXi F;
  /// igl::read_triangle_mesh("bunny.obj",V,F);
  /// // 100 points in [-1,1]³ cube
  /// Eigen::MatrixXd P = Eigen::MatrixXd::Random(100,3);
  /// Eigen::VectorXd sqrD;
  /// Eigen::VectorXi I;
  /// Eigen::MatrixXd C;
  /// igl::point_mesh_squared_distance(P,V,F,sqrD,I,C);
  /// // Now sqrD(i) = squared distance from P.row(i) to mesh
  /// // I(i) = closest primitive index
  /// // C.row(i) = closest point on mesh to P.row(i)
  /// ```
  template <
    typename DerivedP,
    typename DerivedV,
    typename DerivedEle,
    typename DerivedsqrD,
    typename DerivedI,
    typename DerivedC>
  IGL_INLINE void point_mesh_squared_distance(
    const Eigen::MatrixBase<DerivedP> &P,
    const Eigen::MatrixBase<DerivedV> &V,
    const Eigen::MatrixBase<DerivedEle> &Ele,
    Eigen::PlainObjectBase<DerivedsqrD> &sqrD,
    Eigen::PlainObjectBase<DerivedI> &I,
    Eigen::PlainObjectBase<DerivedC> &C);
}

#ifndef IGL_STATIC_LIBRARY
#  include "point_mesh_squared_distance.cpp"
#endif

#endif
