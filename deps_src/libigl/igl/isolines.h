// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2023 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.


#ifndef IGL_ISOLINES_H
#define IGL_ISOLINES_H
#include "igl_inline.h"

#include <Eigen/Core>


namespace igl
{
  /// Compute isolines of a scalar field on a triangle mesh.
  ///
  /// Isolines may cross perfectly at vertices. The output should not contain
  /// degenerate segments (so long as the input does not contain degenerate
  /// faces). The output segments are *oriented* so that isolines curl
  /// counter-clockwise around local maxima (i.e., for 2D scalar fields). Unless
  /// an isoline hits a boundary, it should be a closed loop. Isolines may run
  /// perfectly along boundaries. Isolines should appear just "above" constants
  /// regions.
  ///
  /// @param[in] V  #V by dim list of mesh vertex positions
  /// @param[in] F  #F by 3 list of mesh triangle indices into V
  /// @param[in] S  #V by 1 list of per-vertex scalar values
  /// @param[in] vals  #vals by 1 list of values to compute isolines for
  /// @param[out] iV  #iV by dim list of isoline vertex positions
  /// @param[out] iE  #iE by 2 list of edge indices into iV
  /// @param[out] I  #iE by 1 list of indices into vals indicating which value
  ///   each segment belongs to
  ///
  /// \see isolines_intrinsic, edge_crossings
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedS,
    typename Derivedvals,
    typename DerivediV,
    typename DerivediE,
    typename DerivedI>
  void isolines(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedS> & S,
    const Eigen::MatrixBase<Derivedvals> & vals,
    Eigen::PlainObjectBase<DerivediV> & iV,
    Eigen::PlainObjectBase<DerivediE> & iE,
    Eigen::PlainObjectBase<DerivedI> & I);
}

#ifndef IGL_STATIC_LIBRARY
#  include "isolines.cpp"
#endif

#endif
