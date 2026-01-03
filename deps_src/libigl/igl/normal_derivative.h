// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_NORMAL_DERIVATIVE_H
#define IGL_NORMAL_DERIVATIVE_H
#include "igl_inline.h"

#include <Eigen/Core>
#include <Eigen/Sparse>
namespace igl
{
  /// Computes the directional derivative **normal** to
  /// **all** (half-)edges of a triangle mesh (not just boundary edges). These
  /// are integrated along the edge: they're the per-face constant gradient dot
  /// the rotated edge vector (unit rotated edge vector for direction then
  /// magnitude for integration).
  ///
  /// @param[in] V  #V by dim list of mesh vertex positions
  /// @param[in] F  #F by 3|4 list of triangle|tetrahedron indices into V
  /// @param[out] DD  #F*3|4 by #V sparse matrix representing operator to compute
  ///     directional derivative with respect to each facet of each element.
  template <
    typename DerivedV,
    typename DerivedEle,
    typename Scalar>
  IGL_INLINE void normal_derivative(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedEle> & Ele,
    Eigen::SparseMatrix<Scalar>& DD);
}

#ifndef IGL_STATIC_LIBRARY
#  include "normal_derivative.cpp"
#endif

#endif

