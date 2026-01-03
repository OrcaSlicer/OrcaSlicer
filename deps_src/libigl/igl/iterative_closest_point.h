// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2019 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ITERATIVE_CLOSEST_POINT_H
#define IGL_ITERATIVE_CLOSEST_POINT_H
#include "igl_inline.h"
#include <Eigen/Core>
#include "AABB.h"

namespace igl
{
  /// Solve for the rigid transformation that places mesh X onto mesh Y using the
  /// iterative closest point method. In particular, optimize:
  ///
  /// min      ∫_X inf ‖x*R+t - y‖² dx
  /// R∈SO(3)      y∈Y
  /// t∈R³
  ///
  /// Typically optimization strategies include using Gauss Newton
  /// ("point-to-plane" linearization) and stochastic descent (sparse random
  /// sampling each iteration).
  ///
  /// @param[in] VX  #VX by 3 list of mesh X vertices
  /// @param[in] FX  #FX by 3 list of mesh X triangle indices into rows of VX
  /// @param[in] VY  #VY by 3 list of mesh Y vertices
  /// @param[in] FY  #FY by 3 list of mesh Y triangle indices into rows of VY
  /// @param[in] num_samples  number of random samples to use (larger --> more accurate,
  ///     but also more suceptible to sticking to local minimum)
  /// @param[out] R  3x3 rotation matrix so that (VX*R+t,FX) ~~ (VY,FY)
  /// @param[out] t  1x3 translation row vector
  template <
    typename DerivedVX,
    typename DerivedFX,
    typename DerivedVY,
    typename DerivedFY,
    typename DerivedR,
    typename Derivedt
    >
  IGL_INLINE void iterative_closest_point(
    const Eigen::MatrixBase<DerivedVX> & VX,
    const Eigen::MatrixBase<DerivedFX> & FX,
    const Eigen::MatrixBase<DerivedVY> & VY,
    const Eigen::MatrixBase<DerivedFY> & FY,
    const int num_samples,
    const int max_iters,
    Eigen::PlainObjectBase<DerivedR> & R,
    Eigen::PlainObjectBase<Derivedt> & t);
  /// \overload
  /// @param[in] Ytree  precomputed AABB tree for accelerating closest point queries
  /// @param[in] NY  #FY by 3 list of precomputed unit face normals
  template <
    typename DerivedVX,
    typename DerivedFX,
    typename DerivedVY,
    typename DerivedFY,
    typename DerivedNY,
    typename DerivedR,
    typename Derivedt
    >
  IGL_INLINE void iterative_closest_point(
    const Eigen::MatrixBase<DerivedVX> & VX,
    const Eigen::MatrixBase<DerivedFX> & FX,
    const Eigen::MatrixBase<DerivedVY> & VY,
    const Eigen::MatrixBase<DerivedFY> & FY,
    const igl::AABB<DerivedVY,3> & Ytree, 
    const Eigen::MatrixBase<DerivedNY> & NY,
    const int num_samples,
    const int max_iters,
    Eigen::PlainObjectBase<DerivedR> & R,
    Eigen::PlainObjectBase<Derivedt> & t);
}

#ifndef IGL_STATIC_LIBRARY
#  include "iterative_closest_point.cpp"
#endif

#endif
