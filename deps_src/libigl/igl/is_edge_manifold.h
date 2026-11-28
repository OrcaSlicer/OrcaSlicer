// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_IS_EDGE_MANIFOLD_H
#define IGL_IS_EDGE_MANIFOLD_H
#include "igl_inline.h"

#include <Eigen/Core>

namespace igl 
{
  /// Check if the mesh is edge-manifold (every edge is incident one one face
  /// (boundary) or two oppositely oriented faces).
  ///
  /// @param[in] F  #F by 3 list of triangle indices
  /// @return true iff all edges are manifold
  ///
  /// \see is_vertex_manifold
  template <typename DerivedF>
  IGL_INLINE bool is_edge_manifold(
    const Eigen::MatrixBase<DerivedF>& F);
  /// Checks if mesh is edge-manifold and outputs per-edge info.
  ///
  /// @param[in] F  #F by 3 list of triangle indices
  /// @param[out] BF  #F by 3 list of flags revealing if edge opposite
  /// corresponding vertex is non-manifold.
  /// @param[out] E  #E by 2 list of unique edges
  /// @param[out] EMAP  3*#F list of indices of opposite edges in "E"
  /// @param[out] BE  #E list of flags whether edge is non-manifold
  template <
    typename DerivedF, 
    typename DerivedBF,
    typename DerivedE,
    typename DerivedEMAP,
    typename DerivedBE>
  IGL_INLINE bool is_edge_manifold(
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedBF>& BF,
    Eigen::PlainObjectBase<DerivedE>& E,
    Eigen::PlainObjectBase<DerivedEMAP>& EMAP,
    Eigen::PlainObjectBase<DerivedBE>& BE);
  /// \overload
  template <
    typename DerivedF,
    typename DerivedEMAP,
    typename DerivedBF,
    typename DerivedBE>
  IGL_INLINE bool is_edge_manifold(
    const Eigen::MatrixBase<DerivedF>& F,
    const typename DerivedF::Index ne,
    const Eigen::MatrixBase<DerivedEMAP>& EMAP,
    Eigen::PlainObjectBase<DerivedBF>& BF,
    Eigen::PlainObjectBase<DerivedBE>& BE);
}

#ifndef IGL_STATIC_LIBRARY
#  include "is_edge_manifold.cpp"
#endif

#endif
