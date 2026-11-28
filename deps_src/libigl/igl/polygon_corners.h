// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2020 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_POLYGON_CORNERS_H
#define IGL_POLYGON_CORNERS_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>

namespace igl
{
  /// Convert a list-of-lists polygon mesh faces representation to list of
  /// polygon corners and sizes
  ///
  /// @param[in] P  #P list of lists of vertex indices into rows of some matrix V
  /// @param[out] I  #I vectorized list of polygon corner indices into rows of some matrix V
  /// @param[out] C  #P+1 list of cumulative polygon sizes so that C(i+1)-C(i) = size of
  ///     the ith polygon, and so I(C(i)) through I(C(i+1)-1) are the indices of
  ///     the ith polygon
  ///
  template <
    typename PType, 
    typename DerivedI,
    typename DerivedC>
  IGL_INLINE void polygon_corners(
    const std::vector<std::vector<PType> > & P,
    Eigen::PlainObjectBase<DerivedI> & I,
    Eigen::PlainObjectBase<DerivedC> & C);
  /// \overload
  /// \brief Convert a pure k-gon list of polygon mesh indices to list of
  /// polygon corners and sizes
  ///
  /// @param[in] Q  #Q by k list of polygon indices (ith row is a k-gon, unless Q(i,j) =
  ///     -1 then it's a j-gon)
  template <
    typename DerivedQ, 
    typename DerivedI,
    typename DerivedC>
  IGL_INLINE void polygon_corners(
    const Eigen::MatrixBase<DerivedQ> & Q,
    Eigen::PlainObjectBase<DerivedI> & I,
    Eigen::PlainObjectBase<DerivedC> & C);
}

#ifndef IGL_STATIC_LIBRARY
#  include "polygon_corners.cpp"
#endif

#endif 
