// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SORTROWS_H
#define IGL_SORTROWS_H
#include "igl_inline.h"

#include <vector>
#include <Eigen/Core>
namespace igl
{
  /// Act like matlab's [Y,I] = sortrows(X)
  ///
  /// @tparam DerivedX derived scalar type, e.g. MatrixXi or MatrixXd
  /// @tparam DerivedI derived integer type, e.g. MatrixXi
  /// @param[in] X  m by n matrix whose entries are to be sorted
  /// @param[in] ascending  sort ascending (true, matlab default) or descending (false)
  /// @param[out] Y  m by n matrix whose entries are sorted (**should not** be same
  ///     reference as X)
  /// @param[out] I  m list of indices so that Y = X(I,:);
  template <typename DerivedX, typename DerivedY,typename DerivedI>
  IGL_INLINE void sortrows(
    const Eigen::DenseBase<DerivedX>& X,
    const bool ascending,
    Eigen::PlainObjectBase<DerivedY>& Y,
    Eigen::PlainObjectBase<DerivedI>& I);
  /// \overload
  template <typename DerivedX, typename DerivedY>
  IGL_INLINE void sortrows(
    const Eigen::DenseBase<DerivedX>& X,
    const bool ascending,
    Eigen::PlainObjectBase<DerivedY>& Y);
}

#ifndef IGL_STATIC_LIBRARY
#  include "sortrows.cpp"
#endif

#endif

