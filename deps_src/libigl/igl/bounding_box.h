// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_BOUNDING_BOX_H
#define IGL_BOUNDING_BOX_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Build a triangle mesh of the bounding box of a given list of vertices
  /// 
  /// @param[in]  V  #V by dim list of rest domain positions
  /// @param[out] BV  2^dim by dim list of bounding box corners positions
  /// @param[out] BF  #BF by dim list of simplex facets 
  template <typename DerivedV, typename DerivedBV, typename DerivedBF>
  IGL_INLINE void bounding_box(
    const Eigen::MatrixBase<DerivedV>& V,
    Eigen::PlainObjectBase<DerivedBV>& BV,
    Eigen::PlainObjectBase<DerivedBF>& BF);
  /// \overload \brief With padding.
  /// 
  /// @param[in]  pad  padding offset
  template <typename DerivedV, typename DerivedBV, typename DerivedBF>
  IGL_INLINE void bounding_box(
    const Eigen::MatrixBase<DerivedV>& V,
    const typename DerivedV::Scalar pad,
    Eigen::PlainObjectBase<DerivedBV>& BV,
    Eigen::PlainObjectBase<DerivedBF>& BF);
}

#ifndef IGL_STATIC_LIBRARY
#  include "bounding_box.cpp"
#endif

#endif

