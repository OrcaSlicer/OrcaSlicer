// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_CONNECT_BOUNDARY_TO_INFINITY_H
#define IGL_CONNECT_BOUNDARY_TO_INFINITY_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Connect all boundary edges to a fictitious point at infinity.
  ///
  /// @param[in] F  #F by 3 list of face indices into some V
  /// @param[out] FO  #F+#O by 3 list of face indices into [V;inf inf inf], original F are
  ///     guaranteed to come first. If (V,F) was a manifold mesh, now it is
  ///     closed with a possibly non-manifold vertex at infinity (but it will be
  ///     edge-manifold).
  template <typename DerivedF, typename DerivedFO>
  IGL_INLINE void connect_boundary_to_infinity(
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedFO> & FO);
  /// Connect all boundary edges to a fictitious point at infinity.
  ///
  /// @param[in] F  #F by 3 list of face indices into some V
  /// @param[in] inf_index  index of point at infinity (usually V.rows() or F.maxCoeff())
  /// @param[out] FO  #F+#O by 3 list of face indices into [V;inf inf inf], original F are
  ///     guaranteed to come first. If (V,F) was a manifold mesh, now it is
  ///     closed with a possibly non-manifold vertex at infinity (but it will be
  ///     edge-manifold).
  template <typename DerivedF, typename DerivedFO>
  IGL_INLINE void connect_boundary_to_infinity(
    const Eigen::MatrixBase<DerivedF> & F,
    const typename DerivedF::Scalar inf_index,
    Eigen::PlainObjectBase<DerivedFO> & FO);
  /// Connect all boundary edges to a fictitious point at infinity.
  ///
  /// @param[in] V  #V by 3 list of vertex positions
  /// @param[in] F  #F by 3 list of face indices into some V
  /// @param[out] VO  #V+1 by 3 list of vertex positions, original V are guaranteed to
  ///     come first. Last point is inf, inf, inf
  /// @param[out] FO  #F+#O by 3 list of face indices into VO
  /// 
  template <
    typename DerivedV, 
    typename DerivedF, 
    typename DerivedVO, 
    typename DerivedFO>
  IGL_INLINE void connect_boundary_to_infinity(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedVO> & VO,
    Eigen::PlainObjectBase<DerivedFO> & FO);
}
#ifndef IGL_STATIC_LIBRARY
#  include "connect_boundary_to_infinity.cpp"
#endif
#endif
