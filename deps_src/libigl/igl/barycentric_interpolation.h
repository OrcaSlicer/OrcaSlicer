// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2020 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_BARYCENTRIC_INTERPOLATION_H
#define IGL_BARYCENTRIC_INTERPOLATION_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Interpolate data on a triangle mesh using barycentric coordinates 
  ///
  /// @param[in] D  #D by dim list of per-vertex data
  /// @param[in] F  #F by 3 list of triangle indices
  /// @param[in] B  #X by 3 list of barycentric corodinates
  /// @param[in] I  #X list of triangle indices
  /// @param[out] X  #X by dim list of interpolated data
  template <
    typename DerivedD,
    typename DerivedF,
    typename DerivedB,
    typename DerivedI,
    typename DerivedX>
  IGL_INLINE void barycentric_interpolation(
    const Eigen::MatrixBase<DerivedD> & D,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedB> & B,
    const Eigen::MatrixBase<DerivedI> & I,
    Eigen::PlainObjectBase<DerivedX> & X);
}

#ifndef IGL_STATIC_LIBRARY
#  include "barycentric_interpolation.cpp"
#endif

#endif

