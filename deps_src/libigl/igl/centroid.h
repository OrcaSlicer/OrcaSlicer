// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_CENTROID_H
#define IGL_CENTROID_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Computes the centroid and enclosed volume of a closed mesh using a surface integral.
  /// 
  /// @param[in] V  #V by dim list of rest domain positions
  /// @param[in] F  #F by 3 list of triangle indices into V
  /// @param[out]   c  dim vector of centroid coordinates
  /// @param[out]   vol  total volume of solid.
  ///
  template <
    typename DerivedV, 
    typename DerivedF, 
    typename Derivedc, 
    typename Derivedvol>
  IGL_INLINE void centroid(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<Derivedc>& c,
    Derivedvol & vol);
  /// \overload
  template <
    typename DerivedV, 
    typename DerivedF, 
    typename Derivedc>
  IGL_INLINE void centroid(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<Derivedc>& c);

}

#ifndef IGL_STATIC_LIBRARY
#  include "centroid.cpp"
#endif

#endif

