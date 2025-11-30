// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ALL_H
#define IGL_ALL_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Sparse>
namespace igl
{
  /// Check whether all values are logically true along a dimension.
  ///
  /// \note For Dense matrices use: A.rowwise().all() or A.colwise().all()
  ///
  /// @param[in]  A  m by n sparse matrix
  /// @param[in] dim  dimension along which to check for all (1 or 2)
  /// @param[out] B  n-long vector (if dim == 1) 
  ///   or m-long vector (if dim == 2)
  ///
  template <typename AType, typename DerivedB>
  IGL_INLINE void all(
    const Eigen::SparseMatrix<AType> & A, 
    const int dim,
    Eigen::PlainObjectBase<DerivedB>& B);
}
#ifndef IGL_STATIC_LIBRARY
#  include "all.cpp"
#endif
#endif


