// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_INVERT_DIAG_H
#define IGL_INVERT_DIAG_H
#include "igl_inline.h"
#define EIGEN_YES_I_KNOW_SPARSE_MODULE_IS_NOT_STABLE_YET
#include <Eigen/Sparse>

namespace igl
{
  /// Invert the diagonal entries of a matrix (if the matrix is a diagonal
  /// matrix then this amounts to inverting the matrix)
  ///
  /// @tparam  T  should be a eigen sparse matrix primitive type like int or double
  /// @param[in] X  an m by n sparse matrix
  /// @param[out] Y  an m by n sparse matrix
  template <typename DerivedX, typename MatY>
  IGL_INLINE void invert_diag(
    const Eigen::SparseCompressedBase<DerivedX>& X,
    MatY& Y);
}

#ifndef IGL_STATIC_LIBRARY
#  include "invert_diag.cpp"
#endif

#endif

