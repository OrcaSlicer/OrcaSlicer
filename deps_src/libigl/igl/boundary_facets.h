// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_BOUNDARY_FACETS_H
#define IGL_BOUNDARY_FACETS_H
#include "igl_inline.h"

#include <Eigen/Dense>

#include <vector>

namespace igl
{
  /// Determine boundary faces (edges) of tetrahedra (triangles) stored in T
  /// (analogous to qptoolbox's `outline` and `boundary_faces`).
  ///
  /// @param[in] T  tetrahedron (triangle) index list, m by 4 (3), where m is the number of tetrahedra
  /// @param[out] F  list of boundary faces, n by 3 (2), where n is the number
  ///   of boundary faces. Faces are oriented so that igl::centroid(V,F,…)
  /// computes the same sign volume as igl::volume(V,T)
  /// @param[out] J  list of indices into T, n by 1
  /// @param[out] K  list of indices revealing across from which vertex is this facet
  ///
  template <
    typename DerivedT, 
    typename DerivedF,
    typename DerivedJ,
    typename DerivedK>
  IGL_INLINE void boundary_facets(
    const Eigen::MatrixBase<DerivedT>& T,
    Eigen::PlainObjectBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedJ>& J,
    Eigen::PlainObjectBase<DerivedK>& K);
  /// Determine boundary faces (edges) of tetrahedra (triangles) stored in T.
  ///
  /// @param[in] T  tetrahedron (triangle) index list, m by 4 (3), where m is the number of tetrahedra
  /// @param[out] F  list of boundary faces, n by 3 (2), where n is the number of boundary faces
  template <typename DerivedT, typename DerivedF>
  IGL_INLINE void boundary_facets(
    const Eigen::MatrixBase<DerivedT>& T,
    Eigen::PlainObjectBase<DerivedF>& F);
  /// Determine boundary faces (edges) of tetrahedra (triangles) stored in T.
  ///
  /// @param[in] T  tetrahedron (triangle) index list, m by 4 (3), where m is the number of tetrahedra
  /// @return list of boundary faces, n by 3 (2), where n is the number of boundary faces
  template <typename DerivedT, typename Ret>
  Ret boundary_facets(
    const Eigen::MatrixBase<DerivedT>& T);
}

#ifndef IGL_STATIC_LIBRARY
#  include "boundary_facets.cpp"
#endif

#endif
