// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2024 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SMOOTH_CORNER_ADJACENCY_H
#define IGL_SMOOTH_CORNER_ADJACENCY_H

#include <Eigen/Core>
#include "igl_inline.h"

namespace igl
{
  /// Determine the corner-to-face adjacency relationship that can be used for
  /// computing crease-aware per-corner normals.
  ///
  /// @param[in] V  #V by 3 list of mesh vertex positions
  /// @param[in] F  #F by 3 list of triangle mesh indices into rows of V
  /// @param[in] corner_threshold_radians  dihedral angle considered non-smooth (in
  ///     radians)
  /// @param[out] CI  #CI list of face neighbors as indices into rows of F
  /// @param[out] CC  3*#F+1 list of cumulative sizes so that CC(i*3+j+1) - CC(i*3+j) is
  ///     the number of faces considered smoothly incident on corner at F(i,j)
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedCI,
    typename DerivedCC>
  void smooth_corner_adjacency(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const double corner_threshold_radians,
    Eigen::PlainObjectBase<DerivedCI> & CI,
    Eigen::PlainObjectBase<DerivedCC> & CC);
  /// Determine the effective corner-to-face adjacency relationship implied by a
  /// set of indexed vertex positions (FV) and normals (FV) (e.g., those read in
  /// from a .obj file).
  ///
  /// @param[in] FV  #F by 3 list of triangle mesh indices into rows of some V
  /// @param[in] FN  #F by 3 list of triangle mesh indices into rows of some N
  /// @param[out] CI  #CI list of face neighbors as indices into rows of F
  /// @param[out] CC  3*#F+1 list of cumulative sizes so that CC(i*3+j+1) - CC(i*3+j) is
  ///     the number of faces considered smoothly incident on corner at F(i,j)
  template <
    typename DerivedFV,
    typename DerivedFN,
    typename DerivedCI,
    typename DerivedCC>
  void smooth_corner_adjacency(
    const Eigen::MatrixBase<DerivedFV> & FV,
    const Eigen::MatrixBase<DerivedFN> & FN,
    Eigen::PlainObjectBase<DerivedCI> & CI,
    Eigen::PlainObjectBase<DerivedCC> & CC);
}

#ifndef IGL_STATIC_LIBRARY
#  include "smooth_corner_adjacency.cpp"
#endif

#endif
