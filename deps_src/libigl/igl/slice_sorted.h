// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2019 Jérémie Dumas <jeremie.dumas@ens-lyon.org>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_SLICE_SORTED_H
#define IGL_SLICE_SORTED_H

#include "igl_inline.h"
#include <Eigen/Dense>
#include <Eigen/Sparse>

namespace igl
{
  /// Act like the matlab X(row_indices,col_indices) operator, where row_indices,
  /// col_indices are non-negative integer indices. This version is about 2x faster
  /// than igl::slice, but it assumes that the indices to slice with are already sorted.
  ///
  /// @param[in] X  m by n matrix
  /// @param[in] R  list of row indices
  /// @param[in] C  list of column indices
  /// @param[out] Y  #R by #C matrix
  ///
  /// \see slice
  template <typename TX, typename TY, typename DerivedR, typename DerivedC>
  IGL_INLINE void slice_sorted(
    const Eigen::SparseMatrix<TX> &X,
    const Eigen::DenseBase<DerivedR> &R,
    const Eigen::DenseBase<DerivedC> &C,
    Eigen::SparseMatrix<TY> &Y);
} 

#ifndef IGL_STATIC_LIBRARY
#include "slice_sorted.cpp"
#endif

#endif
